#include "BrainProcessor.h"

#include "BrainEditor.h"
#include "MaUi.h"
#include "ma/Instrument.h"

#include <algorithm>
#include <functional>
#include <cmath>

using ma::ui::pl;

namespace
{
constexpr int kUpdateHz = 5;
constexpr double kSmoothingSeconds = 3.0;
constexpr std::uint64_t kSpectraRequestMs = 3000;
constexpr std::uint64_t kAgentSwitchMs = 400; // Agents poll the bus at 10 Hz
constexpr double kCancellingCorrelation = -0.2;  // low-band correlation with the sidechain that loses low end
constexpr double kDuckMaxHz = 300.0;             // collisions below this are timing problems (kick vs bass)
constexpr int kDuckSync = 2;                     // release in a sixteenth: back before the next off-beat
constexpr double kPhoneGoalDb = -12.0;           // a channel this much quieter (vs the mix) on a phone ...
constexpr double kLaptopGoalDb = -8.0;           // ... or a laptop counts as lost there
constexpr double kPhoneAimDb = -6.0;             // what the proposals aim for: still clearly there ...
constexpr double kLaptopAimDb = -4.0;            // ... on a phone and on a laptop
constexpr double kOnsetShare = 0.5;              // share of a percussive channel's energy in its onsets
constexpr float kMaxHarmonicsDb = -3.0f;
constexpr float kMaxPercussiveHarmonicsDb = -8.0f;
constexpr float kMaxAttackDb = 6.0f;
constexpr std::uint64_t kAdaptEveryMs = 1000;   // adaptive mode: one small step a second ...
constexpr double kAdaptStepDb = 0.25;           // ... of at most this much per band
constexpr double kAdaptDeadbandDb = 0.3;        // closer than this to the answer counts as there
constexpr int kAdaptReleaseRounds = 5;          // a band is let go only after the solver drops it this many times in a row ...
constexpr int kAdaptEnterRounds = 3;            // ... and a new one starts after it asked for it this many times
constexpr double kAdaptMinimumLufs = -40.0;     // quieter than this (momentary) is a break, not the mix ...
constexpr std::uint64_t kAdaptSettleMs = 4000;  // ... and the averaged spectra need this long after one to settle

juce::String quoted(const juce::String& name)
{
    return pl("„") + name + pl("”");
}

juce::String hzText(double hz)
{
    // juce::String(x, 0) prints every digit, so whole kHz go through roundToInt.
    if (hz >= 10000.0)
        return juce::String(juce::roundToInt(hz / 1000.0)) + " kHz";
    return hz >= 1000.0 ? juce::String(hz / 1000.0, 1) + " kHz" : juce::String(juce::roundToInt(hz)) + " Hz";
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
    adaptive = parameters.getRawParameterValue(adaptiveId);
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

juce::StringArray BrainProcessor::systemNames()
{
    return { pl("Pełne pasmo"), pl("Telefon"), pl("Laptop"), pl("Słuchawki douszne"), pl("Samochód"), pl("Klub (mono)") };
}

juce::AudioProcessorValueTreeState::ParameterLayout BrainProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // Pre-master headroom: highest true peak the mix may reach before mastering (Ozone).
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { peakCeilingId, 1 }, "Pre-master peak ceiling",
        juce::NormalisableRange<float> { -12.0f, 0.0f, 0.5f }, -6.0f,
        juce::AudioParameterFloatAttributes().withLabel("dBTP")));
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID { adaptiveId, 2 }, "Adaptive mode", false));

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
    simulator.prepare(sampleRate);
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

    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
            if (const auto bpm = position->getBpm())
                hostBpm.store(*bpm);

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

    // Listening through a simulated system; the measurements above still see the real mix.
    simulator.setSystem(static_cast<ma::PlaybackSystem>(previewSystem.load()));
    simulator.process(buffer.getArrayOfWritePointers(), numChannels, numSamples);

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

    std::vector<ma::TranslationChannel> translation(mix.channels.size());
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        const auto& channel = mix.channels[c];
        translation[c].valid = channel.outputValid || channel.spectrumValid;
        for (int b = 0; b < ma::bus::kBands; ++b)
            translation[c].rawDb[static_cast<size_t>(b)] = channel.outputValid ? channel.outputDb[static_cast<size_t>(b)]
                                                                               : channel.heardDb[static_cast<size_t>(b)];
    }
    mix.translation = ma::analyseTranslation(translation);
    updateLoudnessMatch(now);
    adaptStep(now);
    buildReport();
}

ma::SolverResult BrainProcessor::solveNow(double headroomGainDb) const
{
    std::vector<ma::SolverChannel> channels(mix.channels.size());
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        channels[c].valid = mix.channels[c].spectrumValid;
        channels[c].priority = mix.channels[c].data.priority;
        for (int b = 0; b < ma::bus::kBands; ++b)
            channels[c].rawDb[static_cast<size_t>(b)] = mix.channels[c].bandsDb[static_cast<size_t>(b)];
    }
    ma::SolverOptions options;
    options.headroomGainDb = headroomGainDb;
    if (mix.tonal.valid)
        options.tonal = &mix.tonal;
    return ma::solve(channels, options);
}

