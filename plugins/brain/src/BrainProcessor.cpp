#include "BrainProcessor.h"

#include "BrainEditor.h"
#include "MaUi.h"

#include <algorithm>
#include <cmath>

using ma::ui::pl;

namespace
{
constexpr int kUpdateHz = 5;
constexpr double kSmoothingSeconds = 3.0;
constexpr std::uint64_t kSpectraRequestMs = 3000;
constexpr std::uint64_t kAgentSwitchMs = 400; // Agents poll the bus at 10 Hz

juce::String quoted(const juce::String& name)
{
    return pl("„") + name + pl("”");
}

juce::String hzText(double hz)
{
    return hz >= 1000.0 ? juce::String(hz / 1000.0, hz >= 10000.0 ? 0 : 1) + " kHz" : juce::String(juce::roundToInt(hz)) + " Hz";
}

// What usually lives in a band in house and trance, for advice when nothing in the mix does.
juce::String arrangementHint(double hz)
{
    if (hz < 60.0)
        return pl("sub bas albo dłuższy ogon stopy");
    if (hz < 250.0)
        return pl("bas, dół stopy");
    if (hz < 2000.0)
        return pl("pad, akordy, ciało wokalu albo leadu");
    if (hz < 6000.0)
        return pl("wokal, lead, clap, perkusjonalia");
    return pl("hi-hat, shaker, ride, „powietrze” na wokalu");
}

juce::String joinNames(const juce::StringArray& names)
{
    const int shown = juce::jmin(names.size(), 4);
    auto text = juce::StringArray(names.begin(), shown).joinIntoString(", ");
    if (names.size() > shown)
        text += pl(" i ") + juce::String(names.size() - shown) + pl(" innych");
    return text;
}
} // namespace

BrainProcessor::BrainProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "BrainState", createLayout())
{
    peakCeiling = parameters.getRawParameterValue(peakCeilingId);
    for (auto& b : bands)
        b.store(-INFINITY);
    for (auto& b : averageBands)
        b.store(-INFINITY);
    startTimerHz(kUpdateHz);
}

BrainProcessor::~BrainProcessor()
{
    stopTimer();
    if (comparing)
        setComparing(false);
}

juce::AudioProcessorValueTreeState::ParameterLayout BrainProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // Pre-master headroom: highest true peak the mix may reach before mastering (Ozone).
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { peakCeilingId, 1 }, "Pre-master peak ceiling",
        juce::NormalisableRange<float> { -12.0f, 0.0f, 0.5f }, -6.0f,
        juce::AudioParameterFloatAttributes().withLabel("dBTP")));

    return layout;
}

bool BrainProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void BrainProcessor::prepareToPlay(double sampleRate, int)
{
    const int channels = getTotalNumInputChannels();
    loudness.prepare(sampleRate, channels);
    truePeakMeter.prepare(channels);
    spectrum.prepare(sampleRate, ma::kTonalScale);
    hopSeconds = 2048.0 / sampleRate; // half of the analyser's 4096-point FFT
    correlation.prepare(sampleRate);
    stereo.store(channels >= 2);
    outputGain.reset(sampleRate, 0.1);
    outputGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(outputGainDb.load()));
}

void BrainProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numChannels = getTotalNumInputChannels();
    const int numSamples = buffer.getNumSamples();
    for (int c = numChannels; c < getTotalNumOutputChannels(); ++c)
        buffer.clear(c, 0, numSamples);

    if (resetRequested.exchange(false))
    {
        loudness.reset();
        truePeakMeter.reset();
        spectrum.reset();
        correlation.reset();
    }

    // Analysis only: the audio passes through untouched.
    const auto* const* input = buffer.getArrayOfReadPointers();
    loudness.process(input, numChannels, numSamples);
    truePeakMeter.process(input, numChannels, numSamples);
    spectrum.process(input, numChannels, numSamples);
    if (numChannels >= 2)
    {
        correlation.process(input[0], input[1], numSamples);
        correlationLow.store(correlation.lowBand());
        correlationFull.store(correlation.fullBand());
    }

    integrated.store(loudness.integratedLufs());
    shortTerm.store(loudness.shortTermLufs());
    momentary.store(loudness.momentaryLufs());
    truePeak.store(truePeakMeter.truePeakDb());

    const auto recent = spectrum.recentDb();
    const auto average = spectrum.averageDb();
    for (size_t b = 0; b < bands.size(); ++b)
    {
        bands[b].store(recent[b]);
        averageBands[b].store(average[b]);
    }
    analysedSeconds.store(spectrum.framesAnalysed() * hopSeconds);

    // Loudness matching while comparing before/after; measurements above stay unaffected.
    outputGain.setTargetValue(juce::Decibels::decibelsToGain(outputGainDb.load()));
    if (outputGain.isSmoothing())
        outputGain.applyGain(buffer, numSamples);
    else if (std::abs(outputGain.getTargetValue() - 1.0f) > 1e-6f)
        buffer.applyGain(outputGain.getTargetValue());
}

