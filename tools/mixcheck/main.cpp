// mixcheck: the plugins' own Agents and Brain on real multitrack stems, offline.
//
//   mixcheck --stems DIR [--reference MIX] [--target-refs A B ...] [--bpm N] [--out DIR]
//
// One Agent per stem, one Brain on their sum, run the way they run in FL: every Agent learns its
// level on the drop (the loudest 16 s of the plain sum, looped), the Brain measures the drop,
// "Analizuj" on the whole chain, "Zastosuj", then the refining rounds by themselves. The whole song
// is rendered with the Agents as they ended up (ours.wav) and as a plain sum (raw.wav), and both
// are compared with the reference mix over its own drop at matched loudness: tonal balance in the
// four ranges Tonal Balance Control shows, loudness, peaks, dynamics and width. report.md holds the
// numbers, the Brain's report, its proposal and its decision per tool and channel.
//
// The plugins' timers are ticked by the rendered audio on a test clock (`ma::clock::testMs`), so a
// run is faster than real time and gives the same numbers every time.

#include "AgentProcessor.h"
#include "BrainProcessor.h"
#include "MasterProcessor.h"
#include "Clock.h"
#include "MaUi.h"

#include "ma/LoudnessMeter.h"
#include "ma/SpectrumAnalyzer.h"
#include "ma/TonalBalance.h"
#include "ma/TruePeakMeter.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <vector>

using namespace juce;

namespace
{
constexpr int kBlock = 512;
constexpr double kDropSeconds = 16.0;

struct Transport : AudioPlayHead
{
    double sampleRate = 48000.0;
    double bpm = 124.0;
    int64 position = 0;
    bool playing = true;

    Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm(bpm);
        info.setIsPlaying(playing);
        info.setTimeInSamples(position);
        info.setTimeInSeconds(static_cast<double>(position) / sampleRate);
        info.setPpqPosition(static_cast<double>(position) / sampleRate * bpm / 60.0);
        return info;
    }
};

struct Track
{
    String name;
    AudioBuffer<float> audio; // stereo
    int role = 0;             // ma::Role, 0 = let the Learn pass recognise it
    ma::SpectrumAnalyzer::Bands dropDb {}; // ERB over the drop as it comes in, the way an Agent measures
};

// Power sum (dB) of ERB bands with centres in [lowHz, highHz).
double bandPowerDb(const ma::SpectrumAnalyzer::Bands& db, double lowHz, double highHz)
{
    double sum = 0.0;
    for (int b = 0; b < ma::SpectrumAnalyzer::numBandsFor(ma::BandScale::erb); ++b)
    {
        const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
        if (hz >= lowHz && hz < highHz)
            sum += std::pow(10.0, db[static_cast<size_t>(b)] / 10.0);
    }
    return sum > 1e-20 ? 10.0 * std::log10(sum) : -200.0;
}

// The role from the file name, the way stems are usually called; 0 when nothing fits.
int roleFromName(const String& fileName)
{
    const auto n = fileName.toLowerCase();
    const auto any = [&](std::initializer_list<const char*> words) {
        for (const auto* w : words)
            if (n.contains(w))
                return true;
        return false;
    };
    if (any({ "reverb", "verb", "delay", "echo", "return", "send" })) // before vocals: "VoxReverb" is a return
        return static_cast<int>(ma::Role::send);
    if (any({ "kick", "bd ", "bassdrum", "bass drum" }))
        return static_cast<int>(ma::Role::kick);
    if (any({ "vox", "vocal", "voc", "lead vox", "acapella", "adlib", "choir" }))
        return static_cast<int>(ma::Role::vocal);
    if (any({ "snare", "clap" })) // "KickSnare" is caught as a kick above
        return static_cast<int>(ma::Role::clap);
    if (any({ "hat", "hh", "perc", "shaker", "ride", "cymbal", "crash", "toms", "conga", "bongo", "top", "loop", "drum" }))
        return static_cast<int>(ma::Role::percussion);
    if (any({ "fx", "riser", "sweep", "impact", "noise", "uplifter", "downlifter", "atmos", "reverse" })) // before bass: "BassFX"
        return static_cast<int>(ma::Role::effects);
    if (any({ "bass", "sub", "808", "reese" }))
        return static_cast<int>(ma::Role::bass);
    if (any({ "pad", "chord", "keys", "piano", "organ", "string", "rhodes", "stab" }))
        return static_cast<int>(ma::Role::pad);
    if (any({ "lead", "synth", "pluck", "arp", "melody", "hook", "guitar", "sax" }))
        return static_cast<int>(ma::Role::lead);
    return 0;
}

bool readAudio(AudioFormatManager& formats, const File& file, double wantedRate, AudioBuffer<float>& out, double& rate)
{
    std::unique_ptr<AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr)
        return false;
    rate = reader->sampleRate;
    const int length = static_cast<int>(reader->lengthInSamples);
    AudioBuffer<float> in(static_cast<int>(std::max(1u, reader->numChannels)), length);
    reader->read(&in, 0, length, 0, true, true);
    AudioBuffer<float> stereo(2, length);
    stereo.copyFrom(0, 0, in, 0, 0, length);
    stereo.copyFrom(1, 0, in, std::min(1, in.getNumChannels() - 1), 0, length);
    if (wantedRate <= 0.0 || std::abs(wantedRate - rate) < 0.5)
    {
        out = std::move(stereo);
        return true;
    }
    // Another sample rate: resampled to the session's.
    const double ratio = rate / wantedRate;
    const int outLength = static_cast<int>(std::floor(length / ratio));
    out.setSize(2, outLength);
    for (int c = 0; c < 2; ++c)
    {
        LagrangeInterpolator interpolator;
        interpolator.process(ratio, stereo.getReadPointer(c), out.getWritePointer(c), outLength);
    }
    rate = wantedRate;
    return true;
}