void BrainProcessor::adaptStep(std::uint64_t now)
{
    adaptiveState.running = false;
    adaptiveState.waitingFor = {};
    if (!isAdaptive())
    {
        adaptVotes.clear();
        return;
    }
    int playing = 0;
    for (const auto& channel : mix.channels)
        playing += channel.spectrumValid ? 1 : 0;
    if (useProfiles)
        adaptiveState.waitingFor = pl("działa na tym, co gra, nie na nauczonych profilach");
    else if (comparing || capturing)
        adaptiveState.waitingFor = comparing ? pl("trwa porównanie przed / po") : pl("trwa nagrywanie referencji");
    else if (playing < 2)
        adaptiveState.waitingFor = pl("potrzeba co najmniej dwóch grających kanałów");
    // A break (or its first seconds back) must not move anything: the spectra are still fading.
    const bool playingNow = std::isfinite(momentaryLufs()) && momentaryLufs() >= kAdaptMinimumLufs;
    playingSinceMs = playingNow ? (playingSinceMs == 0 ? now : playingSinceMs) : 0;
    if (adaptiveState.waitingFor.isEmpty() && (playingSinceMs == 0 || now - playingSinceMs < kAdaptSettleMs))
        adaptiveState.waitingFor = pl("czeka na muzykę (przerwy nie zmieniają ustawień)");
    if (adaptiveState.waitingFor.isNotEmpty())
        return;
    adaptiveState.running = true;
    if (now - lastAdaptMs < kAdaptEveryMs)
        return;
    lastAdaptMs = now;

    // The solver's full answer from the pre-EQ spectra; each band then moves a small step towards it.
    auto solved = solveNow(0.0);
    // A low collision the masker already gives way to dynamically (kick and bass) needs no static cut too.
    for (const auto& change : solved.changes)
    {
        if (change.kind != ma::SolverChange::Kind::eqCut || change.highHz > kDuckMaxHz)
            continue;
        const auto& masker = mix.channels[static_cast<size_t>(change.channel)];
        if (masker.data.duck.on > 0.5f)
            solved.eq[static_cast<size_t>(change.channel)][static_cast<size_t>(change.eqSlot)] = {};
    }
    int moved = 0;
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        const auto& channel = mix.channels[c];
        if (!channel.spectrumValid || channel.data.eqBypass != 0)
            continue;
        auto& votes = adaptVotes[channel.owner];
        ma::EqSettings next {};
        bool changed = false;
        for (int b = 0; b < ma::kEqBands; ++b)
        {
            const auto slot = static_cast<size_t>(b);
            const auto current = channel.data.eq[b];
            auto target = solved.eq[c][slot];
            next[slot] = current;

            // Letting a band go needs the solver to agree several times; it may only be wavering.
            const bool same = ma::isOn(current) && ma::isOn(target) && std::abs(std::log2(current.hz / target.hz)) < 1.0 / 3.0;
            if (ma::isOn(current) && !same)
            {
                if (++votes.release[slot] < kAdaptReleaseRounds)
                    continue;
            }
            else
            {
                votes.release[slot] = 0;
            }
            if (!ma::isOn(current) && ma::isOn(target))
            {
                if (++votes.enter[slot] < kAdaptEnterRounds)
                    continue;
            }
            else
            {
                votes.enter[slot] = 0;
            }

            if (same || !ma::isOn(current))
            {
                if (!ma::isOn(target))
                    continue;
                // Same band (or a free slot): follow the answer's frequency and Q, step the gain.
                const double gap = target.gainDb - (ma::isOn(current) ? current.gainDb : 0.0);
                if (ma::isOn(current) && std::abs(gap) < kAdaptDeadbandDb)
                    continue;
                const double from = ma::isOn(current) ? current.gainDb : 0.0;
                next[slot] = { target.hz, static_cast<float>(from + juce::jlimit(-kAdaptStepDb, kAdaptStepDb, gap)), target.q };
            }
            else
            {
                // A band the answer no longer has (or has elsewhere): fade it out first.
                const double gain = current.gainDb;
                next[slot].gainDb = std::abs(gain) <= kAdaptStepDb ? 0.0f : static_cast<float>(gain - std::copysign(kAdaptStepDb, gain));
            }
            changed = changed || !ma::sameBand(next[slot], current);
        }
        if (!changed)
            continue;
        ma::bus::Command command {};
        command.target = channel.owner;
        command.serial = ++commandSerial;
        command.kind = ma::bus::CommandKind::adapt;
        for (int b = 0; b < ma::kEqBands; ++b)
            command.eq[b] = next[static_cast<size_t>(b)];
        sendCommand(channel.owner, command);
        ++moved;
    }
    if (moved > 0)
    {
        adaptiveState.lastChangeMs = now;
        adaptiveState.channelsMoved = moved;
    }
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
            auto& smoothed = channel.rawDb[static_cast<size_t>(b)];
            if (useProfiles || !continuing)
                smoothed = level;
            else
                smoothed = previous->rawDb[static_cast<size_t>(b)]
                           + alpha * (level - previous->rawDb[static_cast<size_t>(b)]);
        }
        applyCleanup(channel, channel.data.cleanup);

        // What the Agent actually sends on, smoothed like the rest.
        channel.outputValid = !useProfiles && channel.data.liveValid != 0
                              && *std::max_element(std::begin(channel.data.outputDb), std::end(channel.data.outputDb)) > -120.0f;
        const bool outputContinuing = previous != mix.channels.end() && previous->outputValid;
        for (int b = 0; b < ma::bus::kBands; ++b)
        {
            const double level = channel.outputValid ? channel.data.outputDb[b] : -200.0;
            auto& smoothed = channel.outputDb[static_cast<size_t>(b)];
            smoothed = outputContinuing && channel.outputValid
                           ? previous->outputDb[static_cast<size_t>(b)] + alpha * (level - previous->outputDb[static_cast<size_t>(b)])
                           : level;
        }

        // Keep a channel that just went quiet in the picture while its average fades.
        channel.spectrumValid = audible || (continuing && !useProfiles && *std::max_element(channel.rawDb.begin(), channel.rawDb.end()) > -120.0);
        next.push_back(channel);
    }

    std::sort(next.begin(), next.end(), [](const auto& a, const auto& b) {
        if (a.data.priority != b.data.priority)
            return a.data.priority < b.data.priority;
        return a.name.compareNatural(b.name) < 0;
    });
    mix.channels = std::move(next);
}