void BrainProcessor::setUseLearnedProfiles(bool shouldUse)
{
    if (useProfiles == shouldUse)
        return;
    useProfiles = shouldUse;
    for (auto& channel : mix.channels)
        channel.spectrumValid = false; // start the smoothing over from the new source
}

void BrainProcessor::timerCallback()
{
    auto* region = session->region();
    if (region == nullptr)
    {
        mix.channels.clear();
        buildReport();
        return;
    }

    const auto now = ma::SharedSession::nowMs();
    region->header.brainHeartbeatMs.store(now);
    if (resetAtMs != 0 && now >= resetAtMs)
    {
        resetAtMs = 0;
        requestReset();
    }
    if (editorOpen && !useProfiles)
        region->header.spectraWantedUntil.store(now + kSpectraRequestMs);

    collectChannels(*region, now);

    std::vector<ma::MaskingChannel> masking(mix.channels.size());
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        const auto& channel = mix.channels[c];
        masking[c].valid = channel.spectrumValid;
        masking[c].priority = channel.data.priority;
        for (int b = 0; b < ma::bus::kBands; ++b)
            masking[c].rawDb[static_cast<size_t>(b)] = channel.heardDb[static_cast<size_t>(b)];
    }
    mix.masking = ma::analyseMasking(masking);
    mix.tonal = {};
    if (currentTarget.valid() && !capturing && !comparing && secondsAnalysed() >= kMinimumSeconds)
        mix.tonal = ma::compareToTarget(averageSpectrum(), currentTarget);
    updateLoudnessMatch(now);
    buildReport();
}

void BrainProcessor::collectChannels(ma::bus::Region& region, std::uint64_t now)
{
    const double alpha = 1.0 - std::exp(-1.0 / (kSmoothingSeconds * kUpdateHz));
    std::vector<MixSnapshot::Channel> next;

    for (const auto& slot : region.slots)
    {
        MixSnapshot::Channel channel;
        channel.owner = slot.owner.load();
        if (channel.owner == 0 || !ma::bus::read(slot, channel.data, now))
            continue;

        channel.data.name[ma::bus::kNameLength - 1] = '\0';
        channel.name = juce::String::fromUTF8(channel.data.name);

        const bool hasSource = useProfiles ? channel.data.hasProfile != 0 : channel.data.liveValid != 0;
        const float* source = useProfiles ? channel.data.profileDb : channel.data.liveDb;
        double loudest = -200.0;
        for (int b = 0; b < ma::bus::kBands; ++b)
            loudest = std::max(loudest, static_cast<double>(source[b]));
        const bool audible = hasSource && loudest > -120.0;

        const auto previous = std::find_if(mix.channels.begin(), mix.channels.end(),
                                           [&](const auto& c) { return c.owner == channel.owner; });
        const bool continuing = previous != mix.channels.end() && previous->spectrumValid;

        for (int b = 0; b < ma::bus::kBands; ++b)
        {
            // The mix hears the channel after its trim.
            const double level = audible ? source[b] + channel.data.trimDb : -200.0;
            auto& smoothed = channel.bandsDb[static_cast<size_t>(b)];
            if (useProfiles || !continuing)
                smoothed = level;
            else
                smoothed = previous->bandsDb[static_cast<size_t>(b)]
                           + alpha * (level - previous->bandsDb[static_cast<size_t>(b)]);
        }
        ma::EqSettings eq {};
        if (channel.data.eqBypass == 0)
            for (int b = 0; b < ma::kEqBands; ++b)
                eq[static_cast<size_t>(b)] = channel.data.eq[b];
        for (int b = 0; b < ma::bus::kBands; ++b)
            channel.heardDb[static_cast<size_t>(b)] = channel.bandsDb[static_cast<size_t>(b)]
                + ma::eqResponseDb(eq, ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b));

        // Keep a channel that just went quiet in the picture while its average fades.
        channel.spectrumValid = audible || (continuing && !useProfiles && *std::max_element(channel.bandsDb.begin(), channel.bandsDb.end()) > -120.0);
        next.push_back(channel);
    }

    std::sort(next.begin(), next.end(), [](const auto& a, const auto& b) {
        if (a.data.priority != b.data.priority)
            return a.data.priority < b.data.priority;
        return a.name.compareNatural(b.name) < 0;
    });
    mix.channels = std::move(next);
}