bool writeWav(const File& file, const AudioBuffer<float>& audio, double rate)
{
    file.deleteFile();
    std::unique_ptr<FileOutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return false;
    WavAudioFormat wav;
    std::unique_ptr<AudioFormatWriter> writer(wav.createWriterFor(stream.get(), rate, 2, 24, {}, 0));
    if (writer == nullptr)
        return false;
    stream.release();
    return writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
}

// Short-term loudness a second at a time.
std::vector<double> loudnessPerSecond(const AudioBuffer<float>& audio, double rate)
{
    ma::LoudnessMeter meter;
    meter.prepare(rate, 2);
    std::vector<double> out;
    const int second = static_cast<int>(rate);
    for (int start = 0; start + second <= audio.getNumSamples(); start += second)
    {
        const float* ch[] = { audio.getReadPointer(0, start), audio.getReadPointer(1, start) };
        meter.process(ch, 2, second);
        out.push_back(meter.shortTermLufs());
    }
    return out;
}

// The loudest stretch of `seconds` (its start in samples): where the drop is.
int64 loudestStretch(const AudioBuffer<float>& audio, double rate, double seconds)
{
    const auto lufs = loudnessPerSecond(audio, rate);
    const int window = static_cast<int>(seconds);
    if (static_cast<int>(lufs.size()) <= window)
        return 0;
    double best = -1e9;
    int bestStart = 0;
    for (int s = 0; s + window <= static_cast<int>(lufs.size()); ++s)
    {
        double sum = 0.0;
        for (int k = 0; k < window; ++k)
            sum += std::isfinite(lufs[static_cast<size_t>(s + k)]) ? std::pow(10.0, lufs[static_cast<size_t>(s + k)] / 10.0) : 0.0;
        if (sum > best)
        {
            best = sum;
            bestStart = s;
        }
    }
    // The short-term window lags 3 s behind.
    return static_cast<int64>(std::max(0, bestStart - 2)) * static_cast<int64>(rate);
}

struct Measure
{
    double integrated = -INFINITY, truePeak = -INFINITY, dropLufs = -INFINITY, range = 0.0;
    ma::Curve tonal {};          // normalised, over the drop
    ma::Curve mid {}, side {};   // ERB dB over the drop
};

Measure measure(const AudioBuffer<float>& audio, double rate, int64 dropStart, int64 dropLength)
{
    Measure m;
    ma::LoudnessMeter whole, drop;
    whole.prepare(rate, 2);
    drop.prepare(rate, 2);
    ma::TruePeakMeter peak;
    peak.prepare(2);
    ma::SpectrumAnalyzer spectrum, mid, side;
    spectrum.prepare(rate, ma::kTonalScale);
    mid.prepare(rate, ma::BandScale::erb);
    side.prepare(rate, ma::BandScale::erb);
    std::vector<float> m1(kBlock), s1(kBlock);
    for (int64 start = 0; start < audio.getNumSamples(); start += kBlock)
    {
        const int n = static_cast<int>(std::min<int64>(kBlock, audio.getNumSamples() - start));
        const float* ch[] = { audio.getReadPointer(0, static_cast<int>(start)), audio.getReadPointer(1, static_cast<int>(start)) };
        whole.process(ch, 2, n);
        peak.process(ch, 2, n);
        if (start >= dropStart && start < dropStart + dropLength)
        {
            drop.process(ch, 2, n);
            spectrum.process(ch, 2, n);
            for (int i = 0; i < n; ++i)
            {
                m1[static_cast<size_t>(i)] = 0.5f * (ch[0][i] + ch[1][i]);
                s1[static_cast<size_t>(i)] = 0.5f * (ch[0][i] - ch[1][i]);
            }
            const float* mp[] = { m1.data() };
            const float* sp[] = { s1.data() };
            mid.process(mp, 1, n);
            side.process(sp, 1, n);
        }
    }
    m.integrated = whole.integratedLufs();
    m.truePeak = peak.truePeakDb();
    m.dropLufs = drop.integratedLufs();
    m.tonal = ma::normaliseCurve(spectrum.averageDb());
    m.mid = mid.averageDb();
    m.side = side.averageDb();
    // Loudness range, roughly: the spread of the short-term loudness over the louder part of the song.
    auto lufs = loudnessPerSecond(audio, rate);
    lufs.erase(std::remove_if(lufs.begin(), lufs.end(), [&](double v) { return !std::isfinite(v) || v < m.integrated - 20.0; }), lufs.end());
    if (lufs.size() > 4)
    {
        std::sort(lufs.begin(), lufs.end());
        m.range = lufs[lufs.size() * 95 / 100] - lufs[lufs.size() * 10 / 100];
    }
    return m;
}

// The four TBC ranges of `mix` against `reference` (one-reference target: its curve is the centre).
std::array<double, ma::kTonalZones> zonesAgainst(const ma::Curve& mix, const ma::Curve& reference)
{
    ma::TonalTarget target;
    target.name = "reference";
    target.references.push_back(reference);
    target.rebuild();
    const auto comparison = ma::compareToTarget(mix, target);
    const auto zones = ma::zoneOffsets(comparison, target);
    std::array<double, ma::kTonalZones> out {};
    for (int z = 0; z < ma::kTonalZones; ++z)
        out[static_cast<size_t>(z)] = zones[static_cast<size_t>(z)].offsetDb;
    return out;
}

// Side minus mid (dB) in a frequency range: how wide.
double widthDb(const Measure& m, double lowHz, double highHz)
{
    double mid = 0.0, side = 0.0;
    for (int b = 0; b < ma::SpectrumAnalyzer::numBandsFor(ma::BandScale::erb); ++b)
    {
        const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
        if (hz < lowHz || hz > highHz)
            continue;
        mid += std::pow(10.0, m.mid[static_cast<size_t>(b)] / 10.0);
        side += std::pow(10.0, m.side[static_cast<size_t>(b)] / 10.0);
    }
    return mid > 1e-20 && side > 1e-20 ? 10.0 * std::log10(side / mid) : -60.0;
}

String db(double v, int decimals = 1)
{
    return std::isfinite(v) ? String(v, decimals) : String("-inf");
}

