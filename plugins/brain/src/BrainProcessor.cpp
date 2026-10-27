#include "BrainProcessor.h"

#include "BrainEditor.h"
#include "MaUi.h"
#include "ma/Instrument.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <functional>
#include <cmath>
#include <cstring>
#include <thread>

using ma::ui::pl;

namespace
{
constexpr int kUpdateHz = 5;
constexpr double kSmoothingSeconds = 3.0;
constexpr std::uint64_t kSpectraRequestMs = 3000;
constexpr std::uint64_t kAgentSwitchMs = 400; // Agents poll the bus at 10 Hz
constexpr double kCancellingCorrelation = -0.2;  // low-band correlation with the sidechain that loses low end
constexpr double kDuckMaxHz = 300.0;             // collisions below this are timing problems (kick vs bass); a masked kick ducks at any height
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
const char* const kRegionNames[ma::kStemRegions] { "sub", "bas", "dolny środek", "środek", "prezencja", "powietrze" };

juce::String regionText(int region)
{
    return pl(kRegionNames[region]) + " (" + hzText(ma::stemRegionEdgeHz(region)) + pl("–") + hzText(ma::stemRegionEdgeHz(region + 1)) + ")";
}

int groupOfChannel(const MixSnapshot::Channel& channel)
{
    return ma::stemGroupForRole(static_cast<ma::Role>(juce::jlimit(0, static_cast<int>(ma::Role::send), static_cast<int>(channel.data.role))));
}

// The part of the file names every stem shares: the reference track's title.
juce::String commonTitle(const juce::Array<juce::File>& files)
{
    juce::String title = files.getFirst().getFileNameWithoutExtension();
    for (const auto& file : files)
    {
        const auto name = file.getFileNameWithoutExtension();
        int same = 0;
        while (same < title.length() && same < name.length() && title[same] == name[same])
            ++same;
        title = title.substring(0, same);
    }
    title = title.trimCharactersAtEnd(" -_.([").trim();
    return title.length() >= 3 ? title : files.getFirst().getParentDirectory().getFileName();
}
} // namespace

// Analysis of one reference track's stems on a worker thread: the readers, the profiler and
// the result belong to it until `done`.
struct BrainProcessor::StemJob
{
    std::thread worker;
    std::atomic<double> progress { 0.0 };
    std::atomic<bool> done { false }, cancel { false };
    ma::StemProfile profile;
    juce::String error, warning;
    std::array<bool, ma::kStemGroups> groups {};
    bool fullMix = false;

    ~StemJob()
    {
        cancel.store(true);
        if (worker.joinable())
            worker.join();
    }

    void run(const juce::Array<juce::File>& files, const juce::String& title)
    {
        auto fail = [this](const juce::String& message) {
            error = message;
            done.store(true);
        };
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::vector<std::string> names;
        for (const auto& file : files)
            names.push_back(file.getFileName().toStdString());
        const auto kinds = ma::stemGroupsFromNames(names);

        struct Source
        {
            std::unique_ptr<juce::AudioFormatReader> reader;
            int group = 0;
        };
        std::vector<Source> sources;
        juce::StringArray unknown;
        double rate = 0.0;
        juce::int64 length = -1, longest = 0;
        for (int i = 0; i < files.size(); ++i)
        {
            const int kind = kinds[static_cast<size_t>(i)];
            if (kind == ma::kStemUnknown)
            {
                unknown.add(quoted(files[i].getFileName()));
                continue;
            }
            std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(files[i]));
            if (reader == nullptr)
                return fail(pl("Nie mogę otworzyć ") + quoted(files[i].getFileName()) + pl(" (czytam WAV, AIFF, FLAC, MP3 i OGG)."));
            if (rate <= 0.0)
                rate = reader->sampleRate;
            else if (std::abs(reader->sampleRate - rate) > 0.5)
                return fail(pl("Pliki mają różne częstotliwości próbkowania, więc nie są z jednej separacji."));
            if (kind == ma::kStemFullMix)
            {
                if (fullMix)
                    return fail(pl("Dwa pliki wyglądają na cały utwór. Zostaw jeden („mix”) albo żaden."));
                fullMix = true;
            }
            else
            {
                groups[static_cast<size_t>(kind)] = true;
            }
            length = length < 0 ? reader->lengthInSamples : std::min(length, reader->lengthInSamples);
            longest = std::max(longest, reader->lengthInSamples);
            sources.push_back({ std::move(reader), kind });
        }
        if (!unknown.isEmpty())
            return fail(pl("Nie wiem, czym jest ") + joinNames(unknown)
                        + pl(". W nazwie pliku powinno być drums, bass, vocals albo other (albo perkusja, bas, wokal, reszta), "
                             "a dla całego utworu „mix”."));
        if (std::count(groups.begin(), groups.end(), true) < 2)
            return fail(pl("Potrzebuję co najmniej dwóch stemów jednego utworu (np. perkusja i bas)."));
        if (length < static_cast<juce::int64>(rate * 15.0))
            return fail(pl("Stemy są za krótkie: potrzebuję co najmniej 15 s."));
        if (static_cast<double>(longest - length) > rate)
            warning = pl("Pliki różnią się długością o ") + juce::String(static_cast<double>(longest - length) / rate, 1)
                      + pl(" s, porównuję wspólną część. Sprawdź, czy wszystkie są z tego samego utworu.");

        ma::StemProfiler profiler;
        profiler.prepare(rate, fullMix);
        const int n = profiler.blockLength();
        juce::AudioBuffer<float> read(2, n), sum(2, n), mix(2, n), tonal(2, n); // tonal: the stems with a key
        std::array<juce::AudioBuffer<float>, ma::kStemGroups> perGroup;
        for (auto& buffer : perGroup)
            buffer.setSize(2, n);
        auto readBlock = [&](juce::int64 start, int len) {
            sum.clear();
            mix.clear();
            tonal.clear();
            for (auto& buffer : perGroup)
                buffer.clear();
            for (auto& source : sources)
            {
                read.clear();
                source.reader->read(&read, 0, len, start, true, true);
                if (source.reader->numChannels == 1)
                    read.copyFrom(1, 0, read, 0, 0, len);
                auto& into = source.group == ma::kStemFullMix ? mix : perGroup[static_cast<size_t>(source.group)];
                for (int ch = 0; ch < 2; ++ch)
                {
                    into.addFrom(ch, 0, read, ch, 0, len);
                    if (source.group != ma::kStemFullMix)
                        sum.addFrom(ch, 0, read, ch, 0, len);
                    if (ma::stemGroupHasKey(source.group))
                        tonal.addFrom(ch, 0, read, ch, 0, len);
                }
            }
        };

        const bool haveTonal = std::any_of(sources.begin(), sources.end(), [](const Source& s) { return ma::stemGroupHasKey(s.group); });
        const auto blocks = static_cast<int>((length + n - 1) / n);
        for (int b = 0; b < blocks; ++b)
        {
            if (cancel.load())
                return fail({});
            const juce::int64 start = static_cast<juce::int64>(b) * n;
            const int len = static_cast<int>(std::min<juce::int64>(n, length - start));
            readBlock(start, len);
            profiler.measureBlock(sum.getReadPointer(0), sum.getReadPointer(1), len, haveTonal ? tonal.getReadPointer(0) : nullptr,
                                  haveTonal ? tonal.getReadPointer(1) : nullptr);
            if (groups[0])
                profiler.measureDrums(perGroup[0].getReadPointer(0), perGroup[0].getReadPointer(1), len);
            progress.store(0.5 * (b + 1) / blocks);
        }
        profiler.chooseDrop();
        for (int b = 0; b < blocks; ++b)
        {
            if (cancel.load())
                return fail({});
            progress.store(0.5 + 0.5 * (b + 1) / blocks);
            if (!profiler.needed(b))
                continue;
            const juce::int64 start = static_cast<juce::int64>(b) * n;
            const int len = static_cast<int>(std::min<juce::int64>(n, length - start));
            readBlock(start, len);
            std::array<const float*, ma::kStemGroups> left {}, right {};
            for (int g = 0; g < ma::kStemGroups; ++g)
                if (groups[static_cast<size_t>(g)])
                {
                    left[static_cast<size_t>(g)] = perGroup[static_cast<size_t>(g)].getReadPointer(0);
                    right[static_cast<size_t>(g)] = perGroup[static_cast<size_t>(g)].getReadPointer(1);
                }
            profiler.analyseBlock(b, left, right, fullMix ? mix.getReadPointer(0) : nullptr, fullMix ? mix.getReadPointer(1) : nullptr, len);
        }
        profile = profiler.finish(title.toStdString());
        done.store(true);
    }
};