void BrainProcessor::applyCleanup(MixSnapshot::Channel& channel, const ma::CleanupSettings& cleanup)
{
    ma::EqSettings eq {};
    if (channel.data.eqBypass == 0)
        for (int b = 0; b < ma::kEqBands; ++b)
            eq[static_cast<size_t>(b)] = channel.data.eq[b];
    for (int b = 0; b < ma::bus::kBands; ++b)
    {
        const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
        const auto band = static_cast<size_t>(b);
        channel.bandsDb[band] = std::max(-200.0, channel.rawDb[band] + ma::cleanupResponseDb(cleanup, hz));
        channel.heardDb[band] = std::max(-200.0, channel.bandsDb[band] + ma::eqResponseDb(eq, hz));
    }
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
        juce::StringArray notLearned, limited, hot, driven;
        for (const auto& channel : mix.channels)
        {
            if (channel.data.hasProfile == 0)
                notLearned.add(channel.name);
            if (channel.data.trimLimited != 0)
                limited.add(channel.name);
            // A channel marked for deliberate overdrive may go over: it is reported, not flagged.
            if (channel.data.intentionalDrive != 0)
                driven.add(channel.name);
            else if (channel.data.outputPeakDb > 0.0f)
                hot.add(channel.name);
        }

        MixSnapshot::ReportItem item;
        item.title = pl("Gain staging kanałów");
        if (!hot.isEmpty())
        {
            item.status = Status::problem;
            item.detail = pl("Ponad 0 dBFS na wyjściu: ") + joinNames(hot)
                          + pl(". Naucz poziom albo obniż trim. Jeśli to zamierzone, zaznacz w Agencie „Celowy przester”.");
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
            item.detail = driven.isEmpty() ? pl("Wszystkie kanały nauczone, bez przesterowań.") : pl("Wszystkie kanały nauczone.");
        }
        if (!driven.isEmpty())
            item.detail << "\n" << pl("Celowo przesterowane (bez ostrzeżeń): ") << joinNames(driven) << ".";
        report.push_back(item);

        // Roles drive priorities and every proposal; the Agent recognises them while it learns.
        juce::StringArray noRole;
        for (const auto& channel : mix.channels)
            if (channel.data.role == static_cast<int>(ma::Role::other))
                noRole.add(channel.name);
        if (!noRole.isEmpty())
        {
            MixSnapshot::ReportItem roles;
            roles.title = pl("Role kanałów");
            roles.status = Status::warning;
            roles.detail = pl("Bez roli: ") + joinNames(noRole)
                           + pl(". „Naucz poziom” w Agencie rozpozna instrument i ustawi rolę i priorytet, albo wybierz ją sam.");
            report.push_back(roles);
        }
    }

    // Adaptive mode.
    if (isAdaptive())
    {
        MixSnapshot::ReportItem item;
        item.title = pl("Tryb adaptacyjny");
        const auto& a = adaptiveState;
        if (!a.running)
        {
            item.status = Status::warning;
            item.detail = pl("Włączony, wstrzymany: ") + a.waitingFor + ".";
        }
        else
        {
            item.status = Status::good;
            item.detail = pl("Śledzi miks: korekcje EQ zmieniają się najwyżej o ") + juce::String(kAdaptStepDb, 2)
                          + pl(" dB na sekundę, w limitach solvera.");
            if (a.lastChangeMs != 0)
                item.detail << pl(" Ostatnia zmiana ") << juce::String(static_cast<int>((ma::SharedSession::nowMs() - a.lastChangeMs) / 1000))
                            << pl(" s temu (kanały: ") << juce::String(a.channelsMoved) << ").";
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

    // Kick and bass in time: dynamic give-way and polarity, measured by Agents with a sidechain.
    {
        juce::StringArray noSignal, cancelling, ducking;
        bool kick = false, bass = false, anySidechain = false;
        for (const auto& channel : mix.channels)
        {
            kick = kick || channel.data.role == 1;
            bass = bass || channel.data.role == 2;
            const bool connected = channel.data.sidechainConnected != 0;
            anySidechain = anySidechain || connected;
            if (channel.data.duck.on > 0.5f && !connected)
                noSignal.add(channel.name);
            if (connected && channel.data.correlationValid != 0 && channel.data.sidechainLowCorrelation < kCancellingCorrelation)
                cancelling.add(channel.name + " (" + juce::String(channel.data.sidechainLowCorrelation, 2) + ")");
            if (connected && channel.data.duck.on > 0.5f)
                ducking.add(channel.name + pl(" do ") + juce::String(channel.data.duckReductionDb, 1) + " dB");
        }

        MixSnapshot::ReportItem item;
        item.title = pl("Stopa i bas w czasie");
        if (!cancelling.isEmpty())
        {
            item.status = Status::problem;
            item.detail = pl("W basie znosi się z sidechainem: ") + joinNames(cancelling)
                          + pl(". „Zaproponuj” odwróci polaryzację, porównaj „Przed / po”.");
        }
        else if (!noSignal.isEmpty())
        {
            item.status = Status::warning;
            item.detail = pl("Ustępowanie włączone, ale brak sygnału sidechain: ") + joinNames(noSignal)
                          + pl(". W mikserze FL na kanale stopy kliknij prawym strzałkę wysyłki do tego kanału i wybierz "
                               "„Sidechain to this track”.");
        }
        else if (anySidechain)
        {
            item.status = Status::good;
            item.detail = ducking.isEmpty() ? pl("Sidechain podłączony, faza w basie w porządku.")
                                            : pl("Faza w porządku. Ustępuje: ") + joinNames(ducking) + ".";
        }
        else if (kick && bass)
        {
            item.detail = pl("Podłącz stopę jako sidechain do Agenta basu, żeby sprawdzić fazę i ustępować dynamicznie.");
        }
        if (item.detail.isNotEmpty())
            report.push_back(item);
    }

    // Translation to small speakers, cars and clubs.
    {
        MixSnapshot::ReportItem item;
        item.title = pl("Translacja na inne systemy");
        const int preview = previewSystemIndex();
        if (preview != 0)
        {
            item.status = Status::warning;
            item.detail = pl("Odsłuch przez symulację: ") + systemNames()[preview] + pl(". Wyłącz przed eksportem (Odsłuch: Pełne pasmo).");
        }
        else if (!mix.translation.valid)
        {
            item.detail = pl("Puść utwór: potrzeba co najmniej dwóch grających kanałów.");
        }
        else
        {
            juce::StringArray lost, weak;
            for (size_t c = 0; c < mix.channels.size(); ++c)
            {
                const auto& change = mix.translation.shareChangeDb[c];
                const double phone = change[static_cast<size_t>(ma::PlaybackSystem::phone)];
                const double laptop = change[static_cast<size_t>(ma::PlaybackSystem::laptop)];
                if (phone < kPhoneGoalDb - 6.0 || laptop < kLaptopGoalDb - 6.0)
                    lost.add(mix.channels[c].name);
                else if (phone < kPhoneAimDb || laptop < kLaptopAimDb)
                    weak.add(mix.channels[c].name);
            }
            if (!lost.isEmpty())
            {
                item.status = Status::problem;
                item.detail = pl("Na telefonie i laptopie prawie znika: ") + joinNames(lost)
                              + pl(". „Zaproponuj” dobierze kroki. Posłuchaj w „Odsłuch: Telefon”.");
            }
            else if (!weak.isEmpty())
            {
                item.status = Status::warning;
                item.detail = pl("Na małych głośnikach słabnie: ") + joinNames(weak) + pl(". „Zaproponuj” dobierze kroki.");
            }
            else
            {
                item.status = Status::good;
                item.detail = pl("Wszystkie kanały słychać na telefonie, laptopie, w samochodzie i w klubie.");
            }
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

    const double peak = truePeakDb();
    const double over = std::isfinite(peak) ? peak - peakCeilingDb() : 0.0;
    const auto solved = solveNow(over > 0.0 ? -std::ceil((over + 0.5) * 2.0) / 2.0 : 0.0);
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
        }
        currentProposal.items.push_back(item);
    }
    proposeDynamics();
    proposeHarmonics();
}

namespace
{
bool sameCleanup(const ma::CleanupSettings& a, const ma::CleanupSettings& b)
{
    return std::abs(a.hpfHz - b.hpfHz) < 1.0f && std::abs(a.monoBelowHz - b.monoBelowHz) < 1.0f
           && std::abs(a.mudDb - b.mudDb) < 0.1f && (a.mudDb > -0.05f || std::abs(a.mudHz - b.mudHz) < 5.0f)
           && std::equal(a.notches.begin(), a.notches.end(), b.notches.begin(), [](const auto& x, const auto& y) {
                  return std::abs(x.db - y.db) < 0.1f && (x.db > -0.05f || std::abs(std::log2(x.hz / y.hz)) < 0.02);
              });
}

bool sameCompressor(const ma::CompressorSettings& a, const ma::CompressorSettings& b)
{
    return (a.on > 0.5f) == (b.on > 0.5f) && std::abs(a.thresholdDb - b.thresholdDb) < 0.5f && std::abs(a.ratio - b.ratio) < 0.1f
           && std::abs(a.releaseMs - b.releaseMs) < 5.0f && std::abs(a.makeupDb - b.makeupDb) < 0.3f;
}

juce::String noteName(double hz)
{
    static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int midi = juce::roundToInt(69.0 + 12.0 * std::log2(hz / 440.0));
    return juce::String(names[((midi % 12) + 12) % 12]) + juce::String(midi / 12 - 1);
}

juce::String cleanupText(const ma::CleanupSettings& cleanup, ma::ChainPlan::HighPass reason, int crowd, const ma::ChainPlan* plan = nullptr)
{
    juce::StringArray parts;
    if (cleanup.hpfHz > 0.0f)
    {
        auto part = "HPF " + hzText(cleanup.hpfHz);
        switch (reason)
        {
            case ma::ChainPlan::HighPass::junk: part += pl(" (poniżej są tylko śmieci i dudnienie)"); break;
            case ma::ChainPlan::HighPass::lowEndOwned: part += pl(" (dół należy do stopy i basu, tu tylko go zamula)"); break;
            case ma::ChainPlan::HighPass::subsonic: part += pl(" (tylko subsonic, dół zostaje)"); break;
            case ma::ChainPlan::HighPass::none: break;
        }
        parts.add(part);
    }
    if (cleanup.monoBelowHz > 0.0f)
        parts.add(pl("mono poniżej ") + hzText(cleanup.monoBelowHz));
    if (cleanup.mudDb < 0.0f)
        parts.add(pl("muł ") + juce::String(cleanup.mudDb, 1) + pl(" dB przy ") + hzText(cleanup.mudHz) + " ("
                  + juce::String(crowd) + pl(" kanały tłoczą się w 200–500 Hz)"));
    for (size_t n = 0; n < cleanup.notches.size(); ++n)
    {
        const auto& notch = cleanup.notches[n];
        if (notch.db > -0.01f)
            continue;
        auto part = pl("rezonans ") + juce::String(notch.db, 1) + pl(" dB przy ") + hzText(notch.hz) + " (Q " + juce::String(notch.q, 1);
        if (plan != nullptr && plan->notchProminenceDb[n] > 0.0)
            part += pl(", wystaje ") + juce::String(juce::roundToInt(plan->notchProminenceDb[n])) + pl(" dB i dzwoni stale")
                    + (plan->notchCoversImportant[n] ? pl(", zasłania ważniejszy kanał") : juce::String());
        parts.add(part + ")");
    }
    return parts.joinIntoString(", ");
}
} // namespace

void BrainProcessor::proposeWholeMix()
{
    std::vector<ma::ChainChannel> chain(mix.channels.size());
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        const auto& channel = mix.channels[c];
        auto& in = chain[c];
        in.role = static_cast<ma::Role>(juce::jlimit(0, static_cast<int>(ma::Role::send), static_cast<int>(channel.data.role)));
        in.priority = channel.data.priority;
        in.valid = channel.spectrumValid;
        for (int b = 0; b < ma::bus::kBands; ++b)
            in.bandsDb[static_cast<size_t>(b)] = channel.rawDb[static_cast<size_t>(b)];
        // The Learn pass measured before the trim; the chain sits after it.
        in.hasStats = channel.data.statsValid != 0;
        in.levelP10 = channel.data.levelP10 + channel.data.trimDb;
        in.levelP50 = channel.data.levelP50 + channel.data.trimDb;
        in.levelP95 = channel.data.levelP95 + channel.data.trimDb;
        in.truePeakDb = channel.data.learnedTruePeakDb + channel.data.trimDb;
        in.lowNoteHz = channel.data.lowNoteHz;
        in.intentionalDrive = channel.data.intentionalDrive != 0;
        for (int r = 0; r < ma::kMaxResonances; ++r)
            in.resonances[static_cast<size_t>(r)] = channel.data.resonances[r];
    }
    ma::ChainContext context;
    context.bpm = tempoBpm();
    const double peak = truePeakDb();
    context.headroomShortDb = std::isfinite(peak) ? std::max(0.0, peak - peakCeilingDb()) : 0.0;
    const auto plans = ma::planChains(chain, context);

    // Collisions, kick and bass, translation: the usual proposal, made on the mix as it will
    // sound with the planned cleanup, so the solver does not cut what the high-pass removes.
    const auto saved = mix;
    for (size_t c = 0; c < mix.channels.size(); ++c)
        applyCleanup(mix.channels[c], plans[c].cleanup);
    std::vector<ma::MaskingChannel> masking(mix.channels.size());
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        masking[c].valid = mix.channels[c].spectrumValid;
        masking[c].priority = mix.channels[c].data.priority;
        for (int b = 0; b < ma::bus::kBands; ++b)
            masking[c].rawDb[static_cast<size_t>(b)] = mix.channels[c].heardDb[static_cast<size_t>(b)];
    }
    mix.masking = ma::analyseMasking(masking);
    propose();

    const double bpm = tempoBpm();
    std::vector<Proposal::Item> chainItems;
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        const auto& channel = mix.channels[c];
        const auto& plan = plans[c];
        if (!channel.spectrumValid)
            continue;
        currentProposal.verdicts.push_back(judge(c, plan));

        Proposal::Item item;
        item.action = Proposal::Item::Action::chain;
        item.owner = channel.owner;
        const auto name = quoted(channel.name) + ": ";

        if (!sameCleanup(plan.cleanup, channel.data.cleanup))
        {
            item.tool = Proposal::Item::ChainTool::cleanup;
            item.cleanup = plan.cleanup;
            const auto text = cleanupText(plan.cleanup, plan.highPassReason, plan.mudCrowd, &plan);
            item.text = name + (text.isEmpty() ? pl("porządki wyłączone, nie są potrzebne") : text);
            chainItems.push_back(item);
        }
        if (plan.compressor.on > 0.5f && !sameCompressor(plan.compressor, channel.data.compressor))
        {
            const auto& comp = plan.compressor;
            item.tool = Proposal::Item::ChainTool::compressor;
            item.compressor = comp;
            item.text = name + pl("kompresja ") + juce::String(comp.ratio, 1) + pl(":1 od ") + juce::String(comp.thresholdDb, 1)
                        + pl(" dB, atak ") + juce::String(juce::roundToInt(comp.attackMs)) + pl(" ms, powrót ")
                        + juce::String(juce::roundToInt(comp.releaseMs)) + " ms" + (bpm > 0.0 ? pl(" (pod tempo)") : juce::String())
                        + ", makeup +" + juce::String(comp.makeupDb, 1) + pl(" dB: głośne i ciche fragmenty dzieli ")
                        + juce::String(plan.spreadDb, 1) + pl(" dB, po kompresji ok. ") + juce::String(plan.spreadAfterDb, 1)
                        + pl(" dB (cel ") + juce::String(plan.targetSpreadDb, 1) + " dB)";
            if (plan.lostInQuietParts)
                item.text += pl(". Ciche fragmenty giną pod resztą miksu, więc trzymam go ciaśniej");
            if (plan.arrangementDb > 0.0)
                item.text += pl(". Pozostałe ") + juce::String(plan.arrangementDb, 1)
                             + pl(" dB to dynamika aranżacji (cichsze takty), kompresor ich nie spłaszcza");
            chainItems.push_back(item);
        }
        if (plan.clipper.on > 0.5f
            && (channel.data.clipper.on < 0.5f || std::abs(plan.clipper.ceilingDb - channel.data.clipper.ceilingDb) > 0.3f))
        {
            item.tool = Proposal::Item::ChainTool::clipper;
            item.clipper = plan.clipper;
            item.text = name + pl("clipper ścina ") + juce::String(plan.clipDb, 1) + pl(" dB szczytów (sufit ")
                        + juce::String(plan.clipper.ceilingDb, 1) + pl(" dBFS): szczyt jest ") + juce::String(plan.crestDb, 1)
                        + pl(" dB nad głośną częścią, wystarczy ") + juce::String(plan.maxCrestDb, 0)
                        + pl(". Zapas na mastering bez słyszalnej straty");
            chainItems.push_back(item);
        }
        if (plan.evener.on > 0.5f
            && (channel.data.evener.on < 0.5f || std::abs(plan.evener.referenceHz - channel.data.evener.referenceHz) > 2.0f))
        {
            item.tool = Proposal::Item::ChainTool::evener;
            item.evener = plan.evener;
            item.text = name + pl("wyrównanie nut basu względem ") + noteName(plan.evener.referenceHz) + " ("
                        + hzText(plan.evener.referenceHz) + pl(", ") + juce::String(juce::roundToInt(plan.evener.amount * 100.0f))
                        + pl("%): niższe nuty ucho słyszy ciszej, więc dostają więcej, wyższe mniej. Linia gra równo");
            chainItems.push_back(item);
        }
    }
    mix = saved;

    currentProposal.items.insert(currentProposal.items.begin(), chainItems.begin(), chainItems.end());
    currentProposal.wholeMix = true;
}