void BrainProcessor::buildReport()
{
    using Status = MixSnapshot::Status;
    auto& report = mix.report;
    report.clear();

    // Headroom for mastering.
    {
        const double peak = truePeakDb();
        const double ceiling = peakCeilingDb();
        MixSnapshot::ReportItem item;
        item.title = pl("Zapas na mastering");
        if (!std::isfinite(peak))
        {
            item.detail = pl("Czekam na sygnał.");
        }
        else if (peak <= ceiling)
        {
            item.status = Status::good;
            item.detail = pl("Szczyt ") + ma::ui::formatDb(peak, "dBTP") + ", " + juce::String(ceiling - peak, 1)
                          + pl(" dB poniżej limitu.");
        }
        else
        {
            item.status = Status::problem;
            item.detail = pl("Szczyt ") + juce::String(peak - ceiling, 1) + pl(" dB ponad limitem ")
                          + ma::ui::formatDb(ceiling, "dBTP") + pl(". Ścisz kanały (Agent: niższy cel) zamiast sumy.");
        }
        report.push_back(item);
    }

    // Low end in mono: club systems sum it.
    if (stereo.load())
    {
        const double low = lowCorrelation();
        MixSnapshot::ReportItem item;
        item.title = pl("Bas w mono (poniżej 120 Hz)");
        item.status = low >= 0.7 ? Status::good : low >= 0.0 ? Status::warning : Status::problem;
        item.detail = pl("Korelacja ") + juce::String(low, 2)
                      + (low >= 0.7   ? pl(". Dół zagra tak samo w klubie i w mono.")
                         : low >= 0.0 ? pl(". Dół jest szeroki: w mono straci moc. Zawęź stereo basu i stopy.")
                                      : pl(". Dół jest w przeciwfazie: w mono zniknie. Sprawdź fazę i stereo na basie."));
        report.push_back(item);

        const double full = fullCorrelation();
        MixSnapshot::ReportItem phase;
        phase.title = pl("Faza całego miksu");
        phase.status = full >= 0.2 ? Status::good : full >= 0.0 ? Status::warning : Status::problem;
        phase.detail = pl("Korelacja ") + juce::String(full, 2)
                       + (full >= 0.2   ? pl(". Zgodność z mono w porządku.")
                          : full >= 0.0 ? pl(". Bardzo szeroko: część elementów zniknie w mono.")
                                        : pl(". Przeciwfaza: miks w mono się rozpadnie."));
        report.push_back(phase);
    }

    // Agents connected.
    {
        MixSnapshot::ReportItem item;
        item.title = pl("Agenci");
        if (session->region() == nullptr)
        {
            item.status = Status::problem;
            item.detail = pl("Nie udało się otworzyć kanału danych (plik w folderze TEMP).");
        }
        else if (mix.channels.empty())
        {
            item.status = Status::warning;
            item.detail = pl("Żaden Agent nie jest podłączony. Wstaw Agenta na kanały miksera.");
        }
        else
        {
            item.status = Status::good;
            item.detail = pl("Podłączonych: ") + juce::String(static_cast<int>(mix.channels.size())) + ".";
        }
        report.push_back(item);
    }

    // Gain staging of the channels.
    if (!mix.channels.empty())
    {
        juce::StringArray notLearned, limited, hot;
        for (const auto& channel : mix.channels)
        {
            if (channel.data.hasProfile == 0)
                notLearned.add(channel.name);
            if (channel.data.trimLimited != 0)
                limited.add(channel.name);
            if (channel.data.outputPeakDb > 0.0f)
                hot.add(channel.name);
        }

        MixSnapshot::ReportItem item;
        item.title = pl("Gain staging kanałów");
        if (!hot.isEmpty())
        {
            item.status = Status::problem;
            item.detail = pl("Ponad 0 dBFS na wyjściu: ") + joinNames(hot) + pl(". Naucz poziom albo obniż trim.");
        }
        else if (!limited.isEmpty() || !notLearned.isEmpty())
        {
            item.status = Status::warning;
            if (!notLearned.isEmpty())
                item.detail = pl("Bez nauki poziomu: ") + joinNames(notLearned) + ".";
            if (!limited.isEmpty())
                item.detail << (item.detail.isEmpty() ? "" : "\n") << pl("Trim ograniczony szczytami: ")
                            << joinNames(limited) << pl(". Rozważ kompresję przed Agentem.");
        }
        else
        {
            item.status = Status::good;
            item.detail = pl("Wszystkie kanały nauczone, bez przesterowań.");
        }
        report.push_back(item);
    }

    // Tonal balance against the chosen target.
    {
        MixSnapshot::ReportItem item;
        item.title = pl("Balans tonalny");
        if (!currentTarget.valid())
        {
            item.detail = currentTarget.name.empty()
                              ? pl("Wybierz albo utwórz cel i nagraj kilka referencji (widok „Balans tonalny”).")
                              : pl("Cel nie ma jeszcze referencji. Nagraj co najmniej jedną.");
        }
        else if (capturing)
        {
            item.detail = pl("Nagrywam referencję...");
        }
        else if (!mix.tonal.valid)
        {
            item.detail = pl("Słucham miksu: potrzeba ") + juce::String(juce::roundToInt(kMinimumSeconds)) + pl(" s.");
        }
        else if (mix.tonal.regions.empty())
        {
            item.status = Status::good;
            item.detail = pl("Cały miks mieści się w korytarzu celu ") + quoted(juce::String::fromUTF8(currentTarget.name.c_str())) + ".";
        }
        else
        {
            double worst = 0.0;
            juce::StringArray parts;
            for (const auto& region : mix.tonal.regions)
            {
                worst = std::max(worst, std::abs(region.deviationDb));
                if (parts.size() < 3)
                    parts.add((region.hole() ? pl("brakuje ") : pl("za dużo ")) + hzText(region.lowHz) + pl("–") + hzText(region.highHz));
            }
            item.status = worst >= 3.0 ? Status::problem : Status::warning;
            item.detail = parts.joinIntoString(", ") + pl(". „Zaproponuj” podpowie, co zrobić.");
        }
        report.push_back(item);
    }

    // Collisions.
    {
        int valid = 0;
        for (const auto& channel : mix.channels)
            valid += channel.spectrumValid ? 1 : 0;

        MixSnapshot::ReportItem item;
        item.title = pl("Kolizje częstotliwości");
        if (valid < 2)
        {
            item.detail = useProfiles ? pl("Potrzeba co najmniej dwóch kanałów z nauczonym profilem.")
                                      : pl("Puść utwór: potrzeba co najmniej dwóch grających kanałów.");
        }
        else if (mix.masking.issues.empty())
        {
            item.status = Status::good;
            item.detail = pl("Ważniejsze kanały nie są zasłaniane.");
        }
        else
        {
            const auto count = static_cast<int>(mix.masking.issues.size());
            item.status = mix.masking.issues.front().score >= 0.5 ? Status::problem : Status::warning;
            item.detail = juce::String(count) + (count == 1 ? pl(" kolizja") : count < 5 ? pl(" kolizje") : pl(" kolizji"))
                          + pl(". Szczegóły w liście poniżej mapy.");
        }
        report.push_back(item);
    }
}