BrainProcessor::BrainProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "BrainState", createLayout())
{
    peakCeiling = parameters.getRawParameterValue(peakCeilingId);
    adaptive = parameters.getRawParameterValue(adaptiveId);
    masterTarget = parameters.getRawParameterValue(masterTargetId);
    for (auto& b : bands)
        b.store(-INFINITY);
    for (auto& b : averageBands)
        b.store(-INFINITY);
    for (auto* set : { &masterMid, &masterSide })
        for (auto& b : *set)
            b.store(-200.0f);
    for (auto* set : { &keyChroma, &keyBass })
        for (auto& k : *set)
            k.store(0.0f);
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
    // Phase 11: the loudness the master will get in Ozone, for the master preview.
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { masterTargetId, 3 }, "Master loudness target", juce::NormalisableRange<float> { -14.0f, -5.0f, 0.5f }, -8.0f,
        juce::AudioParameterFloatAttributes().withLabel("LUFS")));

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
    masterStereo.prepare(sampleRate);
    keyDetector.prepare(sampleRate);
    masterPreview.setGainDb(previewGainDb.load());
    masterPreview.prepare(sampleRate, channels);
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
        masterStereo.reset();
    }
    if (keyResetRequested.exchange(false))
        keyDetector.reset();
    if (previewResetRequested.exchange(false))
        masterPreview.reset();

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
        masterStereo.process(input[0], input[1], numSamples);
        const auto mid = masterStereo.recentMidDb(), side = masterStereo.recentSideDb();
        for (size_t b = 0; b < masterMid.size(); ++b)
        {
            masterMid[b].store(static_cast<float>(std::max(-200.0, mid[b])));
            masterSide[b].store(static_cast<float>(std::max(-200.0, side[b])));
        }
    }

    // The key keeps listening across resets: applying a proposal does not change it.
    keyDetector.process(input, numChannels, numSamples);
    for (size_t k = 0; k < 12; ++k)
    {
        keyChroma[k].store(static_cast<float>(keyDetector.chroma()[k]));
        keyBass[k].store(static_cast<float>(keyDetector.bassChroma()[k]));
    }
    keySeconds.store(keyDetector.secondsHeard());

    // The master preview hears the real mix too (before the listening simulation).
    masterPreview.setGainDb(previewGainDb.load());
    masterPreview.process(input, numChannels, numSamples);
    {
        const auto stats = masterPreview.stats();
        previewSeconds.store(stats.seconds);
        previewAverage.store(stats.averageReductionDb);
        previewTypical.store(stats.typicalReductionDb);
        previewMax.store(stats.maxReductionDb);
        previewHeavy.store(stats.heavyShare);
        previewLufs.store(stats.outputLufs);
        previewPeak.store(stats.outputPeakDb);
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
    accumulateDrop();
    updateKey();
    updatePreview();
    if (stemJob != nullptr && stemJob->done.load())
        finishStemJob();

    // The sum in mid and side, smoothed like the channels; only while music plays.
    {
        const double alpha = 1.0 - std::exp(-1.0 / (kSmoothingSeconds * kUpdateHz));
        const bool playing = stereo.load() && std::isfinite(momentaryLufs()) && momentaryLufs() > kAdaptMinimumLufs;
        if (playing)
        {
            for (size_t b = 0; b < mix.master.midDb.size(); ++b)
            {
                const double m = masterMid[b].load(), s = masterSide[b].load();
                mix.master.midDb[b] = mix.master.valid ? mix.master.midDb[b] + alpha * (m - mix.master.midDb[b]) : m;
                mix.master.sideDb[b] = mix.master.valid ? mix.master.sideDb[b] + alpha * (s - mix.master.sideDb[b]) : s;
            }
            mix.master.valid = true;
        }
        mix.antiPhase = ma::antiPhaseRanges(mix.master);
    }

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

    mix.stemMeasure = {};
    mix.stems = {};
    if (stemsReady())
    {
        std::array<bool, ma::kStemGroups> present {};
        std::array<ma::SpectrumAnalyzer::Bands, ma::kStemGroups> side {};
        const auto groups = stemGroupDb(true, present, &side, nullptr);
        mix.stemMeasure = ma::measureStemGroups(groups, side, present);
        mix.stems = ma::compareStems(mix.stemMeasure, keyedStems);
        // What the cleanup or the Brain's cuts took out on purpose is not asked back.
        for (int g = 0; g < ma::kStemGroups; ++g)
            for (int r = 0; r < ma::kStemRegions; ++r)
            {
                auto& d = mix.stems.groups[static_cast<size_t>(g)].shapeDeviationDb[static_cast<size_t>(r)];
                if (d < 0.0 && cutOnPurpose(g, r))
                    d = 0.0;
            }
    }
    mix.breakMeasure = {};
    mix.breakStems = {};
    mix.breakBelowDropDb = NAN;
    mix.breakSeconds = static_cast<double>(breakUpdates) / kUpdateHz;
    if (breakReady())
    {
        std::array<bool, ma::kStemGroups> present {};
        std::array<ma::SpectrumAnalyzer::Bands, ma::kStemGroups> side {};
        const auto groups = stemGroupDb(true, present, &side, nullptr, true);
        mix.breakMeasure = ma::measureStemGroups(groups, side, present);
        mix.breakStems = ma::compareStems(mix.breakMeasure, keyedBreakStems);
        mix.breakBelowDropDb = dropLevelSum / static_cast<double>(std::max(1L, dropUpdates)) - breakLevelSum / static_cast<double>(breakUpdates);
    }
    updateLoudnessMatch(now);
    adaptStep(now);
    buildReport();
}

ma::SolverResult BrainProcessor::solveNow(double headroomGainDb, const ma::StemShapeInput* stems) const
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
    options.stems = stems;
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

        // Mid and side, smoothed the same way.
        channel.stereoValid = !useProfiles && channel.data.stereoValid != 0 && channel.data.liveValid != 0;
        const bool stereoContinuing = previous != mix.channels.end() && previous->stereoValid;
        for (int b = 0; b < ma::bus::kBands; ++b)
        {
            const auto at = static_cast<size_t>(b);
            const double m = channel.data.midDb[b] + channel.data.trimDb, s = channel.data.sideDb[b] + channel.data.trimDb;
            channel.midDb[at] = stereoContinuing && channel.stereoValid ? previous->midDb[at] + alpha * (m - previous->midDb[at]) : m;
            channel.sideDb[at] = stereoContinuing && channel.stereoValid ? previous->sideDb[at] + alpha * (s - previous->sideDb[at]) : s;
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

juce::String BrainProcessor::stereoCulprit(double lowHz, double highHz) const
{
    // The channel bringing the most side into the range while being out of phase there itself.
    juce::String name;
    double most = 0.0;
    for (const auto& channel : mix.channels)
    {
        if (!channel.stereoValid)
            continue;
        double side = 0.0, worst = 1.0;
        for (int b = 0; b < ma::bus::kBands; ++b)
        {
            const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
            if (hz < lowHz || hz > highHz)
                continue;
            const auto at = static_cast<size_t>(b);
            side += std::pow(10.0, channel.sideDb[at] / 10.0);
            worst = std::min(worst, ma::coherence(channel.midDb[at], channel.sideDb[at]));
        }
        if (worst < 0.5 && side > most)
        {
            most = side;
            name = channel.name;
        }
    }
    return name;
}

void BrainProcessor::accumulateDrop()
{
    if (dropResetPending)
    {
        drop.clear();
        dropUpdates = 0;
        dropLevel = -INFINITY;
        mix.dropSeconds = 0.0;
        dropResetPending = false;
        breakSums.clear();
        breakUpdates = 0;
        dropDrumsSum = dropLevelSum = breakLevelSum = 0.0;
        // The short-term loudness needs its 3 s window refilled (and the meter its reset) first.
        dropHoldUpdates = 4 * kUpdateHz;
    }
    if (dropHoldUpdates > 0)
    {
        --dropHoldUpdates;
        return;
    }
    const double level = shortTermLufs();
    if (useProfiles || capturing || comparing || !std::isfinite(level) || level < kAdaptMinimumLufs)
        return;
    // A clearly louder part than any so far is the drop now: start over from it.
    if (level > dropLevel + 1.0)
    {
        if (std::isfinite(dropLevel))
        {
            drop.clear();
            dropUpdates = 0;
            breakSums.clear();
            breakUpdates = 0;
            dropDrumsSum = dropLevelSum = breakLevelSum = 0.0;
        }
        dropLevel = level;
    }
    dropLevel = std::max(dropLevel, level);

    // The drums group's power now (Agents' live spectra are about a second long, so the gaps
    // between kicks do not count).
    double drumsNow = 0.0;
    bool anyDrums = false;
    for (const auto& channel : mix.channels)
        if (groupOfChannel(channel) == static_cast<int>(ma::StemGroup::drums))
        {
            anyDrums = true;
            if (channel.data.liveValid != 0)
                for (int b = 0; b < ma::bus::kBands; ++b)
                    drumsNow += std::pow(10.0, (channel.data.liveDb[b] + channel.data.trimDb) / 10.0);
        }

    auto accumulate = [this](std::map<std::uint64_t, DropSum>& into) {
        for (const auto& channel : mix.channels)
        {
            if (channel.data.liveValid == 0)
                continue;
            auto& sum = into[channel.owner];
            for (int b = 0; b < ma::bus::kBands; ++b)
            {
                const auto at = static_cast<size_t>(b);
                sum.power[at] += std::pow(10.0, (channel.data.liveDb[b] + channel.data.trimDb) / 10.0);
                if (channel.data.stereoValid != 0)
                    sum.side[at] += std::pow(10.0, (channel.data.sideDb[b] + channel.data.trimDb) / 10.0);
            }
        }
    };

    if (level < dropLevel - 3.0)
    {
        // Phase 12: the break, once the drop is known: the drums 15 dB under their drop level,
        // the rest within 20 dB of the drop.
        const bool dropKnown = dropUpdates >= static_cast<long>(kMinimumDropSeconds * kUpdateHz) && dropDrumsSum > 0.0;
        if (anyDrums && dropKnown && level >= dropLevel - 20.0
            && drumsNow <= dropDrumsSum / static_cast<double>(dropUpdates) * std::pow(10.0, -1.5))
        {
            accumulate(breakSums);
            ++breakUpdates;
            breakLevelSum += level;
            mix.breakSeconds = static_cast<double>(breakUpdates) / kUpdateHz;
        }
        return;
    }
    dropDrumsSum += drumsNow;
    dropLevelSum += level;
    for (const auto& channel : mix.channels)
    {
        if (channel.data.liveValid == 0)
            continue;
        auto& sum = drop[channel.owner];
        for (int b = 0; b < ma::bus::kBands; ++b)
        {
            const auto at = static_cast<size_t>(b);
            sum.power[at] += std::pow(10.0, (channel.data.liveDb[b] + channel.data.trimDb) / 10.0);
            if (channel.data.stereoValid != 0)
                sum.side[at] += std::pow(10.0, (channel.data.sideDb[b] + channel.data.trimDb) / 10.0);
        }
    }
    ++dropUpdates;
    mix.dropSeconds = static_cast<double>(dropUpdates) / kUpdateHz;
}

bool BrainProcessor::breakReady() const
{
    return keyedBreakStems.valid() && stemsReady() && breakUpdates >= static_cast<long>(kMinimumDropSeconds * kUpdateHz);
}

bool BrainProcessor::stemsReady() const
{
    return currentStems.valid() && mix.dropSeconds >= kMinimumDropSeconds && dropUpdates > 0;
}

std::array<ma::SpectrumAnalyzer::Bands, ma::kStemGroups>
BrainProcessor::stemGroupDb(bool withEq, std::array<bool, ma::kStemGroups>& present,
                            std::array<ma::SpectrumAnalyzer::Bands, ma::kStemGroups>* side,
                            const std::vector<ma::EqSettings>* eqs, bool fromBreak) const
{
    std::array<ma::SpectrumAnalyzer::Bands, ma::kStemGroups> power {}, sidePower {};
    present = {};
    const auto& sums = fromBreak ? breakSums : drop;
    const double updates = static_cast<double>(std::max(1L, fromBreak ? breakUpdates : dropUpdates));
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        const auto& channel = mix.channels[c];
        const int group = groupOfChannel(channel);
        const auto found = sums.find(channel.owner);
        if (group < 0 || found == sums.end())
            continue;
        const auto& cleanup = channel.data.cleanup;
        ma::EqSettings eq {};
        if (eqs != nullptr && c < eqs->size())
            eq = (*eqs)[c];
        else if (withEq && channel.data.eqBypass == 0)
            for (int b = 0; b < ma::kEqBands; ++b)
                eq[static_cast<size_t>(b)] = channel.data.eq[b];
        bool any = false;
        for (int b = 0; b < ma::bus::kBands; ++b)
        {
            const auto at = static_cast<size_t>(b);
            const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
            const double gain = std::pow(10.0, (ma::cleanupResponseDb(cleanup, hz) + ma::eqResponseDb(eq, hz)) / 10.0);
            const double p = found->second.power[at] / updates * gain;
            power[static_cast<size_t>(group)][at] += p;
            sidePower[static_cast<size_t>(group)][at] += found->second.side[at] / updates * gain
                                                         * std::pow(10.0, ma::sideResponseDb(channel.data.stereo, hz) / 10.0);
            any = any || p > 1e-12;
        }
        present[static_cast<size_t>(group)] = present[static_cast<size_t>(group)] || any;
    }
    auto toDb = [](const ma::SpectrumAnalyzer::Bands& in) {
        ma::SpectrumAnalyzer::Bands out {};
        for (size_t b = 0; b < out.size(); ++b)
            out[b] = in[b] > 1e-20 ? 10.0 * std::log10(in[b]) : -200.0;
        return out;
    };
    std::array<ma::SpectrumAnalyzer::Bands, ma::kStemGroups> out {};
    for (int g = 0; g < ma::kStemGroups; ++g)
    {
        out[static_cast<size_t>(g)] = toDb(power[static_cast<size_t>(g)]);
        if (side != nullptr)
            (*side)[static_cast<size_t>(g)] = toDb(sidePower[static_cast<size_t>(g)]);
    }
    return out;
}