Proposal::Verdict BrainProcessor::judge(size_t index, const ma::ChainPlan& plan) const
{
    using Status = MixSnapshot::Status;
    const auto& channel = mix.channels[index];
    Proposal::Verdict verdict;
    verdict.owner = channel.owner;
    verdict.name = channel.name;
    const auto roles = ma::ui::roleNames();
    verdict.role = roles[juce::jlimit(0, roles.size() - 1, static_cast<int>(channel.data.role))];
    verdict.status = Status::good;
    juce::StringArray attention;

    // Low end and mud.
    const auto cleanup = cleanupText(plan.cleanup, plan.highPassReason, plan.mudCrowd, &plan);
    verdict.reasons.add(cleanup.isEmpty() ? pl("Dół i środek: nic do sprzątania.") : pl("Porządki: ") + cleanup + ".");
    if (plan.resonancesBuried > 0)
        verdict.reasons.add(pl("Rezonanse, których miks nie słyszy (zakryte przez resztę): ") + juce::String(plan.resonancesBuried)
                            + pl(", zostają bez cięcia."));

    // Dynamics.
    if (!plan.dynamicsKnown)
    {
        verdict.reasons.add(pl("Dynamika nieznana: kliknij „Naucz poziom” w Agencie na najgęstszym fragmencie."));
        verdict.status = Status::unknown;
    }
    else if (channel.data.intentionalDrive != 0)
    {
        verdict.reasons.add(pl("Celowy przester: dynamika i szczyty zostają tak, jak są."));
    }
    else if (plan.overCompressed)
    {
        verdict.reasons.add(pl("Już mocno ściśnięty przed Agentem (szczyt tylko ") + juce::String(plan.crestDb, 1)
                            + pl(" dB nad głośną częścią). Nie dokładam kompresji; jeśli brzmi płasko, zdejmij kompresję u źródła."));
        attention.add(pl("przekompresowany u źródła"));
    }
    else if (plan.percussive)
    {
        verdict.reasons.add(pl("Perkusyjny: bez kompresora, uderzenia zostają."));
    }
    else if (plan.compressor.on > 0.5f)
    {
        verdict.reasons.add(pl("Dynamika: ") + juce::String(plan.spreadDb, 1) + pl(" dB między głośnymi i cichymi fragmentami, po kompresji ")
                            + juce::String(plan.spreadAfterDb, 1) + pl(" dB (cel dla roli ") + juce::String(plan.targetSpreadDb, 1)
                            + " dB).");
        if (plan.arrangementDb > 0.0)
            verdict.reasons.add(pl("Reszta różnicy (") + juce::String(plan.arrangementDb, 1)
                                + pl(" dB) to aranżacja, np. cichszy takt. Kompresor tego nie spłaszcza; jeśli te fragmenty mają "
                                     "być głośniej, zautomatyzuj głośność."));
    }
    else
    {
        verdict.reasons.add(pl("Dynamika w normie: ") + juce::String(plan.spreadDb, 1) + pl(" dB (cel ")
                            + juce::String(plan.targetSpreadDb, 1) + pl(" dB), kompresja niepotrzebna."));
    }
    if (plan.lostInQuietParts)
        verdict.reasons.add(pl("Ciche fragmenty giną pod resztą miksu: kompresja trzyma je wyżej."));

    if (plan.clipper.on > 0.5f)
        verdict.reasons.add(pl("Szczyty: ") + juce::String(plan.crestDb, 1) + pl(" dB nad głośną częścią, clipper zdejmuje ")
                            + juce::String(plan.clipDb, 1) + " dB.");
    else if (plan.dynamicsKnown && plan.crestDb > 0.0 && channel.data.intentionalDrive == 0)
        verdict.reasons.add(pl("Szczyty w normie (") + juce::String(plan.crestDb, 1) + pl(" dB nad głośną częścią)."));

    if (plan.evener.on > 0.5f)
        verdict.reasons.add(pl("Linia basu wokół ") + noteName(plan.evener.referenceHz) + pl(": niskie nuty wyrównane dla ucha."));

    // Its place in the mix: what still covers it, and the fixes on the way.
    for (const auto& issue : mix.masking.issues)
    {
        if (issue.masked != static_cast<int>(index) || issue.score < 0.3)
            continue;
        const auto& masker = mix.channels[static_cast<size_t>(issue.masker)];
        const bool fixed = std::any_of(currentProposal.items.begin(), currentProposal.items.end(), [&](const auto& item) {
            return item.maskedOwner == channel.owner && item.owner == masker.owner && !item.advisory;
        });
        verdict.reasons.add(pl("Zasłania go ") + quoted(masker.name) + pl(" w ") + hzText(issue.lowHz) + pl("–") + hzText(issue.highHz)
                            + " (" + juce::String(juce::roundToInt(issue.score * 100.0)) + "%)"
                            + (fixed ? pl(": poprawka jest w propozycji.") : pl(": zostaje, bo ") + quoted(masker.name)
                                                                                  + pl(" jest tam ważniejszy.")));
        if (!fixed && issue.score >= 0.5)
            attention.add(pl("ginie pod ") + quoted(masker.name));
    }
    if (index < mix.translation.shareChangeDb.size() && mix.translation.valid)
    {
        const double phone = mix.translation.shareChangeDb[index][static_cast<size_t>(ma::PlaybackSystem::phone)];
        const bool helped = std::any_of(currentProposal.items.begin(), currentProposal.items.end(), [&](const auto& item) {
            return item.owner == channel.owner
                   && (item.action == Proposal::Item::Action::harmonics || item.action == Proposal::Item::Action::transients);
        });
        if (phone < kPhoneGoalDb && helped)
            verdict.reasons.add(pl("Na telefonie ginie (") + juce::String(juce::roundToInt(phone))
                                + pl(" dB względem reszty): harmoniczne albo atak są w propozycji."));
        else if (phone < kPhoneGoalDb)
            attention.add(pl("ginie na telefonie (") + juce::String(juce::roundToInt(phone)) + " dB)");
    }

    if (verdict.status == Status::unknown)
        verdict.summary = pl("Porządki ustawione, reszta po nauce poziomu.");
    else if (!attention.isEmpty())
    {
        verdict.status = Status::warning;
        verdict.summary = pl("Do sprawdzenia: ") + attention.joinIntoString(", ") + ".";
    }
    else
        verdict.summary = pl("Po zmianach ustawiony poprawnie w kontekście miksu.");
    return verdict;
}