juce::String BrainProcessor::describeIssue(const ma::MaskingIssue& issue) const
{
    const auto& masker = mix.channels[static_cast<size_t>(issue.masker)];
    const auto& masked = mix.channels[static_cast<size_t>(issue.masked)];
    return quoted(masker.name) + pl(" zasłania ") + quoted(masked.name) + pl(" w ") + hzText(issue.lowHz) + pl("–")
           + hzText(issue.highHz) + " (" + juce::String(juce::roundToInt(issue.score * 100.0)) + "%)";
}

void BrainProcessor::propose()
{
    currentProposal = {};
    currentProposal.made = true;

    std::vector<ma::SolverChannel> channels(mix.channels.size());
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        channels[c].valid = mix.channels[c].spectrumValid;
        channels[c].priority = mix.channels[c].data.priority;
        for (int b = 0; b < ma::bus::kBands; ++b)
            channels[c].rawDb[static_cast<size_t>(b)] = mix.channels[c].bandsDb[static_cast<size_t>(b)];
    }

    ma::SolverOptions options;
    const double peak = truePeakDb();
    const double over = std::isfinite(peak) ? peak - peakCeilingDb() : 0.0;
    if (over > 0.0)
        options.headroomGainDb = -std::ceil((over + 0.5) * 2.0) / 2.0;

    if (mix.tonal.valid)
        options.tonal = &mix.tonal;
    const auto solved = ma::solve(channels, options);
    currentProposal.crowdingBefore = solved.crowdingBefore;
    currentProposal.crowdingAfter = solved.crowdingAfter;
    for (size_t c = 0; c < mix.channels.size(); ++c)
        currentProposal.eq.emplace_back(mix.channels[c].owner, solved.eq[c]);

    const auto roles = ma::ui::roleNames();
    for (const auto& change : solved.changes)
    {
        Proposal::Item item;
        item.change = change;
        const auto targetName = quoted(juce::String::fromUTF8(currentTarget.name.c_str()));
        const auto range = hzText(change.lowHz) + pl("–") + hzText(change.highHz);
        if (change.kind == ma::SolverChange::Kind::globalGain)
        {
            item.text = pl("Wszystkie kanały: ") + juce::String(change.gainDb, 1) + pl(" dB, bo szczyt sumy jest ")
                        + juce::String(over, 1) + pl(" dB ponad limitem pre-master");
        }
        else if (change.kind == ma::SolverChange::Kind::tonalAdvice)
        {
            item.advisory = true;
            item.selected = false;
            item.text = pl("Dziura ") + range + pl(": żaden kanał tam nie gra. ");
            if (change.sourceChannel >= 0)
                item.text += pl("Saturacja albo exciter na ") + quoted(mix.channels[static_cast<size_t>(change.sourceChannel)].name)
                             + pl(" doda harmoniczne w tym paśmie, albo ");
            item.text += pl("dodaj element: ") + arrangementHint(std::sqrt(change.lowHz * change.highHz));
        }
        else if (change.kind == ma::SolverChange::Kind::tonalBoost || change.kind == ma::SolverChange::Kind::tonalCut)
        {
            const auto& channel = mix.channels[static_cast<size_t>(change.channel)];
            item.owner = channel.owner;
            item.band = solved.eq[static_cast<size_t>(change.channel)][static_cast<size_t>(change.eqSlot)];
            const bool boost = change.kind == ma::SolverChange::Kind::tonalBoost;
            item.text = quoted(channel.name) + ": " + (boost ? "+" : "") + juce::String(change.gainDb, 1) + pl(" dB przy ")
                        + hzText(item.band.hz) + " (Q " + juce::String(item.band.q, 1) + pl("), bo w ") + range
                        + (boost ? pl(" brakuje ") : pl(" jest ")) + juce::String(std::abs(change.deviationDb), 1)
                        + (boost ? pl(" dB do celu ") : pl(" dB za dużo względem celu ")) + targetName;
        }
        else
        {
            const auto& masker = mix.channels[static_cast<size_t>(change.channel)];
            const auto& masked = mix.channels[static_cast<size_t>(change.maskedChannel)];
            item.owner = masker.owner;
            item.maskedOwner = masked.owner;
            item.band = solved.eq[static_cast<size_t>(change.channel)][static_cast<size_t>(change.eqSlot)];
            item.text = quoted(masker.name) + ": " + juce::String(item.band.gainDb, 1) + pl(" dB przy ") + hzText(item.band.hz)
                        + " (Q " + juce::String(item.band.q, 1) + pl("), bo zasłania ") + quoted(masked.name) + pl(" w ")
                        + hzText(change.lowHz) + pl("–") + hzText(change.highHz) + " ("
                        + juce::String(juce::roundToInt(change.scoreBefore * 100.0)) + pl("% → ")
                        + juce::String(juce::roundToInt(change.scoreAfter * 100.0)) + "%)";
            // Stopa and bas: a static cut is a first step; the dynamic one comes in phase 4.
            if (masker.data.role == 2 && masked.data.role == 1)
                item.text += pl(". Docelowo dynamicznie (faza 4)");
        }
        currentProposal.items.push_back(item);
    }
}