ma::StemShapeInput BrainProcessor::stemShapeInput() const
{
    ma::StemShapeInput in;
    if (!stemsReady())
        return in;
    in.target = &keyedStems;
    for (const auto& channel : mix.channels)
    {
        const auto found = drop.find(channel.owner);
        const int group = found != drop.end() ? groupOfChannel(channel) : -1;
        in.groupOf.push_back(group);
        ma::SpectrumAnalyzer::Bands db {}, side {};
        for (int b = 0; b < ma::bus::kBands; ++b)
        {
            const auto at = static_cast<size_t>(b);
            const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
            const double cleanup = ma::cleanupResponseDb(channel.data.cleanup, hz);
            const double p = group >= 0 ? found->second.power[at] / static_cast<double>(dropUpdates) : 0.0;
            const double sp = group >= 0 ? found->second.side[at] / static_cast<double>(dropUpdates) : 0.0;
            db[at] = p > 1e-20 ? 10.0 * std::log10(p) + cleanup : -200.0;
            side[at] = sp > 1e-20 ? 10.0 * std::log10(sp) + cleanup + ma::sideResponseDb(channel.data.stereo, hz) : -200.0;
        }
        in.dropDb.push_back(db);
        in.dropSideDb.push_back(side);
        in.lowestBoostHz.push_back(channel.data.cleanup.hpfHz > 0.0f ? 2.0 * channel.data.cleanup.hpfHz : 0.0);
    }
    return in;
}

bool BrainProcessor::cutOnPurpose(int group, int region) const
{
    // The group's drop in that region before and after its channels' cleanup (high-pass, mud)
    // and the cuts the Brain set to make room for more important channels.
    double before = 0.0, after = 0.0;
    for (const auto& channel : mix.channels)
    {
        const auto found = drop.find(channel.owner);
        if (groupOfChannel(channel) != group || found == drop.end())
            continue;
        ma::EqSettings eq {};
        if (channel.data.eqBypass == 0)
            for (int b = 0; b < ma::kEqBands; ++b)
                eq[static_cast<size_t>(b)] = channel.data.eq[b];
        for (int b = 0; b < ma::bus::kBands; ++b)
        {
            const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
            if (hz < ma::stemRegionEdgeHz(region) || hz >= ma::stemRegionEdgeHz(region + 1))
                continue;
            const double p = found->second.power[static_cast<size_t>(b)];
            before += p;
            after += p * std::pow(10.0, (ma::cleanupResponseDb(channel.data.cleanup, hz) + std::min(0.0, ma::eqResponseDb(eq, hz))) / 10.0);
        }
    }
    return before > 0.0 && after < before * 0.5;
}

