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

juce::String quoted(const juce::String& name)
{
    return pl("„") + name + pl("”");
}

juce::String hzText(double hz)
{
    return hz >= 1000.0 ? juce::String(hz / 1000.0, hz >= 10000.0 ? 0 : 1) + " kHz" : juce::String(juce::roundToInt(hz)) + " Hz";
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
    startTimerHz(kUpdateHz);
}

BrainProcessor::~BrainProcessor()
{
    stopTimer();
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
    spectrum.prepare(sampleRate);
    correlation.prepare(sampleRate);
    stereo.store(channels >= 2);
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
    for (size_t b = 0; b < bands.size(); ++b)
        bands[b].store(recent[b]);
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
            masking[c].rawDb[static_cast<size_t>(b)] = channel.bandsDb[static_cast<size_t>(b)];
    }
    mix.masking = ma::analyseMasking(masking);
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

void BrainProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    if (auto xml = parameters.copyState().createXml())
        copyXmlToBinary(*xml, destData);
}

void BrainProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        if (xml->hasTagName(parameters.state.getType()))
            parameters.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessorEditor* BrainProcessor::createEditor()
{
    return new BrainEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BrainProcessor();
}