void BrainProcessor::toggleProposalItem(int index)
{
    if (index < 0 || index >= static_cast<int>(currentProposal.items.size()))
        return;
    auto& item = currentProposal.items[static_cast<size_t>(index)];
    if (!item.advisory)
        item.selected = !item.selected;
}

ma::Curve BrainProcessor::averageSpectrum() const
{
    ma::Curve out {};
    for (size_t b = 0; b < out.size(); ++b)
        out[b] = averageBands[b].load();
    return out;
}

juce::File BrainProcessor::targetFolder()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("MixAssistant")
        .getChildFile("targets");
}

juce::StringArray BrainProcessor::targetNames() const
{
    juce::StringArray names;
    for (const auto& file : targetFolder().findChildFiles(juce::File::findFiles, false, "*.matarget"))
    {
        ma::TonalTarget t;
        if (ma::parseTarget(file.loadFileAsString().toStdString(), t))
            names.add(juce::String::fromUTF8(t.name.c_str()));
    }
    names.sortNatural();
    return names;
}

bool BrainProcessor::selectTarget(const juce::String& name)
{
    for (const auto& file : targetFolder().findChildFiles(juce::File::findFiles, false, "*.matarget"))
    {
        ma::TonalTarget t;
        if (ma::parseTarget(file.loadFileAsString().toStdString(), t) && juce::String::fromUTF8(t.name.c_str()) == name)
        {
            currentTarget = t;
            parameters.state.setProperty("target", name, nullptr);
            return true;
        }
    }
    return false;
}