juce::String BrainProcessor::stemGroupName(int group)
{
    switch (group)
    {
        case 0: return pl("Perkusja");
        case 1: return pl("Bas");
        case 2: return pl("Wokal");
        case 3: return pl("Reszta");
        default: return {};
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

    // Phase 11: will it stand being mastered loud?
    {
        MixSnapshot::ReportItem item;
        item.title = pl("Gotowość do głośnego mastera");
        item.detail = previewVerdict(item.status);
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

        // Per band, like a per-band correlation meter: which ranges fall into anti-phase, and who does it.
        MixSnapshot::ReportItem perBand;
        perBand.title = pl("Faza w pasmach");
        if (!mix.master.valid)
        {
            perBand.detail = pl("Czekam na muzykę.");
        }
        else if (mix.antiPhase.empty())
        {
            perBand.status = Status::good;
            perBand.detail = pl("Żadne pasmo powyżej 120 Hz nie wpada w przeciwfazę.");
        }
        else
        {
            perBand.status = Status::problem;
            juce::StringArray parts;
            for (const auto& range : mix.antiPhase)
            {
                auto part = hzText(range.lowHz) + pl("–") + hzText(range.highHz) + " (" + juce::String(range.worst, 2) + ")";
                const auto culprit = stereoCulprit(range.lowHz, range.highHz);
                if (culprit.isNotEmpty())
                    part += pl(", robi ją ") + quoted(culprit);
                parts.add(part);
            }
            perBand.detail = pl("Przeciwfaza w ") + parts.joinIntoString("; ")
                           + pl(". W mono te pasma osłabną. „Ustaw cały miks” zdejmie tam side na winnym kanale.");
        }
        report.push_back(perBand);
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

    // Phase 9: proportions against stem references, drop to drop.
    if (currentStems.valid())
    {
        MixSnapshot::ReportItem item;
        item.title = pl("Proporcje jak w referencjach");
        const auto setName = quoted(juce::String::fromUTF8(currentStems.name.c_str()));
        if (!mix.stems.valid)
        {
            item.detail = pl("Puść najgłośniejszą część utworu (drop): porównuję drop z dropem referencji ") + setName + ". "
                          + juce::String(juce::roundToInt(mix.dropSeconds)) + " / " + juce::String(juce::roundToInt(kMinimumDropSeconds)) + " s.";
        }
        else
        {
            juce::StringArray parts;
            double worst = 0.0;
            for (int g = 0; g < ma::kStemGroups; ++g)
            {
                const auto& c = mix.stems.groups[static_cast<size_t>(g)];
                if (!c.compared)
                    continue;
                if (std::abs(c.levelDeviationDb) >= 0.5)
                {
                    parts.add(stemGroupName(g) + pl(" o ") + juce::String(std::abs(c.levelDeviationDb), 1)
                              + (c.levelDeviationDb > 0.0 ? pl(" dB za głośno") : pl(" dB za cicho")));
                    worst = std::max(worst, std::abs(c.levelDeviationDb));
                }
                int region = -1;
                for (int r = 0; r < ma::kStemRegions; ++r)
                    if (c.shapeCompared[static_cast<size_t>(r)] && std::abs(c.shapeDeviationDb[static_cast<size_t>(r)]) >= 1.5
                        && (region < 0 || std::abs(c.shapeDeviationDb[static_cast<size_t>(r)]) > std::abs(c.shapeDeviationDb[static_cast<size_t>(region)])))
                        region = r;
                if (region >= 0)
                {
                    const double d = c.shapeDeviationDb[static_cast<size_t>(region)];
                    parts.add(stemGroupName(g) + (d < 0.0 ? pl(": za mało ") : pl(": za dużo ")) + regionText(region) + " ("
                              + juce::String(std::abs(d), 1) + " dB)");
                    worst = std::max(worst, std::abs(d) * 0.5);
                }
                if (c.widthCompared && std::abs(c.widthDeviation) >= 0.04)
                    parts.add(stemGroupName(g) + (c.widthDeviation < 0.0 ? pl(" węższa") : pl(" szersza")) + pl(" niż w referencjach (side ")
                              + juce::String(juce::roundToInt(c.width * 100.0)) + "%)");
            }
            if (parts.isEmpty())
            {
                item.status = Status::good;
                item.detail = pl("Perkusja, bas, wokal i reszta stoją jak w referencjach ") + setName + pl(" (poziom i barwa w dropie).");
            }
            else
            {
                item.status = worst >= 3.0 ? Status::problem : Status::warning;
                item.detail = parts.joinIntoString(", ") + pl(". „Ustaw cały miks” dopasuje poziomy grup i barwę szerokimi korekcjami.");
            }
        }
        report.push_back(item);
    }

    // Phase 12: the break against the references' breaks. Advice only: levels inside a section
    // are the arrangement's and the volume automation's, not the EQ's.
    if (keyedBreakStems.valid())
    {
        MixSnapshot::ReportItem item;
        item.title = pl("Break jak w referencjach");
        const bool anyDrums = std::any_of(mix.channels.begin(), mix.channels.end(),
                                          [](const auto& c) { return groupOfChannel(c) == static_cast<int>(ma::StemGroup::drums); });
        if (!anyDrums)
            item.detail = pl("Żaden kanał nie ma roli stopy ani perkusji, więc nie odróżnię breaku od dropu.");
        else if (!mix.breakStems.valid)
            item.detail = pl("Puść break (część bez stopy) po dropie: ") + juce::String(juce::roundToInt(mix.breakSeconds)) + " / "
                          + juce::String(juce::roundToInt(kMinimumDropSeconds)) + " s.";
        else
        {
            juce::StringArray parts;
            for (int g = 0; g < ma::kStemGroups; ++g)
            {
                const auto& c = mix.breakStems.groups[static_cast<size_t>(g)];
                if (c.compared && std::abs(c.levelDeviationDb) >= 1.0)
                    parts.add(stemGroupName(g) + pl(" o ") + juce::String(std::abs(c.levelDeviationDb), 1)
                              + (c.levelDeviationDb > 0.0 ? pl(" dB za głośno") : pl(" dB za cicho")));
                else if (c.extra)
                    parts.add(stemGroupName(g) + pl(" gra, w breakach referencji milknie"));
                else if (c.missing)
                    parts.add(stemGroupName(g) + pl(" milknie, w breakach referencji gra"));
            }
            const auto& contrast = keyedBreakStems.breakContrast;
            const double below = mix.breakBelowDropDb;
            if (contrast.valid && std::isfinite(below) && std::abs(contrast.outside(below)) >= 1.0)
                parts.add(pl("break jest ") + juce::String(below, 1) + pl(" dB cichszy od dropu, w referencjach ") + juce::String(contrast.low, 1)
                          + pl("–") + juce::String(contrast.high, 1) + " dB"
                          + (contrast.outside(below) < 0.0 ? pl(": drop się nie wybija") : pl(": break za bardzo siada")));
            item.status = parts.isEmpty() ? Status::good : Status::warning;
            item.detail = parts.isEmpty() ? pl("Proporcje w breaku i jego głośność względem dropu jak w referencjach.")
                                          : parts.joinIntoString(", ") + pl(". To sprawa aranżu i automatyki głośności w breaku, nie EQ.");
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
            int open = 0, ducked = 0, atLimit = 0;
            bool strong = false;
            for (const auto& issue : mix.masking.issues)
                switch (issueHandling(issue))
                {
                    case IssueHandling::open:
                        ++open;
                        strong = strong || issue.score >= 0.5;
                        break;
                    case IssueHandling::ducked: ++ducked; break;
                    case IssueHandling::cutAtLimit: ++atLimit; break;
                }
            auto collisions = [](int count) {
                return juce::String(count) + (count == 1 ? pl(" kolizja") : count < 5 ? pl(" kolizje") : pl(" kolizji"));
            };
            juce::StringArray parts;
            if (open > 0)
                parts.add(collisions(open) + pl(" do poprawienia"));
            if (ducked > 0)
                parts.add(collisions(ducked) + pl(" obsługuje ustępowanie na stopę"));
            if (atLimit > 0)
                parts.add(collisions(atLimit) + pl(" zostaje mimo cięcia do limitu: to sprawa poziomów (fader) albo aranżu"));
            item.status = open > 0 ? (strong ? Status::problem : Status::warning) : atLimit > 0 ? Status::warning : Status::good;
            item.detail = parts.joinIntoString(", ") + pl(". Szczegóły w liście poniżej mapy.");
        }
        report.push_back(item);
    }
}

namespace
{
// The duck's bell reaches about 0.7 octave / Q to each side of its centre.
bool duckCovers(const ma::DuckSettings& duck, double lowHz, double highHz)
{
    if (duck.wholeSignal > 0.5f)
        return true;
    const double reach = std::pow(2.0, 0.7 / std::max(0.3, static_cast<double>(duck.q)));
    return duck.hz / reach < highHz && duck.hz * reach > lowHz;
}
} // namespace

BrainProcessor::IssueHandling BrainProcessor::issueHandling(const ma::MaskingIssue& issue) const
{
    const auto& masker = mix.channels[static_cast<size_t>(issue.masker)].data;
    const auto& masked = mix.channels[static_cast<size_t>(issue.masked)].data;
    // The sidechain is the kick (that is how the duck proposals set it up).
    if (masked.role == 1 && masker.duck.on > 0.5f && masker.sidechainConnected != 0 && duckCovers(masker.duck, issue.lowHz, issue.highHz))
        return IssueHandling::ducked;
    if (masker.eqBypass == 0)
    {
        const double limit = ma::SolverOptions {}.maxCutDb - 0.5;
        for (const auto& band : masker.eq)
            if (band.gainDb <= -limit && band.hz >= issue.lowHz && band.hz <= issue.highHz)
                return IssueHandling::cutAtLimit;
    }
    return IssueHandling::open;
}

juce::String BrainProcessor::describeIssue(const ma::MaskingIssue& issue) const
{
    const auto& masker = mix.channels[static_cast<size_t>(issue.masker)];
    const auto& masked = mix.channels[static_cast<size_t>(issue.masked)];
    auto text = quoted(masker.name) + pl(" zasłania ") + quoted(masked.name) + pl(" w ") + hzText(issue.lowHz) + pl("–")
                + hzText(issue.highHz) + " (" + juce::String(juce::roundToInt(issue.score * 100.0)) + "%)";
    switch (issueHandling(issue))
    {
        case IssueHandling::ducked: text += pl(": ustępuje na każde uderzenie stopy, w średniej tego nie widać"); break;
        case IssueHandling::cutAtLimit: text += pl(": cięcie już na limicie, reszta to poziom ") + quoted(masker.name); break;
        case IssueHandling::open: break;
    }
    return text;
}

void BrainProcessor::propose()
{
    currentProposal = {};
    currentProposal.made = true;

    const double peak = truePeakDb();
    const double over = std::isfinite(peak) ? peak - peakCeilingDb() : 0.0;
    const auto stems = stemShapeInput();
    const auto solved = solveNow(over > 0.0 ? -std::ceil((over + 0.5) * 2.0) / 2.0 : 0.0, stems.target != nullptr ? &stems : nullptr);
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
        else if (change.kind == ma::SolverChange::Kind::stemBoost || change.kind == ma::SolverChange::Kind::stemCut)
        {
            const auto& channel = mix.channels[static_cast<size_t>(change.channel)];
            item.owner = channel.owner;
            item.band = solved.eq[static_cast<size_t>(change.channel)][static_cast<size_t>(change.eqSlot)];
            const bool boost = change.kind == ma::SolverChange::Kind::stemBoost;
            int region = 0;
            while (region < ma::kStemRegions - 1 && ma::stemRegionEdgeHz(region + 1) <= change.lowHz)
                ++region;
            item.text = quoted(channel.name) + ": " + (boost ? "+" : "") + juce::String(change.gainDb, 1) + pl(" dB przy ")
                        + hzText(item.band.hz) + " (Q " + juce::String(item.band.q, 1) + pl("), bo grupa ") + stemGroupName(change.group)
                        + (boost ? pl(" ma za mało: ") : pl(" ma za dużo: ")) + regionText(region) + ", "
                        + juce::String(std::abs(change.deviationDb), 1) + pl(" dB poza referencjami ")
                        + quoted(juce::String::fromUTF8(currentStems.name.c_str()));
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

juce::String stereoFixText(const ma::StereoPlan::Fix& fix)
{
    const auto range = hzText(fix.lowHz) + pl("–") + hzText(fix.highHz);
    if (fix.lift)
        return pl("side +") + juce::String(fix.gainDb, 1) + pl(" dB w ") + range + pl(": miks jest tam prawie mono, a kanał zostaje w fazie (")
               + juce::String(fix.coherenceBefore, 2) + pl(" → ") + juce::String(fix.coherenceAfter, 2) + ")";
    // A layer with almost no mid stays out of phase however much side goes: say where to look.
    const auto source = fix.coherenceAfter < 0.0 ? pl(". Sam kanał zostaje tam w przeciwfazie, tylko ciszej: sprawdź źródło "
                                                      "(stereo widener, chorus albo odwrócona warstwa)")
                                                 : juce::String();
    if (fix.sumOutOfPhase)
        return pl("side ") + juce::String(fix.gainDb, 1) + pl(" dB w ") + range
               + pl(", bo ciągnie cały miks w przeciwfazę (faza miksu tam ") + juce::String(fix.sumBefore, 2) + pl(" → ")
               + juce::String(fix.sumAfter, 2) + ")" + source;
    return pl("side ") + juce::String(fix.gainDb, 1) + pl(" dB w ") + range + pl(", bo kanał wpada tam w przeciwfazę (")
           + juce::String(fix.coherenceBefore, 2) + pl(" → ") + juce::String(fix.coherenceAfter, 2) + ")" + source;
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

    // Phase 8: keep every band in phase, channel by channel.
    std::vector<ma::StereoChannel> stereoIn(mix.channels.size());
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        const auto& channel = mix.channels[c];
        auto& in = stereoIn[c];
        in.role = chain[c].role;
        in.priority = channel.data.priority;
        in.valid = channel.spectrumValid && channel.stereoValid;
        for (int b = 0; b < ma::bus::kBands; ++b)
        {
            in.midDb[static_cast<size_t>(b)] = channel.midDb[static_cast<size_t>(b)];
            in.sideDb[static_cast<size_t>(b)] = channel.sideDb[static_cast<size_t>(b)];
        }
    }
    const auto stereoPlans = ma::planStereo(stereoIn, mix.master);

    // Collisions, kick and bass, translation: the usual proposal, made on the mix as it will
    // sound with the planned cleanup, so the solver does not cut what the high-pass removes.
    const auto saved = mix;
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        applyCleanup(mix.channels[c], plans[c].cleanup);
        mix.channels[c].data.cleanup = plans[c].cleanup; // the stem references see it too
    }
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
        currentProposal.verdicts.push_back(judge(c, plan, stereoPlans[c]));

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
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        const auto& channel = mix.channels[c];
        const auto& stereoPlan = stereoPlans[c];
        if (!channel.spectrumValid || !channel.stereoValid)
            continue;
        // Parameters round what the plan sets (step and skew), so "the same" allows for that.
        const bool same = std::equal(stereoPlan.settings.side.begin(), stereoPlan.settings.side.end(), channel.data.stereo.side.begin(),
                                     [](const auto& a, const auto& b) {
                                         const bool offA = std::abs(a.db) < 0.05f, offB = std::abs(b.db) < 0.05f;
                                         return (offA && offB)
                                                || (!offA && !offB && std::abs(a.db - b.db) < 0.1f && std::abs(a.q - b.q) < 0.1f
                                                    && std::abs(std::log2(a.hz / b.hz)) < 0.02);
                                     });
        if (same || (stereoPlan.fixes.empty() && !ma::isOn(channel.data.stereo)))
            continue;
        Proposal::Item item;
        item.action = Proposal::Item::Action::chain;
        item.tool = Proposal::Item::ChainTool::stereo;
        item.owner = channel.owner;
        item.stereo = stereoPlan.settings;
        juce::StringArray parts;
        for (const auto& fix : stereoPlan.fixes)
            parts.add(stereoFixText(fix));
        item.text = quoted(channel.name) + ": " + (parts.isEmpty() ? pl("mid-side EQ wyłączony, faza jest w porządku") : parts.joinIntoString("; "));
        chainItems.push_back(item);
    }

    // Phase 9: each stem group's level against the references, with the planned cleanup.
    if (stemsReady())
    {
        // With the EQ just proposed: its cuts and boosts move the groups' levels too.
        std::vector<ma::EqSettings> proposedEq;
        for (const auto& channel : mix.channels)
        {
            const auto found = std::find_if(currentProposal.eq.begin(), currentProposal.eq.end(),
                                            [&](const auto& e) { return e.first == channel.owner; });
            const bool changes = std::any_of(currentProposal.items.begin(), currentProposal.items.end(), [&](const auto& item) {
                return item.owner == channel.owner && item.action == Proposal::Item::Action::solver;
            });
            ma::EqSettings eq {};
            if (changes && found != currentProposal.eq.end())
                eq = found->second;
            else if (channel.data.eqBypass == 0)
                for (int b = 0; b < ma::kEqBands; ++b)
                    eq[static_cast<size_t>(b)] = channel.data.eq[b]; // apply keeps what is there
            proposedEq.push_back(eq);
        }
        std::array<bool, ma::kStemGroups> present {};
        const auto groups = stemGroupDb(true, present, nullptr, &proposedEq);
        const auto levels = ma::planStemLevels(groups, present, keyedStems);
        const auto setName = quoted(juce::String::fromUTF8(currentStems.name.c_str()));
        for (int g = 0; g < ma::kStemGroups && levels.any; ++g)
        {
            const double gain = levels.gainDb[static_cast<size_t>(g)];
            if (std::abs(gain) < 0.05)
                continue;
            juce::StringArray names;
            for (const auto& channel : mix.channels)
                if (groupOfChannel(channel) == g && drop.count(channel.owner) > 0)
                    names.add(quoted(channel.name));
            if (names.isEmpty())
                continue;
            const auto& corridor = keyedStems.level[static_cast<size_t>(g)];
            Proposal::Item item;
            item.action = Proposal::Item::Action::level;
            item.group = g;
            item.gainDb = gain;
            item.text = pl("Grupa ") + stemGroupName(g) + " (" + joinNames(names) + "): " + (gain > 0.0 ? "+" : "")
                        + juce::String(gain, 1) + pl(" dB. W dropie siedzi ") + juce::String(levels.before[static_cast<size_t>(g)], 1)
                        + pl(" dB względem całości, w referencjach ") + setName + " "
                        + (corridor.valid ? juce::String(corridor.low, 1) + pl(" do ") + juce::String(corridor.high, 1) + " dB"
                                          : juce::String());
            if (!corridor.valid || std::abs(levels.before[static_cast<size_t>(g)] - std::clamp(levels.before[static_cast<size_t>(g)], corridor.low, corridor.high)) < 0.05)
                item.text = pl("Grupa ") + stemGroupName(g) + " (" + joinNames(names) + "): " + (gain > 0.0 ? "+" : "")
                            + juce::String(gain, 1) + pl(" dB, żeby cały miks został tak samo głośny po zmianie innych grup");
            chainItems.insert(chainItems.begin(), item);
        }
    }
    mix = saved;

    currentProposal.items.insert(currentProposal.items.begin(), chainItems.begin(), chainItems.end());
    currentProposal.wholeMix = true;
}

Proposal::Verdict BrainProcessor::judge(size_t index, const ma::ChainPlan& plan, const ma::StereoPlan& stereoPlan) const
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

    if (!channel.stereoValid)
        verdict.reasons.add(pl("Stereo: brak pomiaru (kanał mono albo Agent jeszcze nie wysłał danych)."));
    else if (stereoPlan.fixes.empty())
        verdict.reasons.add(pl("Stereo: każde pasmo w fazie, nic do zmiany."));
    else
        for (const auto& fix : stereoPlan.fixes)
            verdict.reasons.add(pl("Stereo: ") + stereoFixText(fix) + ".");

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
        const auto handling = issueHandling(issue);
        verdict.reasons.add(pl("Zasłania go ") + quoted(masker.name) + pl(" w ") + hzText(issue.lowHz) + pl("–") + hzText(issue.highHz)
                            + " (" + juce::String(juce::roundToInt(issue.score * 100.0)) + "%)"
                            + (fixed ? pl(": poprawka jest w propozycji.")
                               : handling == IssueHandling::ducked ? pl(": ustępuje mu na każde uderzenie.")
                               : handling == IssueHandling::cutAtLimit ? pl(": cięcie już na limicie, reszta to poziom ") + quoted(masker.name) + "."
                                                                      : pl(": zostaje, solver nie znalazł cięcia, które by pomogło.")));
        if (!fixed && handling != IssueHandling::ducked && issue.score >= 0.5)
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
        const auto& masker = mix.channels[static_cast<size_t>(issue.masker)];
        const auto& masked = mix.channels[static_cast<size_t>(issue.masked)];
        // The kick plays for a moment only, so whatever covers it (its click too) gives way then.
        const bool kickMasked = masked.data.role == 1 && masker.data.role != 1;
        if (issue.score < 0.3 || (issue.highHz > kDuckMaxHz && !kickMasked))
            continue;
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
        const bool wasOn = masker.data.duck.on > 0.5f && connected;
        const bool alreadyOn = wasOn && duckCovers(masker.data.duck, issue.lowHz, issue.highHz);

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
        // Full depth on the kick's peaks: the detector reaches it 6 dB over the threshold. The
        // sidechain level is the kick's peak; in a band away from its body (the click) the
        // detector hears less. The averaged spectrum overstates the drop there (the click is
        // shorter than the body), so only part of it counts.
        double bandBelowPeakDb = 0.0;
        if (masked.spectrumValid)
        {
            const auto& kick = masked.heardDb;
            const auto loudest = *std::max_element(kick.begin(), kick.end());
            int nearest = 0;
            for (int b = 1; b < ma::bus::kBands; ++b)
                if (std::abs(std::log(ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b) / centre))
                    < std::abs(std::log(ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, nearest) / centre)))
                    nearest = b;
            bandBelowPeakDb = juce::jlimit(-20.0, 0.0, 0.6 * static_cast<double>(kick[static_cast<size_t>(nearest)] - loudest));
        }
        duck.thresholdDb = connected ? static_cast<float>(juce::jlimit(-60.0, -6.0, std::round(masker.data.sidechainDb - 18.0 + bandBelowPeakDb)))
                                     : -30.0f;
        duck.attackMs = 3.0f;
        duck.releaseMs = static_cast<float>(std::round(releaseMs));
        item.duckSync = bpm > 0.0 ? kDuckSync : 0;

        item.text = quoted(masker.name) + (wasOn ? pl(": przenosi ustępowanie, ustępuje ") : pl(": ustępuje ")) + quoted(masked.name) + pl(" dynamicznie, do -")
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