String signedDb(double v)
{
    return (v >= 0.0 ? "+" : "") + String(v, 1);
}

int usage()
{
    std::fprintf(stderr, "usage: mixcheck --stems DIR [--reference MIX] [--target-refs A B ...] [--bpm N] [--out DIR] [--name NAME]\n"
                         "       [--kick-cable] [--shift-bass MS]\n");
    return 2;
}
} // namespace

int main(int argc, char** argv)
{
    ScopedJuceInitialiser_GUI gui;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    File stemsDir, referenceFile, outDir = File::getCurrentWorkingDirectory().getChildFile("mixcheck-out");
    Array<File> targetRefs;
    double bpm = 124.0;
    String name;
    bool masterCheck = false; // a stand-in mastering chain (gain, clip, top lift) and the master check plugin after it
    bool kickCable = false;  // the kick on each bass Agent's sidechain input, as a cable in FL
    double shiftBassMs = 0.0; // move the bass stems in time (> 0 later), to check the alignment
    for (int i = 1; i < argc; ++i)
    {
        const String arg(argv[i]);
        const auto next = [&]() { return i + 1 < argc ? String(argv[++i]) : String(); };
        if (arg == "--stems")
            stemsDir = File(next());
        else if (arg == "--reference")
            referenceFile = File(next());
        else if (arg == "--bpm")
            bpm = next().getDoubleValue();
        else if (arg == "--out")
            outDir = File(next());
        else if (arg == "--name")
            name = next();
        else if (arg == "--master-check")
            masterCheck = true;
        else if (arg == "--kick-cable")
            kickCable = true;
        else if (arg == "--shift-bass")
            shiftBassMs = next().getDoubleValue();
        else if (arg == "--target-refs")
            while (i + 1 < argc && !String(argv[i + 1]).startsWith("--"))
                targetRefs.add(File(String(argv[++i])));
        else
            return usage();
    }
    if (!stemsDir.isDirectory())
        return usage();
    if (name.isEmpty())
        name = stemsDir.getFileName();
    outDir.createDirectory();

    AudioFormatManager formats;
    formats.registerBasicFormats();

    // Stems.
    std::vector<Track> tracks;
    double rate = 0.0;
    auto files = stemsDir.findChildFiles(File::findFiles, false, "*.wav;*.WAV;*.aif;*.aiff;*.flac;*.mp3");
    files.sort();
    for (const auto& file : files)
    {
        if (file == referenceFile)
            continue;
        Track track;
        double fileRate = 0.0;
        if (!readAudio(formats, file, rate, track.audio, fileRate))
        {
            std::fprintf(stderr, "skipped (unreadable): %s\n", file.getFileName().toRawUTF8());
            continue;
        }
        if (track.audio.getMagnitude(0, track.audio.getNumSamples()) < 1e-5f)
        {
            std::fprintf(stderr, "skipped (silent): %s\n", file.getFileName().toRawUTF8());
            continue;
        }
        if (rate <= 0.0)
            rate = fileRate;
        track.name = file.getFileNameWithoutExtension().substring(0, 30);
        track.role = roleFromName(file.getFileNameWithoutExtension());
        tracks.push_back(std::move(track));
    }
    if (tracks.empty() || static_cast<int>(tracks.size()) > 60)
    {
        std::fprintf(stderr, "error: %zu stems (need 1 to 60)\n", tracks.size());
        return 1;
    }
    int length = 0;
    for (const auto& t : tracks)
        length = std::max(length, t.audio.getNumSamples());
    AudioBuffer<float> raw(2, length);
    raw.clear();
    for (auto& t : tracks)
    {
        if (t.audio.getNumSamples() < length)
        {
            AudioBuffer<float> padded(2, length);
            padded.clear();
            for (int c = 0; c < 2; ++c)
                padded.copyFrom(c, 0, t.audio, c, 0, t.audio.getNumSamples());
            t.audio = std::move(padded);
        }
        if (shiftBassMs != 0.0 && t.role == static_cast<int>(ma::Role::bass))
        {
            const int shift = static_cast<int>(std::lround(shiftBassMs * 0.001 * rate));
            AudioBuffer<float> moved(2, length);
            moved.clear();
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < length; ++i)
                    if (i - shift >= 0 && i - shift < length)
                        moved.setSample(c, i, t.audio.getSample(c, i - shift));
            t.audio = std::move(moved);
            std::printf("shifted %s by %.2f ms\n", t.name.toRawUTF8(), shiftBassMs);
        }
        for (int c = 0; c < 2; ++c)
            raw.addFrom(c, 0, t.audio, c, 0, length);
    }
    int kickTrack = -1;
    for (size_t t = 0; t < tracks.size(); ++t)
        if (tracks[t].role == static_cast<int>(ma::Role::kick))
        {
            kickTrack = static_cast<int>(t);
            break;
        }
    const auto dropStart = loudestStretch(raw, rate, kDropSeconds);
    const auto dropLength = static_cast<int64>(std::min<double>(kDropSeconds * rate, length - dropStart));
    std::printf("%zu stems at %.0f Hz, %.0f s; drop from %.0f s\n", tracks.size(), rate, length / rate, dropStart / rate);
    for (auto& t : tracks)
    {
        ma::SpectrumAnalyzer analyser;
        analyser.prepare(rate, ma::BandScale::erb);
        for (int64 start = dropStart; start < dropStart + dropLength; start += kBlock)
        {
            const int n = static_cast<int>(std::min<int64>(kBlock, dropStart + dropLength - start));
            const float* ch[] = { t.audio.getReadPointer(0, static_cast<int>(start)), t.audio.getReadPointer(1, static_cast<int>(start)) };
            analyser.process(ch, 2, n);
        }
        t.dropDb = analyser.averageDb();
    }

    // Genre target from other references (never the reference mix itself).
    if (!targetRefs.isEmpty())
    {
        ma::TonalTarget target;
        target.name = "mixcheck";
        for (const auto& file : targetRefs)
        {
            AudioBuffer<float> audio;
            double fileRate = 0.0;
            if (!readAudio(formats, file, 0.0, audio, fileRate))
                continue;
            const auto start = loudestStretch(audio, fileRate, 30.0);
            const auto m = measure(audio, fileRate, start, static_cast<int64>(30.0 * fileRate));
            target.references.push_back(m.tonal);
        }
        target.rebuild();
        const auto folder = File::getSpecialLocation(File::userApplicationDataDirectory).getChildFile("MixAssistant").getChildFile("targets");
        folder.createDirectory();
        folder.getChildFile("mixcheck.matarget").replaceWithText(ma::serialiseTarget(target));
        std::printf("tonal target from %zu references\n", target.references.size());
    }

    // The session: one Agent per stem, the Brain on the sum.
    Transport transport;
    transport.sampleRate = rate;
    transport.bpm = bpm;
    std::vector<std::unique_ptr<AgentProcessor>> agents;
    std::vector<bool> cabled;
    for (const auto& t : tracks)
    {
        auto agent = std::make_unique<AgentProcessor>();
        auto layout = agent->getBusesLayout();
        const bool cable = kickCable && kickTrack >= 0 && t.role == static_cast<int>(ma::Role::bass);
        cabled.push_back(cable);
        if (layout.inputBuses.size() > 1)
            layout.inputBuses.getReference(1) = cable ? AudioChannelSet::stereo()
                                                      : AudioChannelSet::disabled(); // no cable: what FL users mostly have
        if (cable)
            std::printf("kick cable into %s\n", t.name.toRawUTF8());
        agent->setBusesLayout(layout);
        agent->setPlayHead(&transport);
        agent->setRateAndBufferSizeDetails(rate, kBlock);
        agent->prepareToPlay(rate, kBlock);
        AudioProcessor::TrackProperties properties;
        properties.name = t.name;
        agent->updateTrackProperties(properties);
        if (t.role > 0)
            if (auto* p = agent->parameters.getParameter(AgentProcessor::roleId))
                p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(t.role)));
        agents.push_back(std::move(agent));
    }
    auto brain = std::make_unique<BrainProcessor>();
    brain->setPlayHead(&transport);
    brain->setPlayConfigDetails(2, 2, rate, kBlock);
    brain->prepareToPlay(rate, kBlock);
    brain->setEditorOpen(true); // the Agents send their live spectra only while the Brain's window is open
    if (!targetRefs.isEmpty())
        brain->selectTarget("mixcheck");

    // The test clock: the Agents tick at their 10 Hz and the Brain at its 5 Hz of rendered time.
    for (auto& agent : agents)
        agent->useTestTicks();
    brain->useTestTicks();
    std::unique_ptr<MasterProcessor> master;
    if (masterCheck)
    {
        master = std::make_unique<MasterProcessor>();
        master->prepareToPlay(rate, kBlock);
        master->useTestTicks();
    }
    AudioBuffer<float> mastered(2, kBlock);
    float shelfState[2] = { 0.0f, 0.0f };
    double clockMs = 1000.0;
    int64 ticks = 0;
    ma::clock::testMs.store(static_cast<std::int64_t>(clockMs));
    const auto advance = [&](double ms) {
        const double end = clockMs + ms;
        while (static_cast<int64>(end / 100.0) > ticks)
        {
            ++ticks;
            clockMs = static_cast<double>(ticks) * 100.0;
            ma::clock::testMs.store(static_cast<std::int64_t>(clockMs));
            for (auto& agent : agents)
                agent->testTick();
            if (ticks % 2 == 0)
                brain->testTick();
            if (master != nullptr)
                master->testTick();
            if (ticks % 10 == 0)
                MessageManager::getInstance()->runDispatchLoopUntil(1); // async updates, if any
        }
        clockMs = end;
        ma::clock::testMs.store(static_cast<std::int64_t>(clockMs));
    };
    advance(300.0);

    AudioBuffer<float> block(4, kBlock), sum(2, kBlock);
    MidiBuffer midi;
    // Plays from `from` for `seconds`; inside the drop when `loop`, carrying on where the last loop
    // stopped (short calls each restarting at the drop's first seconds left parts that come in later
    // out of every measurement after "Zastosuj"). `render` collects the sum.
    int64 loopPhase = 0;
    // Each part after its whole chain over the drop, measured while rendering (the Agents' published
    // output spectrum is only their latest second, so a part that plays now and then read as gone).
    std::vector<ma::SpectrumAnalyzer> outAnalysers(tracks.size());
    for (auto& a : outAnalysers)
        a.prepare(rate, ma::BandScale::erb);
    bool measureOut = false;
    // Each part's output over the drop, kept to see which of them make the peaks of the sum.
    std::vector<AudioBuffer<float>> dropOut(tracks.size());
    for (auto& b : dropOut)
    {
        b.setSize(2, static_cast<int>(dropLength));
        b.clear();
    }
    const auto play = [&](int64 from, double seconds, bool loop, AudioBuffer<float>* render) {
        const auto total = static_cast<int64>(seconds * rate);
        int64 done = 0;
        int blocks = 0;
        while (done < total)
        {
            const int n = static_cast<int>(std::min<int64>(kBlock, total - done));
            int64 position = from + done;
            if (loop)
            {
                position = dropStart + loopPhase;
                loopPhase = (loopPhase + n) % dropLength;
            }
            transport.position = position;
            sum.clear();
            for (size_t t = 0; t < tracks.size(); ++t)
            {
                block.clear();
                for (int i = 0; i < n; ++i)
                {
                    int64 p = position + i;
                    if (loop && p >= dropStart + dropLength)
                        p -= dropLength;
                    if (p < length)
                        for (int c = 0; c < 2; ++c)
                        {
                            block.setSample(c, i, tracks[t].audio.getSample(c, static_cast<int>(p)));
                            if (cabled[t])
                                block.setSample(2 + c, i, tracks[static_cast<size_t>(kickTrack)].audio.getSample(c, static_cast<int>(p)));
                        }
                }
                AudioBuffer<float> view(block.getArrayOfWritePointers(), cabled[t] ? 4 : 2, n);
                agents[t]->processBlock(view, midi);
                if (measureOut && !loop && position >= dropStart && position + n <= dropStart + dropLength)
                {
                    const float* ch[] = { block.getReadPointer(0), block.getReadPointer(1) };
                    outAnalysers[t].process(ch, 2, n);
                    for (int c = 0; c < 2; ++c)
                        dropOut[t].copyFrom(c, static_cast<int>(position - dropStart), block, c, 0, n);
                }
                for (int c = 0; c < 2; ++c)
                    sum.addFrom(c, 0, block, c, 0, n);
            }
            AudioBuffer<float> sumView(sum.getArrayOfWritePointers(), 2, n);
            brain->processBlock(sumView, midi);
            if (master != nullptr)
            {
                // The stand-in for Ozone: +14 dB, the top lifted 3 dB from about 6 kHz, hard ceiling at -1 dBFS.
                const float gain = 5.0f, ceiling = 0.891f, k = static_cast<float>(1.0 - std::exp(-2.0 * 3.14159265 * 6000.0 / rate));
                for (int c = 0; c < 2; ++c)
                {
                    const float* in = sum.getReadPointer(c);
                    float* out = mastered.getWritePointer(c);
                    for (int i = 0; i < n; ++i)
                    {
                        shelfState[c] += k * (in[i] - shelfState[c]);
                        const float lifted = in[i] + 0.41f * (in[i] - shelfState[c]);
                        out[i] = jlimit(-ceiling, ceiling, gain * lifted);
                    }
                }
                AudioBuffer<float> masteredView(mastered.getArrayOfWritePointers(), 2, n);
                master->processBlock(masteredView, midi);
            }
            if (render != nullptr && position < render->getNumSamples())
                for (int c = 0; c < 2; ++c)
                    render->copyFrom(c, static_cast<int>(position), sum, c, 0, std::min<int>(n, render->getNumSamples() - static_cast<int>(position)));
            done += n;
            ++blocks;
            advance(1000.0 * n / rate);
        }
        advance(50.0);
    };

    // 1. Learn on the drop.
    std::printf("learning...\n");
    for (auto& agent : agents)
        agent->startLearning();
    play(dropStart, 2.0 * kDropSeconds, true, nullptr);
    for (auto& agent : agents)
        if (agent->isLearning())
            agent->stopLearning();
    advance(500.0);
    for (size_t t = 0; t < tracks.size(); ++t)
    {
        const auto r = agents[t]->lastResult();
        std::printf("  learn %-30s measured %6.1f, active %3.0f%%, trim %+5.1f%s\n", tracks[t].name.toRawUTF8(), r.measuredDb,
                    r.activeShare * 100.0, r.trimDb, r.barelyPlayed ? " (barely played: trim left)" : r.applied ? "" : " (nothing heard)");
    }

    // 2. The Brain measures the drop, then "Analizuj" (whole chain).
    brain->requestReset();
    play(dropStart, 1.6 * kDropSeconds, true, nullptr);
    const auto before = brain->snapshot();
    brain->analyse(1);
    const auto proposal = brain->proposal();
    const auto notes = brain->toolNotes();
    const auto verdicts = brain->verdictsNow();
    std::printf("analysed: %zu proposal items\n", proposal.items.size());

    // 3. "Zastosuj", then the refining rounds by themselves.
    brain->applyByUser();
    String lastStatus;
    for (int s = 0; s < 240 && (s < 4 || brain->isRefining()); s += 4)
    {
        play(dropStart, 4.0, true, nullptr);
        // Each refining step and where the peak stands.
        if (brain->refineStatus() != lastStatus)
        {
            lastStatus = brain->refineStatus();
            std::printf("  %s (peak %.1f dBTP)\n", lastStatus.toRawUTF8(), brain->truePeakDb());
        }
    }
    std::printf("refined: %s\n", brain->refineStatus().toRawUTF8());
    for (int s = 0; s < 5; ++s)
    {
        play(dropStart, 4.0, true, nullptr);
        std::printf("  after refining, %d s: peak %.1f dBTP\n", (s + 1) * 4, brain->truePeakDb());
    }
    for (size_t t = 0; t < tracks.size(); ++t)
        if (cabled[t])
            std::printf("  %s against the kick: %s lag %.2f ms, correlation %.2f now, %.2f at best, delay %.2f ms\n",
                        tracks[t].name.toRawUTF8(), agents[t]->alignmentValid() ? "measured" : "not measured",
                        agents[t]->alignmentLagMs(), agents[t]->alignmentZero(), agents[t]->alignmentBest(),
                        static_cast<double>(*agents[t]->parameters.getRawParameterValue(AgentProcessor::alignDelayId)));
    const auto after = brain->snapshot();

    // 4. The whole song as the Agents ended up.
    std::printf("rendering...\n");
    AudioBuffer<float> ours(2, length);
    ours.clear();
    measureOut = true;
    play(0, length / rate, false, &ours);
    measureOut = false;
    writeWav(outDir.getChildFile("ours.wav"), ours, rate);
    writeWav(outDir.getChildFile("raw.wav"), raw, rate);

    // 5. Measure and compare.
    const auto mOurs = measure(ours, rate, dropStart, dropLength);
    const auto mRaw = measure(raw, rate, dropStart, dropLength);
    Measure mRef;
    bool haveRef = false;
    if (referenceFile.existsAsFile())
    {
        AudioBuffer<float> reference;
        double refRate = 0.0;
        if (readAudio(formats, referenceFile, rate, reference, refRate))
        {
            haveRef = true;
            const auto refDrop = loudestStretch(reference, rate, kDropSeconds);
            mRef = measure(reference, rate, refDrop, static_cast<int64>(std::min<double>(kDropSeconds * rate, reference.getNumSamples() - refDrop)));
            // The reference brought down to our loudness, for listening A/B.
            reference.applyGain(static_cast<float>(std::pow(10.0, (mOurs.integrated - mRef.integrated) / 20.0)));
            writeWav(outDir.getChildFile("reference-at-our-level.wav"), reference, rate);
        }
    }

    const auto roles = ma::ui::roleNames();
    const auto toolNames = ma::ui::toolNames();
    const auto decisionNames = ma::ui::decisionNames();
    String md;
    md << "# mixcheck: " << name << "\n\n";
    md << String(static_cast<int>(tracks.size())) << " stems, " << String(length / rate, 0) << " s, drop from " << String(dropStart / rate, 0)
       << " s, " << String(bpm, 0) << " BPM. Agents and Brain as in FL: learn on the drop, Analizuj (whole chain), Zastosuj, refine.\n\n";
    md << "## Levels\n\n| | integrated LUFS | drop LUFS | true peak dBTP | PLR (drop) | loudness range |\n|---|---|---|---|---|---|\n";
    const auto levelRow = [&](const char* label, const Measure& m) {
        md << "| " << label << " | " << db(m.integrated) << " | " << db(m.dropLufs) << " | " << db(m.truePeak) << " | " << db(m.truePeak - m.dropLufs)
           << " | " << db(m.range) << " |\n";
    };
    levelRow("raw sum", mRaw);
    levelRow("ours", mOurs);
    if (haveRef)
        levelRow("reference", mRef);
    if (haveRef)
    {
        md << "\n## Tonal balance against the reference (drop, dB, + = more than the reference)\n\n";
        md << "| | Low 40-250 | Low-Mid 250-2k | High-Mid 2k-9k | High 9k-16k |\n|---|---|---|---|---|\n";
        for (const auto& [label, m] : { std::pair<const char*, const Measure*> { "raw sum", &mRaw }, { "ours", &mOurs } })
        {
            const auto z = zonesAgainst(m->tonal, mRef.tonal);
            md << "| " << label << " | " << signedDb(z[0]) << " | " << signedDb(z[1]) << " | " << signedDb(z[2]) << " | " << signedDb(z[3]) << " |\n";
        }
        md << "\n## Width (side minus mid, dB, drop)\n\n| | below 120 Hz | 120 Hz-1 kHz | 1-6 kHz | above 6 kHz |\n|---|---|---|---|---|\n";
        for (const auto& [label, m] : { std::pair<const char*, const Measure*> { "raw sum", &mRaw }, { "ours", &mOurs }, { "reference", &mRef } })
            md << "| " << label << " | " << db(widthDb(*m, 20, 120)) << " | " << db(widthDb(*m, 120, 1000)) << " | " << db(widthDb(*m, 1000, 6000))
               << " | " << db(widthDb(*m, 6000, 20000)) << " |\n";
    }

    // What the Agents did to each part over the drop: level as it came in, after its whole chain, and
    // the change in TBC's four ranges (where a channel's change tilts the mix).
    md << "\n## Channels after (drop)\n\n| channel | role | trim dB | in dBFS | out dBFS | change | Low | Low-Mid | High-Mid | High |\n"
          "|---|---|---|---|---|---|---|---|---|---|\n";
    for (const auto& channel : after.channels)
    {
        const auto track = std::find_if(tracks.begin(), tracks.end(), [&](const Track& t) { return t.name == channel.name; });
        ma::SpectrumAnalyzer::Bands out {};
        out.fill(-200.0);
        if (track != tracks.end())
            out = outAnalysers[static_cast<size_t>(track - tracks.begin())].averageDb();
        const double outDb = bandPowerDb(out, 0.0, 1e9);
        md << "| " << channel.name << " | " << roles[jlimit(0, roles.size() - 1, static_cast<int>(channel.data.role))] << " | "
           << signedDb(channel.data.trimDb) << " | ";
        if (track == tracks.end() || outDb < -150.0 || bandPowerDb(track->dropDb, 0.0, 1e9) < -150.0)
        {
            md << (track != tracks.end() ? db(bandPowerDb(track->dropDb, 0.0, 1e9)) : String("?")) << " | " << db(outDb) << " | | | | | |\n";
            continue;
        }
        md << db(bandPowerDb(track->dropDb, 0.0, 1e9)) << " | " << db(outDb) << " | " << signedDb(outDb - bandPowerDb(track->dropDb, 0.0, 1e9));
        for (int z = 0; z < ma::kTonalZones; ++z)
        {
            const double low = ma::tonalZoneEdgeHz(z), high = ma::tonalZoneEdgeHz(z + 1);
            const double in = bandPowerDb(track->dropDb, low, high), o = bandPowerDb(out, low, high);
            md << " | " << (in > -120.0 && o > -120.0 ? signedDb(o - in) : String("-"));
        }
        md << " |\n";
    }

    // Who makes the peaks of the sum: the 32 highest peaks of the drop (at least 100 ms apart), each part's
    // share of the sample there, its own level and crest, and what leaving it out would do to the sum's
    // crest (peak minus RMS): a part that only brings peaks lowers it a lot when it goes.
    {
        const int n = static_cast<int>(dropLength);
        AudioBuffer<float> total(2, n);
        total.clear();
        for (const auto& b : dropOut)
            for (int c = 0; c < 2; ++c)
                total.addFrom(c, 0, b, c, 0, n);
        const auto crestOf = [n](const AudioBuffer<float>& b, double& peakDb, double& rmsDb) {
            double peak = 0.0, square = 0.0;
            for (int c = 0; c < 2; ++c)
            {
                const float* x = b.getReadPointer(c);
                for (int i = 0; i < n; ++i)
                {
                    peak = std::max(peak, static_cast<double>(std::abs(x[i])));
                    square += static_cast<double>(x[i]) * x[i];
                }
            }
            peakDb = 20.0 * std::log10(std::max(peak, 1e-9));
            rmsDb = 10.0 * std::log10(std::max(square / (2.0 * n), 1e-18));
        };
        double sumPeak = 0.0, sumRms = 0.0;
        crestOf(total, sumPeak, sumRms);
        struct Peak
        {
            int at = 0, channel = 0;
            float value = 0.0f;
        };
        std::vector<Peak> peaks;
        std::vector<char> taken(static_cast<size_t>(n), 0);
        const int apart = static_cast<int>(0.1 * rate);
        for (int k = 0; k < 32; ++k)
        {
            Peak best;
            for (int c = 0; c < 2; ++c)
            {
                const float* x = total.getReadPointer(c);
                for (int i = 0; i < n; ++i)
                    if (!taken[static_cast<size_t>(i)] && std::abs(x[i]) > std::abs(best.value))
                        best = { i, c, x[i] };
            }
            if (best.value == 0.0f)
                break;
            peaks.push_back(best);
            for (int i = std::max(0, best.at - apart); i < std::min(n, best.at + apart); ++i)
                taken[static_cast<size_t>(i)] = 1;
        }
        md << "\n## Who makes the peaks (drop)\n\nSum: peak " << db(sumPeak) << " dBFS, RMS " << db(sumRms) << ", crest " << db(sumPeak - sumRms)
           << " dB; the lowest of the 32 peaks is " << db(peaks.empty() ? 0.0 : 20.0 * std::log10(std::abs(peaks.back().value)) - sumPeak)
           << " dB under the highest.\n\n| channel | share of the peaks | peak dBFS | RMS dBFS | crest | sum's crest without it |\n|---|---|---|---|---|---|\n";
        struct Row
        {
            String name;
            double share = 0.0, peakDb = 0.0, rmsDb = 0.0, without = 0.0;
        };
        std::vector<Row> rows;
        AudioBuffer<float> rest(2, n);
        for (size_t t = 0; t < tracks.size(); ++t)
        {
            Row row;
            row.name = tracks[t].name;
            for (const auto& peak : peaks)
                row.share += dropOut[t].getSample(peak.channel, peak.at) / peak.value;
            row.share /= std::max<size_t>(1, peaks.size());
            crestOf(dropOut[t], row.peakDb, row.rmsDb);
            for (int c = 0; c < 2; ++c)
            {
                rest.copyFrom(c, 0, total, c, 0, n);
                rest.addFrom(c, 0, dropOut[t], c, 0, n, -1.0f);
            }
            double restPeak = 0.0, restRms = 0.0;
            crestOf(rest, restPeak, restRms);
            row.without = (restPeak - restRms) - (sumPeak - sumRms);
            rows.push_back(row);
        }
        // What if: nothing but the kick may peak higher than the kick's peak plus this (hard-clipped here, the
        // crudest tool): how far the sum's peak comes down and what it costs in level.
        double kickPeakDb = -200.0;
        for (size_t t = 0; t < tracks.size(); ++t)
            if (static_cast<int>(t) == kickTrack)
                kickPeakDb = rows[t].peakDb;
        String whatIf;
        if (kickPeakDb > -100.0)
            for (const double over : { 0.0, -3.0, -6.0 })
            {
                const float ceiling = static_cast<float>(std::pow(10.0, (kickPeakDb + over) / 20.0));
                rest.clear();
                for (size_t t = 0; t < tracks.size(); ++t)
                    for (int c = 0; c < 2; ++c)
                    {
                        const float* x = dropOut[t].getReadPointer(c);
                        float* y = rest.getWritePointer(c);
                        const bool kick = static_cast<int>(t) == kickTrack;
                        for (int i = 0; i < n; ++i)
                            y[i] += kick ? x[i] : jlimit(-ceiling, ceiling, x[i]);
                    }
                double p = 0.0, r = 0.0;
                crestOf(rest, p, r);
                whatIf << "- other parts held to the kick's peak " << signedDb(over) << " dB: sum peak " << signedDb(p - sumPeak) << " dB, RMS "
                       << signedDb(r - sumRms) << " dB, crest " << db(p - r) << " dB\n";
            }
        // What if the kick were louder (the anchor of a club mix): the sum's crest with the kick up 3 and 6 dB.
        if (kickPeakDb > -100.0)
            for (const double up : { 3.0, 6.0 })
            {
                const float g = static_cast<float>(std::pow(10.0, up / 20.0)) - 1.0f;
                for (int c = 0; c < 2; ++c)
                {
                    rest.copyFrom(c, 0, total, c, 0, n);
                    rest.addFrom(c, 0, dropOut[static_cast<size_t>(kickTrack)], c, 0, n, g);
                }
                double p = 0.0, r = 0.0;
                crestOf(rest, p, r);
                whatIf << "- kick " << signedDb(up) << " dB: sum peak " << signedDb(p - sumPeak) << " dB, RMS " << signedDb(r - sumRms) << " dB, crest "
                       << db(p - r) << " dB\n";
            }
        // More what-ifs: everything but the drums pumping under the kick (a whole-signal duck following the
        // kick's envelope), and each part's own peaks clipped a little.
        if (kickPeakDb > -100.0)
        {
            std::vector<float> envelope(static_cast<size_t>(n), 0.0f);
            const float* kl = dropOut[static_cast<size_t>(kickTrack)].getReadPointer(0);
            const float* kr = dropOut[static_cast<size_t>(kickTrack)].getReadPointer(1);
            const float release = static_cast<float>(std::exp(-1.0 / (0.12 * rate)));
            float e = 0.0f, top = 1e-9f;
            for (int i = 0; i < n; ++i)
            {
                e = std::max(std::max(std::abs(kl[i]), std::abs(kr[i])), e * release);
                envelope[static_cast<size_t>(i)] = e;
                top = std::max(top, e);
            }
            for (const double depth : { 3.0, 6.0 })
            {
                rest.clear();
                for (size_t t = 0; t < tracks.size(); ++t)
                {
                    const bool drums = tracks[t].role == static_cast<int>(ma::Role::kick) || ma::isPercussion(static_cast<ma::Role>(tracks[t].role));
                    for (int c = 0; c < 2; ++c)
                    {
                        const float* x = dropOut[t].getReadPointer(c);
                        float* y = rest.getWritePointer(c);
                        for (int i = 0; i < n; ++i)
                            y[i] += drums ? x[i] : x[i] * static_cast<float>(std::pow(10.0, -depth * std::min(1.0f, envelope[static_cast<size_t>(i)] / top) / 20.0));
                    }
                }
                double p = 0.0, r = 0.0;
                crestOf(rest, p, r);
                whatIf << "- everything but the drums ducks " << db(depth) << " dB under the kick: sum peak " << signedDb(p - sumPeak) << " dB, RMS "
                       << signedDb(r - sumRms) << " dB, crest " << db(p - r) << " dB\n";
            }
        }
        for (const double clip : { 3.0, 6.0 })
            for (const bool bassOnly : { true, false })
            {
                rest.clear();
                for (size_t t = 0; t < tracks.size(); ++t)
                {
                    const bool held = bassOnly ? tracks[t].role == static_cast<int>(ma::Role::bass) : static_cast<int>(t) != kickTrack;
                    const float ceiling = static_cast<float>(std::pow(10.0, (rows[t].peakDb - (held ? clip : 0.0)) / 20.0));
                    for (int c = 0; c < 2; ++c)
                    {
                        const float* x = dropOut[t].getReadPointer(c);
                        float* y = rest.getWritePointer(c);
                        for (int i = 0; i < n; ++i)
                            y[i] += jlimit(-ceiling, ceiling, x[i]);
                    }
                }
                double p = 0.0, r = 0.0;
                crestOf(rest, p, r);
                whatIf << "- " << (bassOnly ? "bass parts" : "every part but the kick") << " clipped " << db(clip) << " dB under their own peak: sum peak "
                       << signedDb(p - sumPeak) << " dB, RMS " << signedDb(r - sumRms) << " dB, crest " << db(p - r) << " dB\n";
            }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.share > b.share; });
        for (const auto& row : rows)
            if (row.rmsDb > -90.0)
                md << "| " << row.name << " | " << String(roundToInt(row.share * 100.0)) << "% | " << db(row.peakDb) << " | " << db(row.rmsDb) << " | "
                   << db(row.peakDb - row.rmsDb) << " | " << signedDb(row.without) << " |\n";
        if (whatIf.isNotEmpty())
            md << "\nKick peak " << db(kickPeakDb) << " dBFS. What if:\n" << whatIf;
    }

    md << "\n## Collisions\n\nBefore: " << String(static_cast<int>(before.masking.issues.size())) << ", after: "
       << String(static_cast<int>(after.masking.issues.size())) << "\n\n";
    for (const auto& issue : after.masking.issues)
        md << "- " << brain->describeIssue(issue) << "\n";

    md << "\n## Proposal (first analysis)\n\n";
    for (const auto& item : proposal.items)
        md << "- " << (item.advisory ? "(advice) " : item.selected ? "[x] " : "[ ] ") << item.text << "\n";

    md << "\n## Report after\n\n";
    for (const auto& item : after.report)
    {
        const char* status = item.status == MixSnapshot::Status::good      ? "ok"
                             : item.status == MixSnapshot::Status::warning ? "WARN"
                             : item.status == MixSnapshot::Status::problem ? "PROBLEM"
                                                                           : "?";
        md << "- **" << status << "** " << item.title << ": " << item.detail << "\n";
    }

    md << "\n## Decisions per tool (first analysis)\n\n";
    for (const auto& channel : before.channels)
    {
        const auto found = notes.find(channel.owner);
        if (found == notes.end())
            continue;
        md << "### " << channel.name << "\n\n";
        for (int t = 0; t < ma::bus::kToolCount; ++t)
        {
            const auto& note = found->second[static_cast<size_t>(t)];
            md << "- " << toolNames[t] << ": " << decisionNames[jlimit(0, decisionNames.size() - 1, static_cast<int>(note.decision))] << ", "
               << note.text << "\n";
        }
        md << "\n";
    }
    outDir.getChildFile("report.md").replaceWithText(md);

    // The same numbers for comparing runs.
    const auto levels = [&](const Measure& m) {
        auto* o = new DynamicObject();
        o->setProperty("integratedLufs", m.integrated);
        o->setProperty("dropLufs", m.dropLufs);
        o->setProperty("truePeakDb", m.truePeak);
        o->setProperty("plrDb", m.truePeak - m.dropLufs);
        o->setProperty("loudnessRangeDb", m.range);
        Array<var> width;
        for (const auto& [low, high] : { std::pair<double, double> { 20, 120 }, { 120, 1000 }, { 1000, 6000 }, { 6000, 20000 } })
            width.add(widthDb(m, low, high));
        o->setProperty("widthDb", width);
        if (haveRef)
        {
            Array<var> zones;
            for (const auto z : zonesAgainst(m.tonal, mRef.tonal))
                zones.add(z);
            o->setProperty("zonesVsReferenceDb", zones);
        }
        return var(o);
    };
    auto* json = new DynamicObject();
    json->setProperty("name", name);
    json->setProperty("stems", static_cast<int>(tracks.size()));
    json->setProperty("seconds", length / rate);
    json->setProperty("dropStartSeconds", static_cast<double>(dropStart) / rate);
    json->setProperty("raw", levels(mRaw));
    json->setProperty("ours", levels(mOurs));
    if (haveRef)
        json->setProperty("reference", levels(mRef));
    json->setProperty("proposalItems", static_cast<int>(proposal.items.size()));
    json->setProperty("collisionsBefore", static_cast<int>(before.masking.issues.size()));
    json->setProperty("collisionsAfter", static_cast<int>(after.masking.issues.size()));
    Array<var> channels;
    for (const auto& channel : after.channels)
    {
        auto* o = new DynamicObject();
        o->setProperty("name", channel.name);
        o->setProperty("role", static_cast<int>(channel.data.role));
        o->setProperty("trimDb", channel.data.trimDb);
        channels.add(var(o));
    }
    json->setProperty("channels", channels);
    outDir.getChildFile("report.json").replaceWithText(JSON::toString(var(json)));
    std::printf("%s\n", md.toRawUTF8());

    brain.reset();
    agents.clear();
    return 0;
}