bool BrainProcessor::createTarget(const juce::String& name)
{
    const auto trimmed = name.trim();
    if (trimmed.isEmpty() || targetNames().contains(trimmed))
        return false;
    currentTarget = {};
    currentTarget.name = trimmed.toStdString();
    parameters.state.setProperty("target", trimmed, nullptr);
    saveTarget();
    return true;
}

void BrainProcessor::saveTarget()
{
    if (currentTarget.name.empty())
        return;
    const auto folder = targetFolder();
    folder.createDirectory();
    const auto fileName = juce::File::createLegalFileName(juce::String::fromUTF8(currentTarget.name.c_str())) + ".matarget";
    folder.getChildFile(fileName).replaceWithText(ma::serialiseTarget(currentTarget), false, false, "\n");
}

void BrainProcessor::removeLastReference()
{
    if (currentTarget.references.empty())
        return;
    currentTarget.references.pop_back();
    currentTarget.rebuild();
    saveTarget();
}

void BrainProcessor::startCapture()
{
    if (comparing)
        setComparing(false);
    capturing = true;
    requestReset();
}

bool BrainProcessor::finishCapture()
{
    if (!capturing || currentTarget.name.empty() || secondsAnalysed() < kMinimumSeconds)
        return false;
    capturing = false;
    currentTarget.references.push_back(ma::normaliseCurve(averageSpectrum()));
    currentTarget.rebuild();
    saveTarget();
    requestReset(); // the mix is measured from scratch again
    return true;
}

void BrainProcessor::cancelCapture()
{
    capturing = false;
    requestReset();
}

void BrainProcessor::sendCommand(std::uint64_t owner, const ma::bus::Command& command)
{
    auto* region = session->region();
    if (region == nullptr)
        return;
    for (auto& slot : region->slots)
        if (slot.owner.load() == owner)
            ma::bus::sendCommand(slot, command);
}