juce::StringArray BrainProcessor::stemTargetNames() const
{
    juce::StringArray names;
    for (const auto& file : targetFolder().findChildFiles(juce::File::findFiles, false, "*.mastems"))
    {
        ma::StemTarget t;
        if (ma::parseStemTarget(file.loadFileAsString().toStdString(), t))
            names.add(juce::String::fromUTF8(t.name.c_str()));
    }
    names.sortNatural();
    return names;
}

bool BrainProcessor::selectStemTarget(const juce::String& name)
{
    for (const auto& file : targetFolder().findChildFiles(juce::File::findFiles, false, "*.mastems"))
    {
        ma::StemTarget t;
        if (ma::parseStemTarget(file.loadFileAsString().toStdString(), t) && juce::String::fromUTF8(t.name.c_str()) == name)
        {
            currentStems = t;
            refreshKeyedStems();
            parameters.state.setProperty("stems", name, nullptr);
            return true;
        }
    }
    return false;
}

bool BrainProcessor::createStemTarget(const juce::String& name)
{
    const auto trimmed = name.trim();
    if (trimmed.isEmpty() || stemTargetNames().contains(trimmed))
        return false;
    currentStems = {};
    currentStems.name = trimmed.toStdString();
    refreshKeyedStems();
    parameters.state.setProperty("stems", trimmed, nullptr);
    saveStemTarget();
    return true;
}

void BrainProcessor::saveStemTarget()
{
    if (currentStems.name.empty())
        return;
    const auto folder = targetFolder();
    folder.createDirectory();
    const auto fileName = juce::File::createLegalFileName(juce::String::fromUTF8(currentStems.name.c_str())) + ".mastems";
    folder.getChildFile(fileName).replaceWithText(ma::serialiseStemTarget(currentStems), false, false, "\n");
}