void BrainProcessor::proposeHarmonics()
{
    if (mix.channels.empty())
        return;

    std::vector<ma::TranslationChannel> base(mix.channels.size());
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        base[c].valid = mix.channels[c].outputValid || mix.channels[c].spectrumValid;
        for (int b = 0; b < ma::bus::kBands; ++b)
            base[c].rawDb[static_cast<size_t>(b)] = mix.channels[c].outputValid ? mix.channels[c].outputDb[static_cast<size_t>(b)]
                                                                                : mix.channels[c].heardDb[static_cast<size_t>(b)];
    }
    const auto before = ma::analyseTranslation(base);
    const auto phoneIndex = static_cast<size_t>(ma::PlaybackSystem::phone);
    const auto laptopIndex = static_cast<size_t>(ma::PlaybackSystem::laptop);

    // Channels that weaken on small speakers. Most important first; every accepted step goes
    // into `trial`, so later channels are judged on the mix as it will be. For each channel:
    // first make room in the band a phone plays (a cut on a less important channel covering
    // it), then, if still short, the best own step for the kind of sound (attack emphasis for
    // percussion, overtones for low content, a presence bell otherwise). Every step is sized
    // by prediction to the smallest amount that reaches the goal, and rejected if it would
    // push another channel under its goal.
    auto trial = base;
    std::vector<size_t> order(mix.channels.size());
    for (size_t c = 0; c < order.size(); ++c)
        order[c] = c;
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return mix.channels[a].data.priority < mix.channels[b].data.priority; });

    const auto deficit = [&](const ma::TranslationResult& r, size_t c) {
        return std::max(0.0, kPhoneAimDb - r.shareChangeDb[c][phoneIndex]) + std::max(0.0, kLaptopAimDb - r.shareChangeDb[c][laptopIndex]);
    };
    // How much a change hurts the other channels: what it adds to their deficits.
    const auto harm = [&](const ma::TranslationResult& was, const ma::TranslationResult& now, size_t c) {
        double total = 0.0;
        for (size_t o = 0; o < mix.channels.size(); ++o)
            if (o != c && trial[o].valid)
                total += std::max(0.0, deficit(now, o) - deficit(was, o));
        return total;
    };
    const auto shares = [&](const ma::TranslationResult& r, size_t c) {
        return juce::String(juce::roundToInt(r.shareChangeDb[c][phoneIndex])) + "/" + juce::String(juce::roundToInt(r.shareChangeDb[c][laptopIndex]));
    };
    const auto proposedEq = [&](std::uint64_t owner) -> ma::EqSettings* {
        for (auto& e : currentProposal.eq)
            if (e.first == owner)
                return &e.second;
        return nullptr;
    };
    const auto freeSlot = [](const ma::EqSettings& eq) {
        for (int b = 0; b < ma::kEqBands; ++b)
            if (!ma::isOn(eq[static_cast<size_t>(b)]))
                return b;
        return -1;
    };
    const auto withBell = [](const ma::Curve& db, const ma::EqBand& band) {
        ma::EqSettings eq {};
        eq[0] = band;
        auto out = db;
        for (int b = 0; b < ma::bus::kBands; ++b)
            out[static_cast<size_t>(b)] += ma::eqResponseDb(eq, ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b));
        return out;
    };
    const auto bandRange = [](double lowHz, double highHz, int& first, int& last) {
        first = ma::bus::kBands;
        last = -1;
        for (int b = 0; b < ma::bus::kBands; ++b)
        {
            const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
            if (hz >= lowHz && hz <= highHz)
            {
                first = std::min(first, b);
                last = std::max(last, b);
            }
        }
    };
    const auto summary = [&](const juce::String& name, const ma::TranslationResult& was, const ma::TranslationResult& now, size_t c) {
        return pl(", żeby ") + quoted(name) + pl(" był słyszalny na małych głośnikach (telefon/laptop ") + shares(was, c) + pl(" → ")
               + shares(now, c) + pl(" dB względem reszty miksu)");
    };

    for (const size_t c : order)
    {
        if (!before.valid || !trial[c].valid)
            continue;
        auto now = ma::analyseTranslation(trial);
        if (deficit(now, c) <= 0.0)
            continue;
        const auto& channel = mix.channels[c];
        const auto start = now;

        // Where this channel has content: measured against its loudest band.
        const auto own = trial[c].rawDb; // copy: `trial` is replaced as steps are accepted
        const double ownPeak = *std::max_element(own.begin(), own.begin() + ma::bus::kBands);

        // 1. Room in what a phone plays (300 Hz - 6 kHz), taken from a less important channel.
        {
            int first = 0, last = -1;
            bandRange(300.0, 6000.0, first, last);
            // Try a -4 dB bell on every less important channel at every band where it covers this
            // one, and keep the place that helps most for the least harm; then size it.
            int bestMasker = -1, bestBand = -1;
            double bestScore = 0.5;
            for (size_t m = 0; m < mix.channels.size(); ++m)
            {
                if (m == c || !trial[m].valid || mix.channels[m].data.priority <= channel.data.priority)
                    continue;
                for (int b = first; b <= last; ++b)
                {
                    if (own[static_cast<size_t>(b)] < ownPeak - 45.0 || own[static_cast<size_t>(b)] < -110.0
                        || trial[m].rawDb[static_cast<size_t>(b)] - own[static_cast<size_t>(b)] < 3.0)
                        continue;
                    auto test = trial;
                    test[m].rawDb = withBell(trial[m].rawDb, { static_cast<float>(ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b)), -4.0f, 1.0f });
                    const auto after = ma::analyseTranslation(test);
                    const double score = deficit(now, c) - deficit(after, c) - harm(now, after, c);
                    if (score > bestScore)
                    {
                        bestScore = score;
                        bestMasker = static_cast<int>(m);
                        bestBand = b;
                    }
                }
            }
            if (bestMasker >= 0)
            {
                const auto m = static_cast<size_t>(bestMasker);
                const auto& masker = mix.channels[m];
                auto* eq = proposedEq(masker.owner);
                const int slot = eq != nullptr ? freeSlot(*eq) : -1;
                if (slot >= 0)
                {
                    ma::EqBand band { static_cast<float>(std::round(ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, bestBand))), 0.0f, 1.0f };
                    auto test = trial;
                    ma::TranslationResult after = now;
                    for (float cut = -2.0f; cut >= -6.0f; cut -= 1.0f)
                    {
                        band.gainDb = cut;
                        test[m].rawDb = withBell(trial[m].rawDb, band);
                        after = ma::analyseTranslation(test);
                        if (deficit(after, c) <= 0.0)
                            break;
                    }
                    const double gain = deficit(now, c) - deficit(after, c);
                    if (gain >= 1.0 && harm(now, after, c) < 0.5 * gain)
                    {
                        (*eq)[static_cast<size_t>(slot)] = band;
                        Proposal::Item item;
                        item.change.kind = ma::SolverChange::Kind::eqCut;
                        item.change.channel = bestMasker;
                        item.change.maskedChannel = static_cast<int>(c);
                        item.change.eqSlot = slot;
                        item.owner = masker.owner;
                        item.maskedOwner = channel.owner;
                        item.band = band;
                        item.text = quoted(masker.name) + ": " + juce::String(band.gainDb, 1) + pl(" dB przy ") + hzText(band.hz)
                                    + " (Q 1.0)" + summary(channel.name, now, after, c);
                        currentProposal.items.push_back(item);
                        trial = test;
                        now = after;
                    }
                }
            }
        }
        if (deficit(now, c) <= 0.0)
            continue;

        // 2. The channel's own step, by the kind of sound.
        const bool percussive = channel.data.role == 1 || channel.data.role == 3;
        int peak = 0;
        for (int b = 1; b < ma::bus::kBands; ++b)
            if (own[static_cast<size_t>(b)] > own[static_cast<size_t>(peak)])
                peak = b;
        const double peakHz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, peak);

        // Where in 1-6 kHz the channel is least covered by the rest of the mix.
        const auto clearestBand = [&](double lowHz, double highHz, double rangeDb) {
            int first = 0, last = -1;
            bandRange(lowHz, highHz, first, last);
            int best = -1;
            double bestMargin = -1e9;
            for (int b = first; b <= last; ++b)
            {
                if (own[static_cast<size_t>(b)] < ownPeak - rangeDb || own[static_cast<size_t>(b)] < -110.0)
                    continue;
                double others = 0.0;
                for (size_t o = 0; o < trial.size(); ++o)
                    if (o != c && trial[o].valid)
                        others += std::pow(10.0, trial[o].rawDb[static_cast<size_t>(b)] / 10.0);
                const double margin = own[static_cast<size_t>(b)] - 10.0 * std::log10(std::max(others, 1e-20));
                if (margin > bestMargin)
                {
                    bestMargin = margin;
                    best = b;
                }
            }
            return best;
        };

        const auto accept = [&](const ma::TranslationResult& after) {
            const double gain = deficit(now, c) - deficit(after, c);
            return gain >= 1.0 && harm(now, after, c) < 0.5 * gain;
        };
        const auto tryAttack = [&] {
            if (channel.data.transients.on > 0.5f)
                return;
            const int band = clearestBand(1000.0, 6000.0, 45.0);
            if (band >= 0)
            {
                ma::TransientSettings settings;
                settings.on = 1.0f;
                settings.hz = static_cast<float>(juce::jlimit(1000.0, 6000.0,
                                                              std::round(ma::SpectrumAnalyzer::bandEdgeHz(ma::BandScale::erb, band) / 100.0) * 100.0));
                auto test = trial;
                ma::TranslationResult after = now;
                for (float amount = 2.0f; amount <= kMaxAttackDb; amount += 1.0f)
                {
                    settings.amountDb = amount;
                    test[c].rawDb = ma::predictTransients(trial[c].rawDb, settings, kOnsetShare);
                    after = ma::analyseTranslation(test);
                    if (deficit(after, c) <= 0.0)
                        break;
                }
                if (accept(after))
                {
                    Proposal::Item item;
                    item.action = Proposal::Item::Action::transients;
                    item.owner = channel.owner;
                    item.transients = settings;
                    item.text = quoted(channel.name) + pl(": podkreślenie ataku powyżej ") + hzText(settings.hz) + pl(", do +")
                                + juce::String(settings.amountDb, 0) + pl(" dB tylko na uderzeniach") + summary(channel.name, now, after, c)
                                + pl(". Wynik ataku zależy od brzmienia, sprawdź „Przed / po”");
                    currentProposal.items.push_back(item);
                    trial = test;
                    now = after;
                }
            }
        };
        const auto tryHarmonics = [&] {
            if (peakHz > 250.0 || channel.data.harmonics.on > 0.5f)
                return;
            ma::HarmonicSettings settings;
            settings.on = 1.0f;
            settings.hz = static_cast<float>(juce::jlimit(60.0, 300.0, std::round(2.0 * peakHz / 5.0) * 5.0));
            auto test = trial;
            ma::TranslationResult after = now;
            // Overtones on a kick soften its punch, so less of them there.
            const float most = percussive ? kMaxPercussiveHarmonicsDb : kMaxHarmonicsDb;
            for (float amount = -24.0f; amount <= most; amount += 1.0f)
            {
                settings.amountDb = amount;
                test[c].rawDb = ma::predictHarmonics(trial[c].rawDb, settings);
                after = ma::analyseTranslation(test);
                if (deficit(after, c) <= 0.0)
                    break;
            }
            if (accept(after))
            {
                Proposal::Item item;
                item.action = Proposal::Item::Action::harmonics;
                item.owner = channel.owner;
                item.harmonics = settings;
                item.text = quoted(channel.name) + pl(": harmoniczne poniżej ") + hzText(settings.hz) + ", "
                            + juce::String(settings.amountDb, 0) + " dB" + summary(channel.name, now, after, c);
                currentProposal.items.push_back(item);
                trial = test;
                now = after;
            }
        };
        const auto tryPresence = [&] {
            auto* eq = proposedEq(channel.owner);
            const int slot = eq != nullptr ? freeSlot(*eq) : -1;
            const int band = clearestBand(1000.0, 5000.0, 30.0);
            if (slot >= 0 && band >= 0)
            {
                ma::EqBand bell { static_cast<float>(std::round(ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, band))), 0.0f, 1.0f };
                auto test = trial;
                ma::TranslationResult after = now;
                for (float gain = 1.0f; gain <= 4.0f; gain += 0.5f)
                {
                    bell.gainDb = gain;
                    test[c].rawDb = withBell(trial[c].rawDb, bell);
                    after = ma::analyseTranslation(test);
                    if (deficit(after, c) <= 0.0)
                        break;
                }
                // No new collision with a more important channel.
                std::vector<ma::MaskingChannel> was(trial.size()), will(trial.size());
                for (size_t o = 0; o < trial.size(); ++o)
                {
                    was[o] = { trial[o].rawDb, mix.channels[o].data.priority, trial[o].valid };
                    will[o] = { test[o].rawDb, mix.channels[o].data.priority, test[o].valid };
                }
                const auto maskWas = ma::analyseMasking(was);
                const auto maskWill = ma::analyseMasking(will);
                bool collides = false;
                for (size_t o = 0; o < trial.size(); ++o)
                    if (o != c && mix.channels[o].data.priority <= channel.data.priority)
                        collides = collides || maskWill.amount[c][o] - maskWas.amount[c][o] > 0.05;
                if (!collides && accept(after))
                {
                    (*eq)[static_cast<size_t>(slot)] = bell;
                    Proposal::Item item;
                    item.change.kind = ma::SolverChange::Kind::tonalBoost;
                    item.change.channel = static_cast<int>(c);
                    item.change.eqSlot = slot;
                    item.owner = channel.owner;
                    item.band = bell;
                    item.text = quoted(channel.name) + ": +" + juce::String(bell.gainDb, 1) + pl(" dB obecności przy ") + hzText(bell.hz)
                                + " (Q 1.0)" + summary(channel.name, now, after, c);
                    currentProposal.items.push_back(item);
                    trial = test;
                    now = after;
                }
            }
        };

        // Gentlest step for the kind of sound first; the next only if still short.
        const std::vector<std::function<void()>> steps = percussive ? std::vector<std::function<void()>> { tryAttack, tryHarmonics }
                                                       : peakHz <= 250.0 ? std::vector<std::function<void()>> { tryHarmonics, tryPresence }
                                                                         : std::vector<std::function<void()>> { tryPresence };
        for (const auto& step : steps)
            if (deficit(now, c) > 0.0)
                step();

        if (deficit(now, c) > 0.0 && deficit(now, c) >= deficit(start, c) - 0.5)
        {
            // Nothing in reach helped: say why rather than stay silent.
            Proposal::Item item;
            item.advisory = true;
            item.selected = false;
            item.text = quoted(channel.name) + pl(" słabnie na małych głośnikach (telefon/laptop ") + shares(now, c)
                        + pl(" dB), a wtyczka nie znalazła kroku, który by pomógł bez szkody dla innych. Spróbuj innej barwy "
                             "albo warstwy w paśmie 1–5 kHz");
            currentProposal.items.push_back(item);
        }
    }

    // Tonal holes nothing plays in: overtones from the channel an octave below fill them.
    for (auto& item : currentProposal.items)
    {
        if (item.action != Proposal::Item::Action::solver || item.change.kind != ma::SolverChange::Kind::tonalAdvice
            || item.change.sourceChannel < 0)
            continue;
        const auto source = static_cast<size_t>(item.change.sourceChannel);
        const auto& channel = mix.channels[source];
        if (channel.data.harmonics.on > 0.5f
            || std::any_of(currentProposal.items.begin(), currentProposal.items.end(), [&](const auto& other) {
                   return other.action == Proposal::Item::Action::harmonics && other.owner == channel.owner;
               }))
            continue;

        int first = ma::bus::kBands, last = -1;
        for (int b = 0; b < ma::bus::kBands; ++b)
        {
            const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
            if (hz >= item.change.lowHz && hz <= item.change.highHz)
            {
                first = std::min(first, b);
                last = std::max(last, b);
            }
        }
        if (last < first)
            continue;
        const auto regionPower = [&](const std::vector<ma::TranslationChannel>& channels) {
            double power = 0.0;
            for (const auto& ch : channels)
                if (ch.valid)
                    for (int b = first; b <= last; ++b)
                        power += std::pow(10.0, ch.rawDb[static_cast<size_t>(b)] / 10.0);
            return power;
        };
        const double was = regionPower(trial);
        const double wanted = std::abs(item.change.deviationDb);

        ma::HarmonicSettings settings;
        settings.on = 1.0f;
        settings.hz = static_cast<float>(juce::jlimit(40.0, 8000.0, std::round(item.change.lowHz)));
        auto holeTrial = trial;
        double gained = 0.0;
        for (float amount = -24.0f; amount <= kMaxHarmonicsDb; amount += 1.0f)
        {
            settings.amountDb = amount;
            holeTrial[source].rawDb = ma::predictHarmonics(trial[source].rawDb, settings);
            gained = 10.0 * std::log10(std::max(regionPower(holeTrial), 1e-30) / std::max(was, 1e-30));
            if (gained >= wanted)
                break;
        }
        if (gained < 0.5)
            continue; // the source has too little to work with; keep the advice

        item.action = Proposal::Item::Action::harmonics;
        item.advisory = false;
        item.selected = true;
        item.owner = channel.owner;
        item.harmonics = settings;
        item.text = quoted(channel.name) + pl(": harmoniczne powyżej ") + hzText(settings.hz) + ", " + juce::String(settings.amountDb, 0)
                    + pl(" dB, wypełnią dziurę ") + hzText(item.change.lowHz) + pl("–") + hzText(item.change.highHz) + pl(" (brakuje ")
                    + juce::String(wanted, 1) + pl(" dB, harmoniczne dodadzą ok. ") + juce::String(gained, 1) + " dB)";
    }
}