int BrainProcessor::applySelected()
{
    if (!currentProposal.made)
        return 0;
    if (comparing)
        setComparing(false);

    double globalGain = 0.0;
    for (const auto& item : currentProposal.items)
        if (item.selected && item.change.kind == ma::SolverChange::Kind::globalGain)
            globalGain += item.change.gainDb;

    int sent = 0;
    for (const auto& channel : mix.channels)
    {
        const auto proposed = std::find_if(currentProposal.eq.begin(), currentProposal.eq.end(),
                                           [&](const auto& e) { return e.first == channel.owner; });

        // The proposal's bands this channel's selected items refer to; the rest switch off.
        ma::EqSettings eq {};
        bool eqChanges = false;
        for (const auto& item : currentProposal.items)
        {
            const bool eqKind = item.change.kind == ma::SolverChange::Kind::eqCut
                                || item.change.kind == ma::SolverChange::Kind::tonalBoost
                                || item.change.kind == ma::SolverChange::Kind::tonalCut;
            if (!item.selected || !eqKind || item.owner != channel.owner)
                continue;
            if (proposed != currentProposal.eq.end())
                eq[static_cast<size_t>(item.change.eqSlot)] = proposed->second[static_cast<size_t>(item.change.eqSlot)];
            eqChanges = true;
        }
        if (!eqChanges && std::abs(globalGain) < 0.05)
            continue;
        if (!eqChanges)
            for (int b = 0; b < ma::kEqBands; ++b)
                eq[static_cast<size_t>(b)] = channel.data.eq[b]; // keep what is there

        ma::bus::Command command {};
        command.target = channel.owner;
        command.serial = ++commandSerial;
        command.kind = ma::bus::CommandKind::apply;
        command.trimDb = static_cast<float>(juce::jlimit(-24.0, 24.0, channel.data.trimDb + globalGain));
        for (int b = 0; b < ma::kEqBands; ++b)
            command.eq[b] = eq[static_cast<size_t>(b)];
        sendCommand(channel.owner, command);
        ++sent;
    }

    if (sent > 0)
    {
        currentProposal = {};
        loudnessBefore = loudnessAfter = -INFINITY;
        resetAtMs = ma::SharedSession::nowMs() + kAgentSwitchMs; // measure the mix again from there
    }
    return sent;
}

bool BrainProcessor::canUndo() const
{
    return std::any_of(mix.channels.begin(), mix.channels.end(), [](const auto& c) { return c.data.hasBackup != 0; });
}

int BrainProcessor::undoLast()
{
    if (comparing)
        setComparing(false);
    int sent = 0;
    for (const auto& channel : mix.channels)
    {
        if (channel.data.hasBackup == 0)
            continue;
        ma::bus::Command command {};
        command.target = channel.owner;
        command.serial = ++commandSerial;
        command.kind = ma::bus::CommandKind::undo;
        sendCommand(channel.owner, command);
        ++sent;
    }
    if (sent > 0)
    {
        loudnessBefore = loudnessAfter = -INFINITY;
        resetAtMs = ma::SharedSession::nowMs() + kAgentSwitchMs;
    }
    return sent;
}

void BrainProcessor::setComparing(bool shouldCompare)
{
    comparing = shouldCompare;
    compareSwitchedMs = ma::SharedSession::nowMs();
    if (auto* region = session->region())
        region->header.compareBefore.store(shouldCompare ? 1u : 0u);
    outputGainDb.store(0.0f);
    // Peaks and loudness of one version must not leak into the other's report.
    resetAtMs = compareSwitchedMs + kAgentSwitchMs;
}

double BrainProcessor::compareGainDb() const
{
    if (!std::isfinite(loudnessBefore) || !std::isfinite(loudnessAfter))
        return std::nan("");
    return juce::jlimit(-12.0, 12.0, loudnessAfter - loudnessBefore);
}

void BrainProcessor::updateLoudnessMatch(std::uint64_t now)
{
    // Short-term loudness covers 3 s; wait until it only holds the current version.
    const double shortTermNow = shortTermLufs();
    if (now - compareSwitchedMs > 3500 + kAgentSwitchMs && std::isfinite(shortTermNow) && shortTermNow > -60.0)
        (comparing ? loudnessBefore : loudnessAfter) = shortTermNow;

    const double gain = compareGainDb();
    outputGainDb.store(comparing && std::isfinite(gain) ? static_cast<float>(gain) : 0.0f);
}

void BrainProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    if (auto xml = parameters.copyState().createXml())
        copyXmlToBinary(*xml, destData);
}

void BrainProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        if (xml->hasTagName(parameters.state.getType()))
        {
            parameters.replaceState(juce::ValueTree::fromXml(*xml));
            const auto name = parameters.state.getProperty("target").toString();
            if (name.isNotEmpty())
                selectTarget(name);
        }
}

juce::AudioProcessorEditor* BrainProcessor::createEditor()
{
    return new BrainEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BrainProcessor();
}