void BrainProcessor::removeLastStemProfile()
{
    if (currentStems.profiles.empty())
        return;
    currentStems.profiles.pop_back();
    currentStems.rebuild();
    refreshKeyedStems();
    saveStemTarget();
    stemNote = {};
}

bool BrainProcessor::loadStems(const juce::Array<juce::File>& files)
{
    if (stemJob != nullptr || currentStems.name.empty() || files.isEmpty())
        return false;
    stemJob = std::make_unique<StemJob>();
    stemNote = pl("Analizuję stemy...");
    stemWarning = false;
    auto* job = stemJob.get();
    const auto title = commonTitle(files);
    job->worker = std::thread([job, files, title] { job->run(files, title); });
    return true;
}

double BrainProcessor::stemProgress() const
{
    return stemJob != nullptr ? stemJob->progress.load() : 0.0;
}

void BrainProcessor::finishStemJob()
{
    std::unique_ptr<StemJob> job = std::move(stemJob);
    if (job->worker.joinable())
        job->worker.join();
    if (job->error.isNotEmpty())
    {
        stemNote = job->error;
        stemWarning = true;
        return;
    }
    const auto& profile = job->profile;
    const auto title = quoted(juce::String::fromUTF8(profile.name.c_str()));
    if (!profile.measure.valid || profile.dropSeconds < kMinimumDropSeconds)
    {
        stemNote = pl("W ") + title + pl(" nie znalazłem głośnej części (drop) dłuższej niż ")
                   + juce::String(juce::roundToInt(kMinimumDropSeconds)) + pl(" s. Nie dodaję.");
        stemWarning = true;
        return;
    }
    // Stems that do not add up to the track would teach the wrong proportions.
    if (profile.separationErrorDb > 3.0)
    {
        stemNote = pl("Stemy ") + title + pl(" nie składają się na cały utwór (średnio ") + juce::String(profile.separationErrorDb, 1)
                   + pl(" dB różnicy): brakuje któregoś albo są z innej wersji. Nie dodaję.");
        stemWarning = true;
        return;
    }

    currentStems.profiles.push_back(profile);
    currentStems.rebuild();
    refreshKeyedStems();
    saveStemTarget();

    juce::StringArray groups, missing;
    for (int g = 0; g < ma::kStemGroups; ++g)
        (profile.measure.groups[static_cast<size_t>(g)].present ? groups : missing).add(stemGroupName(g).toLowerCase());
    stemNote = pl("Dodano ") + title + pl(": drop ") + juce::String(juce::roundToInt(profile.dropSeconds)) + " s, "
               + groups.joinIntoString(", ") + (missing.isEmpty() ? juce::String() : pl(" (bez: ") + missing.joinIntoString(", ") + ")") + ", "
               + (profile.key.valid() ? pl("tonacja ") + keyText(profile.key) + ". "
                                      : pl("tonacji nie rozpoznałem pewnie, więc tej referencji nie przesuwam do Twojej. "))
               + (profile.breakdown.valid ? pl("Break ") + juce::String(juce::roundToInt(profile.breakdown.seconds)) + pl(" s, ")
                                                + juce::String(profile.breakdown.belowDropDb, 1) + pl(" dB ciszej niż drop. ")
                                          : pl("Breaku nie znalazłem (potrzebny stem perkusji i co najmniej 8 s bez niej). "));
    stemWarning = false;
    if (profile.separationErrorDb < 0.0)
    {
        stemNote += pl("Bez pliku całego utworu („mix”) nie sprawdzę separacji.");
    }
    else if (profile.separationErrorDb > 1.0)
    {
        stemNote += pl("Uwaga: stemy różnią się od utworu średnio o ") + juce::String(profile.separationErrorDb, 1)
                    + pl(" dB, separacja jest słaba. Lepiej użyć innego separatora albo innej referencji.");
        stemWarning = true;
    }
    else
    {
        stemNote += pl("Separacja sprawdzona (") + juce::String(profile.separationErrorDb, 1) + pl(" dB różnicy od utworu).");
    }
    if (job->warning.isNotEmpty())
    {
        stemNote += " " + job->warning;
        stemWarning = true;
    }
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

    // Phase 9: a stem group's level moves every channel of the group, its thresholds with it.
    std::array<double, ma::kStemGroups> groupGain {};
    for (const auto& item : currentProposal.items)
        if (item.selected && item.action == Proposal::Item::Action::level && item.group >= 0 && item.group < ma::kStemGroups)
            groupGain[static_cast<size_t>(item.group)] += item.gainDb;

    int sent = 0;
    for (const auto& channel : mix.channels)
    {
        const int group = groupOfChannel(channel);
        const double gain = globalGain + (group >= 0 ? groupGain[static_cast<size_t>(group)] : 0.0);
        // A duck listens to the kick: its threshold follows the drums.
        const double kickGain = globalGain + groupGain[static_cast<size_t>(ma::StemGroup::drums)];
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
                                || item.change.kind == ma::SolverChange::Kind::tonalCut
                                || item.change.kind == ma::SolverChange::Kind::stemBoost
                                || item.change.kind == ma::SolverChange::Kind::stemCut);
            if (!item.selected || !eqKind || item.owner != channel.owner)
                continue;
            if (proposed != currentProposal.eq.end())
                eq[static_cast<size_t>(item.change.eqSlot)] = proposed->second[static_cast<size_t>(item.change.eqSlot)];
            eqChanges = true;
        }
        auto duck = channel.data.duck;
        if (duck.on > 0.5f)
            duck.thresholdDb = static_cast<float>(juce::jlimit(-60.0, 0.0, duck.thresholdDb + kickGain - globalGain));
        auto duckSync = channel.data.duckSync;
        auto polarity = channel.data.polarityInverted;
        auto harmonics = channel.data.harmonics;
        auto transients = channel.data.transients;
        auto cleanup = channel.data.cleanup;
        auto compressor = channel.data.compressor;
        auto clipper = channel.data.clipper;
        auto evener = channel.data.evener;
        auto stereoEq = channel.data.stereo;
        bool dynamicChanges = false;
        for (const auto& item : currentProposal.items)
        {
            if (!item.selected || item.owner != channel.owner)
                continue;
            if (item.action == Proposal::Item::Action::duck)
            {
                duck = item.duck;
                // The threshold follows the kick's level, which the global gain moves too.
                duck.thresholdDb = static_cast<float>(juce::jlimit(-60.0, 0.0, duck.thresholdDb + kickGain));
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
                    case Proposal::Item::ChainTool::stereo: stereoEq = item.stereo; break;
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

        if (!eqChanges && !dynamicChanges && std::abs(gain) < 0.05 && std::abs(kickGain - globalGain) < 0.05)
            continue;
        if (!eqChanges)
            for (int b = 0; b < ma::kEqBands; ++b)
                eq[static_cast<size_t>(b)] = channel.data.eq[b]; // keep what is there

        ma::bus::Command command {};
        command.target = channel.owner;
        command.serial = ++commandSerial;
        command.kind = ma::bus::CommandKind::apply;
        command.trimDb = static_cast<float>(juce::jlimit(-24.0, 24.0, channel.data.trimDb + gain));
        for (int b = 0; b < ma::kEqBands; ++b)
            command.eq[b] = eq[static_cast<size_t>(b)];
        command.duck = duck;
        command.duckSync = duckSync;
        command.polarityInverted = polarity;
        command.harmonics = harmonics;
        command.transients = transients;
        // The trim moves with the gain, and the compressor and clipper levels with it.
        compressor.thresholdDb = static_cast<float>(juce::jlimit(-60.0, 0.0, compressor.thresholdDb + gain));
        clipper.ceilingDb = static_cast<float>(juce::jlimit(-30.0, 0.0, clipper.ceilingDb + gain));
        command.cleanup = cleanup;
        command.compressor = compressor;
        command.clipper = clipper;
        command.evener = evener;
        command.stereo = stereoEq;
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
    // The key heard so far goes with the project, so it is known again right after loading.
    juce::StringArray chroma;
    for (size_t k = 0; k < 12; ++k)
        chroma.add(juce::String(savedChroma[k] + keyChroma[k].load(), 6));
    for (size_t k = 0; k < 12; ++k)
        chroma.add(juce::String(savedBass[k] + keyBass[k].load(), 6));
    parameters.state.setProperty("keyChroma", chroma.joinIntoString(" "), nullptr);
    parameters.state.setProperty("keySeconds", savedKeySeconds + keySeconds.load(), nullptr);
    parameters.state.setProperty("keyOverride", keyOverride, nullptr);
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
            const auto stems = parameters.state.getProperty("stems").toString();
            if (stems.isNotEmpty())
                selectStemTarget(stems);
            auto chroma = juce::StringArray::fromTokens(parameters.state.getProperty("keyChroma").toString(), " ", "");
            chroma.removeEmptyStrings();
            savedChroma = {};
            savedBass = {};
            savedKeySeconds = 0.0;
            if (chroma.size() == 24)
            {
                for (int k = 0; k < 12; ++k)
                {
                    savedChroma[static_cast<size_t>(k)] = std::max(0.0, chroma[k].getDoubleValue());
                    savedBass[static_cast<size_t>(k)] = std::max(0.0, chroma[k + 12].getDoubleValue());
                }
                savedKeySeconds = std::max(0.0, static_cast<double>(parameters.state.getProperty("keySeconds", 0.0)));
                // What the master played before loading belongs to another project.
                keyResetRequested.store(true);
                for (auto* set : { &keyChroma, &keyBass })
                    for (auto& k : *set)
                        k.store(0.0f);
                keySeconds.store(0.0);
            }
            keyOverride = juce::jlimit(-1, 23, static_cast<int>(parameters.state.getProperty("keyOverride", -1)));
            updateKey();
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

// Phase 10: the track's key.

juce::String BrainProcessor::keyText(ma::MusicalKey key)
{
    if (!key.valid())
        return "?";
    return juce::String(ma::keyName(key)) + " (" + juce::String(ma::camelotCode(key)) + ")";
}

ma::MusicalKey BrainProcessor::trackKey() const
{
    if (keyOverride >= 0)
        return ma::keyFromIndex(keyOverride);
    if (heardKeySeconds >= kMinimumKeySeconds && heardKey.confident)
        return heardKey.key;
    return {};
}

void BrainProcessor::setKeyOverride(int index)
{
    keyOverride = juce::jlimit(-1, 23, index);
    updateKey();
}

void BrainProcessor::restartKeyDetection()
{
    savedChroma = {};
    savedBass = {};
    savedKeySeconds = 0.0;
    keyResetRequested.store(true);
    for (auto* set : { &keyChroma, &keyBass })
        for (auto& k : *set)
            k.store(0.0f);
    keySeconds.store(0.0);
    updateKey();
}

void BrainProcessor::updateKey()
{
    std::array<double, 12> chroma {}, bass {};
    for (size_t k = 0; k < 12; ++k)
    {
        chroma[k] = savedChroma[k] + keyChroma[k].load();
        bass[k] = savedBass[k] + keyBass[k].load();
    }
    heardKey = ma::estimateKey(chroma, bass);
    heardKeySeconds = savedKeySeconds + keySeconds.load();
    if (trackKey() != keyedTo)
        refreshKeyedStems();
}

void BrainProcessor::refreshKeyedStems()
{
    keyedTo = trackKey();
    keyedStems = ma::keyedStemTarget(currentStems, keyedTo);
    keyedBreakStems = ma::stemBreakTarget(keyedStems);
}

// Phase 11: the master preview and the report for mastering.

ma::PreviewStats BrainProcessor::previewStats() const
{
    ma::PreviewStats s;
    s.seconds = previewSeconds.load();
    s.averageReductionDb = previewAverage.load();
    s.typicalReductionDb = previewTypical.load();
    s.maxReductionDb = previewMax.load();
    s.heavyShare = previewHeavy.load();
    s.outputLufs = previewLufs.load();
    s.outputPeakDb = previewPeak.load();
    return s;
}

void BrainProcessor::updatePreview()
{
    loudestShortTerm = std::max(loudestShortTerm, shortTermLufs());
    // Agents publish the peak of their latest block; held here, so the report knows whose peaks reach highest.
    for (const auto& channel : mix.channels)
        if (std::isfinite(channel.data.outputPeakDb))
        {
            auto [it, added] = peakHold.try_emplace(channel.owner, channel.data.outputPeakDb);
            if (!added)
                it->second = std::max(it->second, channel.data.outputPeakDb);
        }
    const double integratedNow = integratedLufs();
    if (!std::isfinite(integratedNow) || integratedNow < -60.0)
        return;
    const double wanted = juce::jlimit(-12.0, 24.0, static_cast<double>(masterTargetLufs()) - integratedNow);
    // A big move (a new target, the first reading) starts the statistics over; small drifts of
    // the integrated loudness just follow.
    if (std::isnan(appliedPreviewGain) || std::abs(wanted - appliedPreviewGain) > 1.0)
        previewResetRequested.store(true);
    else if (std::abs(wanted - appliedPreviewGain) < 0.1)
        return;
    appliedPreviewGain = wanted;
    previewGainDb.store(wanted);
}

juce::String BrainProcessor::loudestPeakChannel() const
{
    juce::String loudest;
    float highest = -100.0f;
    for (const auto& channel : mix.channels)
    {
        const auto held = peakHold.find(channel.owner);
        if (held != peakHold.end() && held->second > highest)
        {
            highest = held->second;
            loudest = channel.name;
        }
    }
    return loudest;
}

juce::String BrainProcessor::previewVerdict(MixSnapshot::Status& status) const
{
    const auto s = previewStats();
    const double target = masterTargetLufs();
    const auto targetText = juce::String(target, 1) + " LUFS";
    status = MixSnapshot::Status::unknown;
    if (s.seconds < kMinimumPreviewSeconds)
        return pl("Słucham: sprawdzam, jak miks zniesie podciągnięcie do ") + targetText + " ("
               + juce::String(juce::roundToInt(s.seconds)) + " / " + juce::String(juce::roundToInt(kMinimumPreviewSeconds)) + " s).";
    const auto base = pl("Podciągnięty do ") + targetText + pl(" limiter ścina w głośnej części do ")
                      + juce::String(s.typicalReductionDb, 1) + pl(" dB (najwięcej ") + juce::String(s.maxReductionDb, 1) + pl(" dB)");
    const auto peaks = loudestPeakChannel();
    const auto advice = (peaks.isNotEmpty() ? pl(" Najwyższe szczyty daje ") + quoted(peaks) + pl(": clipper na nim („Ustaw cały miks”)")
                                            : pl(" Clipper na kanałach z najwyższymi szczytami („Ustaw cały miks”)"))
                        + pl(" albo niższy cel, np. ") + juce::String(target - 2.0, 1) + " LUFS.";
    // A limiter working that hard does not even get there.
    const auto shortOf = std::isfinite(s.outputLufs) && s.outputLufs < target - 1.0
                             ? pl(" I tak nie dobije do celu: wychodzi ") + juce::String(s.outputLufs, 1) + " LUFS."
                             : juce::String();
    if (s.typicalReductionDb <= 3.0)
    {
        status = MixSnapshot::Status::good;
        return base + pl(". Uderzenie zostanie, miks zniesie głośny master.") + shortOf;
    }
    if (s.typicalReductionDb <= 6.0)
    {
        status = MixSnapshot::Status::warning;
        return base + pl(". Stopa i perkusja stracą trochę uderzenia.") + shortOf + advice;
    }
    status = MixSnapshot::Status::problem;
    return base + pl(". To za dużo: master zgniecie uderzenie.") + shortOf + advice;
}

namespace
{
juce::String html(const juce::String& text)
{
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("\"", "&quot;");
}

juce::String statusClass(MixSnapshot::Status status)
{
    switch (status)
    {
        case MixSnapshot::Status::good: return "good";
        case MixSnapshot::Status::warning: return "warning";
        case MixSnapshot::Status::problem: return "problem";
        case MixSnapshot::Status::unknown: return "unknown";
    }
    return "unknown";
}

juce::String signedDb(double db)
{
    return (db > 0.0 ? "+" : "") + juce::String(db, 1) + " dB";
}
} // namespace

juce::String BrainProcessor::masteringReportHtml() const
{
    const double integratedNow = integratedLufs(), peak = truePeakDb(), ceiling = peakCeilingDb();
    const double target = masterTargetLufs();
    const auto s = previewStats();
    MixSnapshot::Status previewStatus;
    const auto previewText = previewVerdict(previewStatus);
    const auto number = [](double v, const char* unit) { return std::isfinite(v) ? juce::String(v, 1) + " " + unit : juce::String("-"); };

    juce::String out;
    out << "<!doctype html>\n<html lang=\"pl\"><head><meta charset=\"utf-8\">"
        << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        << "<title>" << pl("Raport przed masteringiem") << "</title><style>"
        << ":root{--bg:#f6f7f9;--panel:#fff;--text:#1d2129;--quiet:#667085;--line:#e3e6eb;--good:#2e9d5b;--warning:#c98a12;--problem:#d0443e}"
        << "@media (prefers-color-scheme: dark){:root{--bg:#16181d;--panel:#1f232a;--text:#e7e9ee;--quiet:#9aa3b2;--line:#2e333c}}"
        << "body{margin:0;background:var(--bg);color:var(--text);font:15px/1.5 system-ui,-apple-system,Segoe UI,sans-serif}"
        << "main{max-width:880px;margin:0 auto;padding:24px 16px 48px}h1{font-size:24px;margin:0 0 4px}"
        << "h2{font-size:17px;margin:28px 0 8px}.meta{color:var(--quiet);margin:0 0 16px}"
        << "section{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:12px 16px;margin:10px 0}"
        << "table{border-collapse:collapse;width:100%}td,th{text-align:left;padding:4px 8px 4px 0;border-bottom:1px solid var(--line);vertical-align:top}"
        << "th{color:var(--quiet);font-weight:600}.num{text-align:right;white-space:nowrap}"
        << ".dot{display:inline-block;width:9px;height:9px;border-radius:50%;margin-right:8px;background:var(--quiet)}"
        << ".good .dot,.dot.good{background:var(--good)}.warning .dot,.dot.warning{background:var(--warning)}.problem .dot,.dot.problem{background:var(--problem)}"
        << "ol li{margin:4px 0}.wrap{overflow-x:auto}</style></head><body><main>\n";

    out << "<h1>" << pl("Raport przed masteringiem") << "</h1><p class=\"meta\">Mix Assistant " << pl("· ")
        << juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M") << pl(" · miks słuchany ") << juce::String(juce::roundToInt(secondsAnalysed()))
        << " s</p>\n";

    // What Ozone has to do, first: the engineer reads this while mastering.
    out << "<h2>" << pl("Dla Ozone") << "</h2><section>";
    // Red items are the mix's to fix, not the master's.
    juce::StringArray first;
    for (const auto& item : mix.report)
        if (item.status == MixSnapshot::Status::problem)
            first.add(item.title);
    if (!first.isEmpty())
        out << "<p class=\"problem\"><span class=\"dot\"></span>" << pl("Najpierw popraw w miksie (szczegóły w raporcie niżej): ")
            << html(first.joinIntoString(", ")) << ".</p>";
    out << "<ol>";
    out << "<li>" << pl("Maximizer: cel ") << juce::String(target, 1) << pl(" LUFS integrated, sufit -1 dBTP.");
    if (std::isfinite(integratedNow))
        out << pl(" Miks ma ") << juce::String(integratedNow, 1) << pl(" LUFS, więc trzeba go podnieść o ok. ")
            << juce::String(target - integratedNow, 1) << " dB.";
    if (s.seconds >= kMinimumPreviewSeconds)
        out << pl(" Podgląd: limiter ścina w głośnej części do ") << juce::String(s.typicalReductionDb, 1) << " dB.";
    out << "</li>";
    if (mix.tonal.valid && !mix.tonal.regions.empty())
    {
        out << "<li>" << pl("EQ na sumie (szeroko, względem celu ") << html(quoted(juce::String::fromUTF8(currentTarget.name.c_str()))) << "): ";
        juce::StringArray parts;
        for (const auto& region : mix.tonal.regions)
            parts.add((region.hole() ? "+" : pl("−")) + juce::String(std::abs(region.deviationDb), 1) + " dB " + hzText(region.lowHz) + pl("–")
                      + hzText(region.highHz));
        out << html(parts.joinIntoString(", ")) << pl(". Lepiej poprawić to w miksie, jeśli to jeden kanał.") << "</li>";
    }
    else if (mix.tonal.valid)
    {
        out << "<li>" << pl("EQ na sumie: balans mieści się w korytarzu celu ") << html(quoted(juce::String::fromUTF8(currentTarget.name.c_str())))
            << pl(", wystarczą drobne korekty do gustu.") << "</li>";
    }
    if (stereo.load())
        out << "<li>" << pl("Imager: dół poniżej 120 Hz ma korelację ") << juce::String(lowCorrelation(), 2)
            << (lowCorrelation() >= 0.7 ? pl(" (mono, nie poszerzaj go).") : pl(": zawęź go w miksie, a w Ozone ustaw dół na mono."))
            << (mix.antiPhase.empty() ? juce::String() : pl(" Nie poszerzaj pasm w przeciwfazie (niżej)."))
            << "</li>";
    if (!keyedStems.name.empty() && keyedStems.valid())
    {
        juce::StringArray refs;
        for (const auto& p : keyedStems.profiles)
            refs.add(quoted(juce::String::fromUTF8(p.name.c_str())));
        out << "<li>" << pl("Referencje do porównania w Ozone: ") << html(refs.joinIntoString(", ")) << pl(" (zestaw ")
            << html(quoted(juce::String::fromUTF8(keyedStems.name.c_str()))) << ").</li>";
    }
    out << "</ol></section>\n";

    out << "<h2>" << pl("Głośność i zapas") << "</h2><section><table>"
        << "<tr><td>LUFS integrated</td><td class=\"num\">" << number(integratedNow, "LUFS") << "</td></tr>"
        << pl("<tr><td>Najgłośniejszy fragment (short-term)</td><td class=\"num\">") << number(loudestShortTerm, "LUFS") << "</td></tr>"
        << "<tr><td>True peak (max)</td><td class=\"num\">" << number(peak, "dBTP") << "</td></tr>"
        << "<tr><td>PLR</td><td class=\"num\">" << (std::isfinite(peak) && std::isfinite(integratedNow) ? juce::String(peak - integratedNow, 1) + " dB" : "-")
        << "</td></tr>"
        << pl("<tr><td>Limit szczytu przed masteringiem</td><td class=\"num\">") << juce::String(ceiling, 1) << " dBTP</td></tr>"
        << "</table></section>\n";

    out << "<h2>" << pl("Podgląd mastera do ") << juce::String(target, 1) << " LUFS</h2><section class=\"" << statusClass(previewStatus)
        << "\"><p><span class=\"dot\"></span>" << html(previewText) << "</p>";
    if (s.seconds >= kMinimumPreviewSeconds)
        out << "<table><tr><td>" << pl("Średnio ścina") << "</td><td class=\"num\">" << juce::String(s.averageReductionDb, 1) << " dB</td></tr>"
            << "<tr><td>" << pl("W głośnej części (95% czasu poniżej)") << "</td><td class=\"num\">" << juce::String(s.typicalReductionDb, 1) << " dB</td></tr>"
            << "<tr><td>" << pl("Najwięcej") << "</td><td class=\"num\">" << juce::String(s.maxReductionDb, 1) << " dB</td></tr>"
            << "<tr><td>" << pl("Czas z ponad 3 dB ścinania") << "</td><td class=\"num\">" << juce::String(juce::roundToInt(s.heavyShare * 100.0))
            << " %</td></tr></table><p class=\"meta\">"
            << pl("Szybkie przybliżenie (limiter z 5 ms wyprzedzenia, sufit -1 dBFS), tylko do analizy. Ozone z clipperem i wielopasmowym "
                  "limiterem zrobi to łagodniej, ale proporcje się zgadzają.")
            << "</p>";
    out << "</section>\n";

    out << "<h2>" << pl("Raport poprawności") << "</h2><section><table>";
    for (const auto& item : mix.report)
        out << "<tr class=\"" << statusClass(item.status) << "\"><td><span class=\"dot\"></span><b>" << html(item.title) << "</b><br>"
            << html(item.detail) << "</td></tr>";
    out << "</table></section>\n";

    if (stereo.load())
    {
        out << "<h2>Stereo</h2><section><p>" << pl("Korelacja poniżej 120 Hz: ") << juce::String(lowCorrelation(), 2) << pl(", całość: ")
            << juce::String(fullCorrelation(), 2) << ".</p>";
        if (mix.antiPhase.empty())
            out << "<p>" << pl("Żadne pasmo nie wpada w przeciwfazę.") << "</p>";
        else
        {
            out << "<p>" << pl("Pasma w przeciwfazie:") << "</p><ul>";
            for (const auto& range : mix.antiPhase)
                out << "<li>" << html(hzText(range.lowHz) + pl("–") + hzText(range.highHz)) << pl(" (najgorzej ") << juce::String(range.worst, 2) << ")</li>";
            out << "</ul>";
        }
        out << "</section>\n";
    }

    const auto groupTable = [](const ma::StemComparison& comparison) {
        static const char* const regionNames[ma::kStemRegions] { "sub", "bas", "dolny środek", "środek", "prezencja", "powietrze" };
        juce::String t;
        t << "<div class=\"wrap\"><table><tr><th>" << pl("Grupa") << "</th><th class=\"num\">" << pl("Poziom") << "</th><th class=\"num\">"
          << pl("Poza korytarzem") << "</th><th>" << pl("Barwa poza korytarzem") << "</th></tr>";
        for (int g = 0; g < ma::kStemGroups; ++g)
        {
            const auto& c = comparison.groups[static_cast<size_t>(g)];
            if (!c.compared && !c.missing)
                continue;
            juce::StringArray shape;
            for (int r = 0; r < ma::kStemRegions; ++r)
                if (c.shapeCompared[static_cast<size_t>(r)] && std::abs(c.shapeDeviationDb[static_cast<size_t>(r)]) >= 1.5)
                    shape.add(pl(regionNames[r]) + " " + signedDb(c.shapeDeviationDb[static_cast<size_t>(r)]));
            t << "<tr><td>" << html(stemGroupName(g)) << "</td><td class=\"num\">" << (c.compared ? juce::String(c.levelDb, 1) + " dB" : pl("brak"))
              << "</td><td class=\"num\">" << (c.compared ? signedDb(c.levelDeviationDb) : juce::String("-")) << "</td><td>"
              << html(shape.isEmpty() ? juce::String("ok") : shape.joinIntoString(", ")) << "</td></tr>";
        }
        t << "</table></div>";
        return t;
    };
    if (keyedStems.valid() && mix.stems.valid)
    {
        out << "<h2>" << pl("Proporcje jak w referencjach (drop do dropu)") << "</h2><section>";
        const auto key = trackKey();
        if (key.valid())
            out << "<p>" << pl("Tonacja utworu: ") << html(keyText(key)) << pl(". Referencje przesunięte do niej.") << "</p>";
        out << groupTable(mix.stems) << "</section>\n";
    }
    if (keyedBreakStems.valid() && mix.breakStems.valid)
    {
        out << "<h2>" << pl("Break jak w referencjach") << "</h2><section><p>" << pl("Break miksu jest ") << juce::String(mix.breakBelowDropDb, 1)
            << pl(" dB cichszy od dropu");
        if (keyedBreakStems.breakContrast.valid)
            out << pl(", w referencjach ") << juce::String(keyedBreakStems.breakContrast.low, 1) << pl("–")
                << juce::String(keyedBreakStems.breakContrast.high, 1) << " dB";
        out << pl(". Różnice w breaku poprawia aranż i automatyka głośności, nie EQ.") << "</p>" << groupTable(mix.breakStems) << "</section>\n";
    }

    out << "<h2>" << pl("Kanały") << "</h2><section class=\"wrap\"><table><tr><th>" << pl("Kanał") << "</th><th>" << pl("Rola")
        << "</th><th class=\"num\">Trim</th><th class=\"num\">" << pl("Szczyt") << "</th><th>" << pl("Łańcuch") << "</th></tr>";
    const auto roles = ma::ui::roleNames();
    for (const auto& channel : mix.channels)
    {
        const auto& d = channel.data;
        juce::StringArray chain;
        if (d.cleanup.hpfHz > 0.0f)
            chain.add("HPF " + hzText(d.cleanup.hpfHz));
        if (d.cleanup.monoBelowHz > 0.0f)
            chain.add(pl("mono poniżej ") + hzText(d.cleanup.monoBelowHz));
        int eqBands = 0;
        for (const auto& band : d.eq)
            eqBands += std::abs(band.gainDb) > 0.05f ? 1 : 0;
        if (eqBands > 0)
            chain.add("EQ " + juce::String(eqBands) + pl(" pasm"));
        if (d.compressor.on > 0.5f)
            chain.add(pl("kompresor ") + juce::String(d.compressor.ratio, 1) + ":1");
        if (d.clipper.on > 0.5f)
            chain.add("clipper " + juce::String(d.clipper.ceilingDb, 1) + " dBFS");
        if (d.duck.on > 0.5f)
            chain.add(pl("ustępowanie"));
        out << "<tr><td>" << html(channel.name) << "</td><td>" << html(roles[juce::jlimit(0, roles.size() - 1, static_cast<int>(d.role))])
            << "</td><td class=\"num\">" << signedDb(d.trimDb) << "</td><td class=\"num\">"
            << [&] {
                   const auto held = peakHold.find(channel.owner);
                   return held != peakHold.end() && held->second > -100.0f ? juce::String(held->second, 1) + " dBFS" : juce::String("-");
               }()
            << "</td><td>"
            << html(chain.isEmpty() ? juce::String("-") : chain.joinIntoString(", ")) << "</td></tr>";
    }
    out << "</table></section>\n</main></body></html>\n";
    return out;
}

bool BrainProcessor::saveMasteringReport(const juce::File& file) const
{
    return file.replaceWithText(masteringReportHtml(), false, false, "\n");
}