juce::String BrainProcessor::kickName() const
{
    for (const auto& channel : mix.channels)
        if (channel.data.role == 1)
            return quoted(channel.name);
    return pl("stopy");
}

void BrainProcessor::proposeDynamics()
{
    const double bpm = tempoBpm();
    const double releaseMs = bpm > 0.0 ? ma::syncedReleaseMs(bpm, ma::bus::duckSyncFraction(kDuckSync)) : 120.0;
    const auto syncName = ma::ui::pl("1/16");

    // Low collisions with the kick happen only when it hits, so the other channel gives way
    // in that band only then. Candidates: low masking issues where the kick is masked (or the
    // bass masks), and any bass sitting within 10 dB of the kick in the kick's strongest low
    // band; the masking score alone misses that when the kick's click spreads over many bands.
    struct Candidate
    {
        int masker, masked;
        double lowHz, highHz, strength;
    };
    std::vector<Candidate> candidates;
    for (const auto& issue : mix.masking.issues)
    {
        if (issue.score < 0.3 || issue.highHz > kDuckMaxHz)
            continue;
        const auto& masker = mix.channels[static_cast<size_t>(issue.masker)];
        const auto& masked = mix.channels[static_cast<size_t>(issue.masked)];
        if (masker.data.role != 1 && (masked.data.role == 1 || masker.data.role == 2))
            candidates.push_back({ issue.masker, issue.masked, issue.lowHz, issue.highHz, issue.score });
    }
    for (size_t k = 0; k < mix.channels.size(); ++k)
    {
        const auto& kick = mix.channels[k];
        if (kick.data.role != 1 || !kick.spectrumValid)
            continue;
        int peak = -1;
        for (int b = 0; b < ma::bus::kBands && ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b) < kDuckMaxHz; ++b)
            if (peak < 0 || kick.heardDb[static_cast<size_t>(b)] > kick.heardDb[static_cast<size_t>(peak)])
                peak = b;
        if (peak < 0 || kick.heardDb[static_cast<size_t>(peak)] < -90.0)
            continue;
        for (size_t c = 0; c < mix.channels.size(); ++c)
        {
            const auto& other = mix.channels[c];
            if (other.data.role != 2 || !other.spectrumValid)
                continue;
            const double gap = other.heardDb[static_cast<size_t>(peak)] - kick.heardDb[static_cast<size_t>(peak)];
            if (gap < -10.0)
                continue;
            const double low = ma::SpectrumAnalyzer::bandEdgeHz(ma::BandScale::erb, std::max(0, peak - 1));
            const double high = ma::SpectrumAnalyzer::bandEdgeHz(ma::BandScale::erb, std::min(ma::bus::kBands, peak + 2));
            candidates.push_back({ static_cast<int>(c), static_cast<int>(k), low, high, juce::jlimit(0.0, 1.0, (gap + 10.0) / 10.0) });
        }
    }

    std::vector<int> ducked;
    for (const auto& candidate : candidates)
    {
        if (std::find(ducked.begin(), ducked.end(), candidate.masker) != ducked.end())
            continue;
        ducked.push_back(candidate.masker);
        const auto& masker = mix.channels[static_cast<size_t>(candidate.masker)];
        const auto& masked = mix.channels[static_cast<size_t>(candidate.masked)];
        const ma::MaskingIssue issue { candidate.masker, candidate.masked, candidate.strength, candidate.lowHz, candidate.highHz, 0, 0 };

        const bool connected = masker.data.sidechainConnected != 0;
        const bool alreadyOn = masker.data.duck.on > 0.5f && connected;

        // The static cut for the same pair is the weaker fix; leave it for the user to pick.
        for (auto& item : currentProposal.items)
            if (item.action == Proposal::Item::Action::solver && item.change.kind == ma::SolverChange::Kind::eqCut
                && item.change.channel == issue.masker && item.change.maskedChannel == issue.masked)
            {
                item.selected = false;
                item.text += alreadyOn ? pl(". Ustępowanie dynamiczne już działa, cięcie zwykle zbędne")
                                       : pl(". Lepiej ustępowanie dynamiczne niżej");
            }
        if (alreadyOn)
            continue;

        Proposal::Item item;
        item.action = Proposal::Item::Action::duck;
        item.owner = masker.owner;
        item.maskedOwner = masked.owner;
        const double centre = std::sqrt(issue.lowHz * issue.highHz);
        auto& duck = item.duck;
        duck.on = 1.0f;
        duck.wholeSignal = 0.0f;
        duck.hz = static_cast<float>(std::round(centre));
        duck.q = static_cast<float>(juce::jlimit(0.5, 3.0, centre / std::max(1.0, issue.highHz - issue.lowHz)));
        duck.depthDb = static_cast<float>(std::round((6.0 + 4.0 * issue.score) * 2.0) / 2.0);
        // Full depth on the kick's peaks: the detector reaches it 6 dB over the threshold.
        duck.thresholdDb = connected ? static_cast<float>(juce::jlimit(-50.0, -6.0, std::round(masker.data.sidechainDb - 18.0)))
                                     : -30.0f;
        duck.attackMs = 3.0f;
        duck.releaseMs = static_cast<float>(std::round(releaseMs));
        item.duckSync = bpm > 0.0 ? kDuckSync : 0;

        item.text = quoted(masker.name) + pl(": ustępuje ") + quoted(masked.name) + pl(" dynamicznie, do -")
                    + juce::String(duck.depthDb, 1) + pl(" dB przy ") + hzText(duck.hz) + pl(", tylko gdy stopa gra (powrót ")
                    + (bpm > 0.0 ? syncName + " = " : juce::String()) + juce::String(juce::roundToInt(duck.releaseMs)) + " ms)";
        if (connected)
            item.text += pl(". Sidechain podłączony");
        else
            item.text += pl(". Potrzebny sidechain: w mikserze FL na kanale ") + kickName()
                         + pl(" kliknij prawym strzałkę wysyłki pod kanałem ") + quoted(masker.name)
                         + pl(" i wybierz „Sidechain to this track”");
        currentProposal.items.push_back(item);
    }

    // Polarity: a channel that cancels the sidechain in the low end loses the low end in the mix.
    for (const auto& channel : mix.channels)
    {
        if (channel.data.sidechainConnected == 0 || channel.data.correlationValid == 0
            || channel.data.sidechainLowCorrelation >= kCancellingCorrelation)
            continue;
        Proposal::Item item;
        item.action = Proposal::Item::Action::polarity;
        item.owner = channel.owner;
        item.text = quoted(channel.name) + pl(": odwróć polaryzację, bo w basie znosi się z sidechainem (korelacja ")
                    + juce::String(channel.data.sidechainLowCorrelation, 2) + pl("). Sprawdź „Przed / po”");
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
        if (item.selected && item.action == Proposal::Item::Action::solver && item.change.kind == ma::SolverChange::Kind::globalGain)
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
            const bool eqKind = item.action == Proposal::Item::Action::solver
                                && (item.change.kind == ma::SolverChange::Kind::eqCut
                                || item.change.kind == ma::SolverChange::Kind::tonalBoost
                                || item.change.kind == ma::SolverChange::Kind::tonalCut);
            if (!item.selected || !eqKind || item.owner != channel.owner)
                continue;
            if (proposed != currentProposal.eq.end())
                eq[static_cast<size_t>(item.change.eqSlot)] = proposed->second[static_cast<size_t>(item.change.eqSlot)];
            eqChanges = true;
        }
        auto duck = channel.data.duck;
        auto duckSync = channel.data.duckSync;
        auto polarity = channel.data.polarityInverted;
        auto harmonics = channel.data.harmonics;
        auto transients = channel.data.transients;
        auto cleanup = channel.data.cleanup;
        auto compressor = channel.data.compressor;
        auto clipper = channel.data.clipper;
        auto evener = channel.data.evener;
        bool dynamicChanges = false;
        for (const auto& item : currentProposal.items)
        {
            if (!item.selected || item.owner != channel.owner)
                continue;
            if (item.action == Proposal::Item::Action::duck)
            {
                duck = item.duck;
                // The threshold follows the kick's level, which the global gain moves too.
                duck.thresholdDb = static_cast<float>(juce::jlimit(-60.0, 0.0, duck.thresholdDb + globalGain));
                duckSync = item.duckSync;
                dynamicChanges = true;
            }
            else if (item.action == Proposal::Item::Action::harmonics)
            {
                harmonics = item.harmonics;
                dynamicChanges = true;
            }
            else if (item.action == Proposal::Item::Action::transients)
            {
                transients = item.transients;
                dynamicChanges = true;
            }
            else if (item.action == Proposal::Item::Action::chain)
            {
                switch (item.tool)
                {
                    case Proposal::Item::ChainTool::cleanup: cleanup = item.cleanup; break;
                    case Proposal::Item::ChainTool::compressor: compressor = item.compressor; break;
                    case Proposal::Item::ChainTool::clipper: clipper = item.clipper; break;
                    case Proposal::Item::ChainTool::evener: evener = item.evener; break;
                    case Proposal::Item::ChainTool::none: break;
                }
                dynamicChanges = true;
            }
            else if (item.action == Proposal::Item::Action::polarity)
            {
                polarity = polarity != 0 ? 0 : 1;
                dynamicChanges = true;
            }
        }

        if (!eqChanges && !dynamicChanges && std::abs(globalGain) < 0.05)
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
        command.duck = duck;
        command.duckSync = duckSync;
        command.polarityInverted = polarity;
        command.harmonics = harmonics;
        command.transients = transients;
        // The trim moves with the global gain, and the compressor and clipper levels with it.
        compressor.thresholdDb = static_cast<float>(juce::jlimit(-60.0, 0.0, compressor.thresholdDb + globalGain));
        clipper.ceilingDb = static_cast<float>(juce::jlimit(-30.0, 0.0, clipper.ceilingDb + globalGain));
        command.cleanup = cleanup;
        command.compressor = compressor;
        command.clipper = clipper;
        command.evener = evener;
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
