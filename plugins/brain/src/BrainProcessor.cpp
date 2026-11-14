#include "BrainProcessor.h"
#include "ma/Audibility.h"

#include "BrainEditor.h"
#include "MaUi.h"
#include "ma/Instrument.h"
#include "ma/KickTuning.h"

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
constexpr double kTranslationShiftDb = 0.5;
constexpr double kTranslationTonalSlackDb = 1.0; // phase 27: summed over bands, how much further out of the corridor a step may take the mix     // phase 27: translation steps moving the balance more get a second pass
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
// Phase 16: sections by the song's position. The Agents' spectra are about a second long and the
// short-term loudness 3 s, so what an update hears lies a little before the host's position.
constexpr double kSpectrumLagSeconds = 0.5;
constexpr double kLoudnessLagSeconds = 1.5;    // where the drop's loudness shows after it came in
constexpr double kMaxSongSeconds = 600.0;
constexpr int kDropRunSeconds = 4;             // a drop: this many seconds in a row at its level
constexpr double kBuildSeconds = 8.0;
constexpr double kGhostDropRangeDb = 4.0; // phase 21: ghosts play while the mix is this close to its drop level
constexpr double kSectionGuardSeconds = 1.0; // phase 19: left out on either side of a section boundary
constexpr double kMinimumBuildSeconds = 4.0;
constexpr double kSectionRangeDb = 30.0;       // playing at all: within this of the drop

juce::String quoted(const juce::String& name)
{
    return pl("„") + name + pl("”");
}

// Phase 24: TBC's names for its four ranges.
juce::String zoneName(int zone)
{
    static const char* names[ma::kTonalZones] { "Low", "Low-Mid", "High-Mid", "High" };
    return names[juce::jlimit(0, ma::kTonalZones - 1, zone)];
}

juce::String signedOneDecimal(double db)
{
    return (db >= 0.05 ? "+" : "") + juce::String(db, 1);
}

juce::String centreText(const ma::SolverChange& change, const juce::String& targetName)
{
    return zoneName(change.zone) + pl(" leży ") + juce::String(std::abs(change.zoneOffsetDb), 1)
           + (change.zoneOffsetDb > 0.0 ? pl(" dB nad środkiem") : pl(" dB pod środkiem")) + pl(" korytarza celu ") + targetName
           + pl(" (w korytarzu, ale przy krawędzi); po zmianie ") + signedOneDecimal(change.zoneAfterDb) + pl(" dB od środka");
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

// Phase 34: the settings for the user's own reverb and delay plugins, at the song's tempo, named by
// the knobs most plugins have (ValhallaPlate / Room, Fruity Reeverb 2, Fruity Delay 3 ...).
juce::String reverbAdviceText(double bpm)
{
    const auto t = ma::depthTimes(bpm);
    if (!t.valid)
        return {};
    return pl("Pogłos (na szynie wysyłki, ") + juce::String(bpm, 1) + pl(" BPM): Mix/Wet 100%, Dry 0%; Pre-delay ")
           + juce::String(juce::roundToInt(t.preDelayMs)) + pl(" ms (1/64: głos wchodzi pierwszy); Decay ok. ")
           + juce::String(t.decaySeconds, 1) + pl(" s (pół taktu, wygasa przed następną frazą); EQ pogłosu: dół na minus "
                                                  "(Low gain -4 do -6 dB poniżej ok. 500 Hz), góra lekko w dół (High gain ok. -3 dB od ok. 4 kHz)");
}

juce::String delayAdviceText(double bpm)
{
    const auto t = ma::depthTimes(bpm);
    if (!t.valid)
        return {};
    return pl("Delay (na szynie wysyłki): Tempo sync włączony, Time 3/16 (1/8 z kropką = ")
           + juce::String(juce::roundToInt(t.dottedEighthMs)) + pl(" ms), spokojniej 1/4 (") + juce::String(juce::roundToInt(t.quarterMs))
           + pl(" ms), gęściej 1/8 (") + juce::String(juce::roundToInt(t.eighthMs))
           + pl(" ms); Feedback 25–35% (2–4 powtórzenia); filtr feedbacku LP ok. 3–5 kHz (każde powtórzenie ciemniejsze); "
                "Ping pong albo Stereo; Wet 100%, Dry 0%");
}

// Phase 35: the bass's low end against the kick's in time, from the bass Agent's own measurement
// (it needs the kick on its sidechain input). The lag is measured before the Agent's delay, so
// planning again gives the same delay.
struct AlignAdvice
{
    enum class Kind
    {
        notBass,     // only the bass is lined up with the kick
        noCable,     // no kick on its sidechain: nothing to measure against
        notMeasured, // cable in, but too little of both playing together yet
        inTime,      // within half a millisecond, or lining up would hardly gain anything
        polarity,    // they cancel as they are: the polarity first
        unclear,     // little in common below 150 Hz, or the best lag too far to be a timing slip
        early,       // the bass comes first: its Agent delays it
        late         // the bass comes later: only moving it earlier in FL helps
    };
    Kind kind = Kind::notBass;
    double lagMs = 0.0, best = 0.0, zero = 0.0;
    float delayMs = 0.0f;
};

constexpr double kAlignMinLagMs = 0.5;
constexpr double kAlignMaxLagMs = 5.0;

AlignAdvice alignAdvice(const ma::bus::AgentData& data)
{
    AlignAdvice a;
    if (data.role != static_cast<int>(ma::Role::bass))
        return a;
    a.lagMs = data.alignLagMs;
    a.best = data.alignBest;
    a.zero = data.alignZero;
    using K = AlignAdvice::Kind;
    if (data.sidechainConnected == 0)
        a.kind = K::noCable;
    else if (data.alignValid == 0)
        a.kind = K::notMeasured;
    else if (std::abs(a.lagMs) < kAlignMinLagMs)
        a.kind = K::inTime;
    else if (a.zero < -0.3 && a.best >= 0.4)
        a.kind = K::polarity;
    else if (a.best < 0.3 || std::abs(a.lagMs) > kAlignMaxLagMs)
        a.kind = K::unclear;
    else if (a.best - a.zero < 0.1)
        a.kind = K::inTime;
    else if (a.lagMs < 0.0)
    {
        a.kind = K::early;
        a.delayMs = static_cast<float>(std::round(-a.lagMs * 10.0) / 10.0);
    }
    else
        a.kind = K::late;
    return a;
}

juce::String alignText(const AlignAdvice& a)
{
    using K = AlignAdvice::Kind;
    const auto ms = [](double v) { return juce::String(std::abs(v), 1) + " ms"; };
    const auto gain = pl(" (zgodność dołu ze stopą ") + juce::String(a.zero, 2) + pl(" teraz, ") + juce::String(a.best, 2) + pl(" po wyrównaniu)");
    switch (a.kind)
    {
        case K::notBass: return pl("wyrównanie w czasie jest dla basu względem stopy");
        case K::noCable:
            return pl("nie mam z czym porównać: w FL wyślij stopę jako sidechain do kanału basu (prawy klik na strzałce wysyłki, "
                      "„Sidechain to this track”) i ustaw wejście sidechain w tym Agencie");
        case K::notMeasured: return pl("stopa i dół basu jeszcze za krótko grały razem, żeby zmierzyć przesunięcie");
        case K::inTime:
            return pl("dół basu idzie razem ze stopą (") + juce::String(a.lagMs, 1) + pl(" ms, przesunięcie niewiele by dało)");
        case K::polarity:
            return pl("stopa i bas znoszą się w dole (zgodność ") + juce::String(a.zero, 2)
                   + pl("): najpierw odwróć polaryzację basu, potem zmierzę czas jeszcze raz");
        case K::unclear:
            return a.best < 0.3
                       ? pl("stopa i bas mało grają razem w dole (zgodność ") + juce::String(a.best, 2)
                             + pl("): bas już ustępuje stopie albo grają na zmianę, nie ma czego wyrównywać")
                       : pl("najlepsze przesunięcie ") + ms(a.lagMs)
                             + pl(" to już pół okresu nuty, nie poślizg w czasie, więc nie przesuwam");
        case K::early:
            return pl("dół basu wchodzi ") + ms(a.lagMs) + pl(" przed stopą: opóźnienie ") + juce::String(a.delayMs, 1)
                   + pl(" ms, żeby dół stopy i basu się nie znosił") + gain;
        case K::late:
            return pl("dół basu wchodzi ") + ms(a.lagMs) + pl(" za stopą: przesuń bas o tyle wcześniej w FL (w Playliście albo "
                                                             "„Time offset” kanału w samplerze); wtyczka może tylko opóźniać")
                   + gain;
    }
    return {};
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


// A bus carries channels that have Agents of their own: in collisions, translation and the
// solver it would be counted twice and collide with its own sources.
bool inMixSums(const MixSnapshot::Channel& channel)
{
    return channel.data.role != static_cast<std::int32_t>(ma::Role::bus);
}

// Phase 23: does the channel's EQ already have this band (same place, depth and width, within what
// one more step of the solver would change)?
bool eqHasBand(const MixSnapshot::Channel& channel, const ma::EqBand& band)
{
    if (channel.data.eqBypass != 0 || std::abs(band.gainDb) < 0.05f)
        return false;
    for (int b = 0; b < ma::kEqBands; ++b)
    {
        const auto& have = channel.data.eq[b];
        if (std::abs(have.gainDb) < 0.05f || (have.gainDb > 0.0f) != (band.gainDb > 0.0f) || have.q <= 0.0f || band.q <= 0.0f)
            continue;
        if (std::abs(std::log2(have.hz / band.hz)) < 0.17f && std::abs(have.gainDb - band.gainDb) < 0.6f
            && std::abs(std::log2(have.q / band.q)) < 0.5f)
            return true;
    }
    return false;
}

// Phase 22: what a channel's own chain adds on top of its EQ (saturation, harmonics, attack), per band.
// The bus spectra are pre-chain, so without it a saturated sine bass would still read as empty above its sub.
double chainAddsDb(const MixSnapshot::Channel& channel, int band)
{
    const auto at = static_cast<size_t>(band);
    if (!channel.outputValid || !channel.spectrumValid || channel.outputDb[at] < -150.0)
        return 0.0;
    return std::max(0.0, channel.outputDb[at] - std::max(channel.heardDb[at], -150.0));
}

// Phase 22: does any channel (bus left out) play in this region, within 24 dB of its own loudest band?
// Then EQ (or a level move) can lift it; otherwise the hole is the arrangement's or the sound's.
bool carriedByAChannel(const std::vector<MixSnapshot::Channel>& channels, const ma::TonalRegion& region)
{
    constexpr double kCarryDb = 24.0;
    for (const auto& channel : channels)
    {
        if (!channel.spectrumValid || !inMixSums(channel))
            continue;
        double peak = -INFINITY, power = 0.0;
        int count = 0;
        for (int b = 0; b < ma::bus::kBands; ++b)
        {
            const double db = channel.heardDb[static_cast<size_t>(b)] + chainAddsDb(channel, b);
            peak = std::max(peak, db);
            if (b >= region.firstBand && b <= region.lastBand)
            {
                power += std::pow(10.0, db / 10.0);
                ++count;
            }
        }
        if (count > 0 && std::isfinite(peak) && 10.0 * std::log10(power / count + 1e-30) >= peak - kCarryDb)
            return true;
    }
    return false;
}

int groupOfChannel(const MixSnapshot::Channel& channel)
{
    return ma::stemGroupForRole(static_cast<ma::Role>(juce::jlimit(0, static_cast<int>(ma::Role::send), static_cast<int>(channel.data.role))));
}

// Phase 19: runs of the song's seconds (gaps up to 2 s bridged, runs under 2 s left out), as
// [start, end) in seconds.
std::vector<std::pair<double, double>> songRanges(const std::set<int>& seconds)
{
    std::vector<std::pair<double, double>> out;
    int start = 0, last = 0;
    bool open = false;
    auto close = [&] {
        if (open && last + 1 - start >= 2)
            out.emplace_back(start, last + 1);
    };
    for (const int s : seconds)
    {
        if (open && s - last <= 3)
        {
            last = s;
            continue;
        }
        close();
        start = last = s;
        open = true;
    }
    close();
    return out;
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

// Phase 12 and 16: how the report and the editor speak of each section.
BrainProcessor::SectionText BrainProcessor::sectionText(ma::StemSection section)
{
    switch (section)
    {
    case ma::StemSection::intro:
        return { pl("Intro jak w referencjach"), pl("intro"), pl("w intrach referencji"),
                 pl("Puść utwór od początku do pierwszego dropu (FL w trybie utworu, żeby Mózg znał pozycję): intro"),
                 pl("Proporcje w intro i jego głośność względem dropu jak w referencjach."),
                 pl("intro za głośne, drop nie robi wrażenia"), pl("intro za ciche, utwór rusza za słabo") };
    case ma::StemSection::build:
        return { pl("Build jak w referencjach"), pl("build"), pl("w buildach referencji"),
                 pl("Puść ostatnie 8 s przed dropem razem z wejściem dropu: build"),
                 pl("Proporcje w buildzie i jego głośność względem dropu jak w referencjach."),
                 pl("build za głośny, drop się nie wybija"), pl("build za cichy, napięcie nie rośnie") };
    case ma::StemSection::breakdown: break;
    }
    return { pl("Break jak w referencjach"), pl("break"), pl("w breakach referencji"), pl("Puść break (część bez stopy) po dropie:"),
             pl("Proporcje w breaku i jego głośność względem dropu jak w referencjach."), pl("drop się nie wybija"),
             pl("break za bardzo siada") };
}

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
            for (int g = 0; g < ma::kStemGroups; ++g)
                if (groups[static_cast<size_t>(g)])
                    profiler.measureGroup(g, perGroup[static_cast<size_t>(g)].getReadPointer(0), perGroup[static_cast<size_t>(g)].getReadPointer(1), len);
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
    autoRefine = parameters.getRawParameterValue(autoRefineId);
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
    selectLastTarget(); // phase 30: a new project starts with the target used last
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
    // Phase 30: after "Zastosuj", measure again and finish what is left by itself.
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID { autoRefineId, 4 }, "Auto refine", true));

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
    ghostDrums.prepare(sampleRate);
    ghostDrums.setPulse(0.07);
    ghostRest.prepare(sampleRate);
    ghostVersionSeen = -1;
    ghostGain.reset(sampleRate, 0.4);
    ghostGain.setCurrentAndTargetValue(0.0f);
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

    double beatPhase = 0.0; // phase 21: where the block starts within a beat (for a drum ghost)
    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
        {
            if (const auto ppq = position->getPpqPosition())
                beatPhase = *ppq - std::floor(*ppq);
            if (const auto bpm = position->getBpm())
                hostBpm.store(*bpm);
            const auto seconds = position->getTimeInSeconds();
            songSeconds.store(position->getIsPlaying() && seconds.hasValue() ? *seconds : -1.0);
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
        const auto tilt = masterPreview.limiterTiltDb();
        for (size_t b = 0; b < previewTilt.size(); ++b)
            previewTilt[b].store(static_cast<float>(tilt[b]));
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

    // Phase 21: the ghosts, after every measurement (the analysis never hears them) and before the
    // simulated system (so a phone preview has them too).
    if (const int version = ghostVersion.load(); version != ghostVersionSeen)
    {
        ghostVersionSeen = version;
        ma::SpectrumAnalyzer::Bands drumsDb {}, restDb {};
        for (size_t b = 0; b < drumsDb.size(); ++b)
        {
            drumsDb[b] = ghostDrumsDb[b].load();
            restDb[b] = ghostRestDb[b].load();
        }
        ghostDrums.setSpectrum(drumsDb, ghostDrumsWidth.load());
        ghostRest.setSpectrum(restDb, ghostRestWidth.load());
    }
    ghostGain.setTargetValue(ghostAudible.load() ? 1.0f : 0.0f);
    if (ghostGain.isSmoothing() || ghostGain.getTargetValue() > 0.0f)
    {
        float* left = buffer.getWritePointer(0);
        float* right = numChannels >= 2 ? buffer.getWritePointer(1) : nullptr;
        const double bpm = hostBpm.load();
        const double beatSeconds = bpm > 0.0 ? 60.0 / bpm : 0.0;
        // In steps of 32 samples, so the fade stays smooth without a per-sample gain in GhostNoise.
        for (int start = 0; start < numSamples; start += 32)
        {
            const int len = std::min(32, numSamples - start);
            const float gain = ghostGain.getNextValue();
            ghostGain.skip(len - 1);
            const double phase = beatSeconds > 0.0 ? beatPhase + start / (beatSeconds * getSampleRate()) : 0.0;
            ghostDrums.process(left + start, right != nullptr ? right + start : nullptr, len, gain, phase, beatSeconds);
            ghostRest.process(left + start, right != nullptr ? right + start : nullptr, len, gain);
        }
    }

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
        masking[c].valid = channel.spectrumValid && inMixSums(channel);
        masking[c].priority = channel.data.priority;
        masking[c].background = channel.data.role == static_cast<int>(ma::Role::send);
        for (int b = 0; b < ma::bus::kBands; ++b)
            masking[c].rawDb[static_cast<size_t>(b)] = channel.heardDb[static_cast<size_t>(b)];
    }
    mix.masking = ma::analyseMasking(masking);
    mix.tonal = {};
    tonalPlain = {};
    if (activeTarget().valid() && !capturing && !comparing && secondsAnalysed() >= kMinimumSeconds)
    {
        mix.tonal = ma::compareToTarget(tonalMasterSpectrum(), activeTarget());
        tonalPlain = ma::compareToTarget(averageSpectrum(), activeTarget());
    }

    std::vector<ma::TranslationChannel> translation(mix.channels.size());
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        const auto& channel = mix.channels[c];
        translation[c].valid = (channel.outputValid || channel.spectrumValid) && inMixSums(channel);
        for (int b = 0; b < ma::bus::kBands; ++b)
            translation[c].rawDb[static_cast<size_t>(b)] = channel.outputValid ? channel.outputDb[static_cast<size_t>(b)]
                                                                               : channel.heardDb[static_cast<size_t>(b)];
    }
    mix.translation = ma::analyseTranslation(translation);

    updateSketch();
    updateGhostNoise();

    mix.stemMeasure = {};
    mix.stems = {};
    if (stemsReady())
    {
        std::array<bool, ma::kStemGroups> present {}, ghostOnly {};
        std::array<ma::SpectrumAnalyzer::Bands, ma::kStemGroups> side {};
        auto groups = stemGroupDb(true, present, &side, nullptr);
        // A sketch's ghosts count in the sum, so the groups that play are not all too loud
        // against it, and a hole the arrangement leaves is not asked of the EQ.
        addGhostGroups(groups, present, ghostOnly);
        mix.stemMeasure = ma::measureStemGroups(groups, side, present);
        mix.stems = ma::compareStems(mix.stemMeasure, keyedStems);
        for (int g = 0; g < ma::kStemGroups; ++g)
            if (ghostOnly[static_cast<size_t>(g)])
            {
                mix.stemMeasure.groups[static_cast<size_t>(g)] = {};
                mix.stems.groups[static_cast<size_t>(g)] = {};
                mix.stems.groups[static_cast<size_t>(g)].missing = true;
            }
        // What the cleanup or the Brain's cuts took out on purpose is not asked back.
        for (int g = 0; g < ma::kStemGroups; ++g)
            for (int r = 0; r < ma::kStemRegions; ++r)
            {
                auto& d = mix.stems.groups[static_cast<size_t>(g)].shapeDeviationDb[static_cast<size_t>(r)];
                if (d < 0.0 && cutOnPurpose(g, r))
                    d = 0.0;
            }
    }
    updateSongSections();
    // Phase 19: the drop's groups as the sections are measured, so no automation raises a group above it.
    std::array<bool, ma::kStemGroups> dropPresent {};
    const auto dropGroups = stemsReady() ? stemGroupDb(true, dropPresent, nullptr, nullptr)
                                         : std::array<ma::SpectrumAnalyzer::Bands, ma::kStemGroups> {};
    for (int s = 0; s < ma::kStemSections; ++s)
    {
        const auto section = static_cast<ma::StemSection>(s);
        const auto& sums = sectionSums(section);
        auto& out = mix.sections[static_cast<size_t>(s)];
        out = {};
        out.seconds = static_cast<double>(sums.updates) / kUpdateHz;
        if (!sectionReady(section))
            continue;
        std::array<bool, ma::kStemGroups> present {};
        std::array<ma::SpectrumAnalyzer::Bands, ma::kStemGroups> side {};
        const auto groups = stemGroupDb(true, present, &side, nullptr, &sums);
        out.measure = ma::measureStemGroups(groups, side, present);
        out.stems = ma::compareStems(out.measure, keyedSectionStems[static_cast<size_t>(s)]);
        out.belowDropDb = dropLevelSum / static_cast<double>(std::max(1L, dropUpdates)) - sums.levelSum / static_cast<double>(sums.updates);
        out.automation = ma::planSectionAutomation(groups, present, keyedSectionStems[static_cast<size_t>(s)],
                                                   keyedSectionStems[static_cast<size_t>(s)].contrast(section), out.belowDropDb, &dropGroups);
        out.ranges = songRanges(sums.seconds);
        // A break comes after the first drop; seconds before it caught by an early guess of the drop are not shown.
        if (section == ma::StemSection::breakdown && std::isfinite(firstDropSecond))
            out.ranges.erase(std::remove_if(out.ranges.begin(), out.ranges.end(),
                                            [this](const auto& r) { return r.second <= firstDropSecond; }),
                             out.ranges.end());
    }
    // Phase 20: the energy map.
    mix.energy.ours = songEnergy();
    mix.energy.references = ma::energyCorridor(keyedStems);
    mix.energy.comparison = ma::compareEnergy(mix.energy.ours, mix.energy.references);
    updateLoudnessMatch(now);
    adaptStep(now);
    updateRefine(now);
    buildReport();
}

ma::SolverResult BrainProcessor::solveNow(double headroomGainDb, const ma::StemShapeInput* stems,
                                          const std::array<double, ma::kStemGroups>* groupGain) const
{
    std::vector<ma::SolverChannel> channels(mix.channels.size());
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        const auto& channel = mix.channels[c];
        channels[c].valid = channel.spectrumValid && inMixSums(channel);
        channels[c].priority = channel.data.priority;
        channels[c].background = channel.data.role == static_cast<int>(ma::Role::send);
        for (int b = 0; b < ma::bus::kBands; ++b)
        {
            channels[c].rawDb[static_cast<size_t>(b)] = channel.bandsDb[static_cast<size_t>(b)];
            channels[c].nowDb[static_cast<size_t>(b)] = channel.measuredDb[static_cast<size_t>(b)];
        }
        // Phase 18: what the master carries now and what this proposal moves, so the tonal stage
        // neither redoes what is applied nor corrects twice what a level move already does.
        channels[c].hasNow = true;
        if (channel.data.eqBypass == 0)
            for (int b = 0; b < ma::kEqBands; ++b)
                channels[c].currentEq[static_cast<size_t>(b)] = channel.data.eq[b];
        const int group = groupOfChannel(channel);
        channels[c].gainDb = groupGain != nullptr && group >= 0 ? (*groupGain)[static_cast<size_t>(group)] : 0.0;
        if (const auto balance = balanceGain.find(channel.owner); balance != balanceGain.end())
            channels[c].gainDb += balance->second;
        if (const auto extra = translationExtra.find(channel.owner); extra != translationExtra.end())
            channels[c].extraDb = extra->second;
    }
    // Phase 17: the ghosts after the channels, so the indices of the channels stay.
    if (mix.sketch.active())
        for (const auto& ghost : mix.sketch.analysis.ghosts)
        {
            ma::SolverChannel channel;
            channel.valid = true;
            channel.ghost = true;
            channel.priority = ghost.priority;
            channel.rawDb = ghost.db;
            channels.push_back(channel);
        }
    ma::SolverOptions options;
    options.headroomGainDb = headroomGainDb;
    if (mix.sketch.active() && mix.sketch.tonal.valid)
        options.tonal = &mix.sketch.tonal; // holes the ghosts fill are the arrangement's, not the EQ's
    else if (mix.tonal.valid)
        options.tonal = &mix.tonal;
    if (mix.tonal.valid)
    {
        options.tonalTarget = &activeTarget(); // phase 38: the built-in one until the user has a target
        options.tonalMasterDb = mix.sketch.active() ? ma::addGhosts(tonalMasterSpectrum(), mix.sketch.analysis) : tonalMasterSpectrum();
        options.centreZones = true; // phase 24
    }
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
        applyCleanup(channel, channel.data.cleanup, channel.data.depth);
        channel.measuredDb = channel.heardDb;

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

void BrainProcessor::applyCleanup(MixSnapshot::Channel& channel, const ma::CleanupSettings& cleanup, const ma::DepthSettings& depth)
{
    ma::EqSettings eq {};
    if (channel.data.eqBypass == 0)
        for (int b = 0; b < ma::kEqBands; ++b)
            eq[static_cast<size_t>(b)] = channel.data.eq[b];
    for (int b = 0; b < ma::bus::kBands; ++b)
    {
        const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
        const auto band = static_cast<size_t>(b);
        channel.bandsDb[band] = std::max(-200.0, channel.rawDb[band] + ma::cleanupResponseDb(cleanup, hz) + ma::depthResponseDb(depth, hz));
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
        dropDrumsSum = dropLevelSum = 0.0;
        songBins.clear();
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
            dropDrumsSum = dropLevelSum = 0.0;
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

    // Phase 16: every update into its second of the song, whatever it turns out to be.
    const double position = songSeconds.load() - kSpectrumLagSeconds;
    if (position >= 0.0 && position < kMaxSongSeconds)
    {
        auto& bin = songBins[static_cast<int>(position)];
        accumulate(bin.sums);
        ++bin.updates;
        bin.levelSum += level;
    }

    if (level < dropLevel - 3.0)
    {
        // Phase 12: the break, once the drop is known: the drums 15 dB under their drop level,
        // the rest within 20 dB of the drop.
        const bool dropKnown = dropUpdates >= static_cast<long>(kMinimumDropSeconds * kUpdateHz) && dropDrumsSum > 0.0;
        if (anyDrums && dropKnown && level >= dropLevel - 20.0
            && drumsNow <= dropDrumsSum / static_cast<double>(dropUpdates) * std::pow(10.0, -1.5))
        {
            accumulate(breakSums.sums);
            ++breakSums.updates;
            breakSums.levelSum += level;
            if (position >= 0.0 && position < kMaxSongSeconds)
                breakSums.seconds.insert(static_cast<int>(position));
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
        if (channel.outputValid && channel.spectrumValid)
        {
            double in = 0.0, out = 0.0;
            for (int b = 0; b < ma::bus::kBands; ++b)
            {
                const auto at = static_cast<size_t>(b);
                const double w = ma::kWeightingDb(ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b));
                in += std::pow(10.0, (channel.heardDb[at] + w) / 10.0);
                out += std::pow(10.0, (channel.outputDb[at] + w) / 10.0);
            }
            sum.chainIn += in;
            sum.chainOut += out;
            ++sum.chainUpdates;
        }
    }
    ++dropUpdates;
    mix.dropSeconds = static_cast<double>(dropUpdates) / kUpdateHz;
}

bool BrainProcessor::sectionReady(ma::StemSection s) const
{
    const double minimum = s == ma::StemSection::build ? kMinimumBuildSeconds : kMinimumDropSeconds;
    return keyedSectionStems[static_cast<size_t>(s)].valid() && stemsReady()
           && sectionSums(s).updates >= static_cast<long>(minimum * kUpdateHz);
}

void BrainProcessor::updateSongSections()
{
    introSums.clear();
    buildSums.clear();
    firstDropSecond = INFINITY;
    if (!stemsReady() || !std::isfinite(dropLevel))
        return;
    // Where the drop first came in: the first run of seconds at its level (each second's own
    // loudness, so a later, louder drop does not move the intro), moved back by the loudness's lag.
    auto levelOf = [](const SectionSums& bin) { return bin.levelSum / static_cast<double>(std::max(1L, bin.updates)); };
    for (auto it = songBins.begin(); it != songBins.end() && !std::isfinite(firstDropSecond); ++it)
    {
        bool run = true;
        for (int k = 0; k < kDropRunSeconds && run; ++k)
        {
            const auto next = songBins.find(it->first + k);
            run = next != songBins.end() && levelOf(next->second) >= dropLevel - 3.0;
        }
        if (run)
            firstDropSecond = std::max(0.0, it->first - kLoudnessLagSeconds);
    }
    if (!std::isfinite(firstDropSecond))
        return;
    for (const auto& [second, bin] : songBins)
    {
        // By each second's middle: the build the last 8 s before the drop, the intro what played before.
        // Phase 19: a second on either side of a boundary is left out, so the spectra's smoothing
        // does not carry one section into the next (and an automated part into the other).
        const double middle = second + 0.5;
        const double buildStart = firstDropSecond - kBuildSeconds;
        const bool playing = levelOf(bin) >= dropLevel - kSectionRangeDb;
        // Where the section is (for the automation's bars) and what is measured of it.
        SectionSums* range = middle < buildStart ? (playing ? &introSums : nullptr) : middle < firstDropSecond ? &buildSums : nullptr;
        if (range != nullptr)
            range->seconds.insert(second);
        SectionSums* into = nullptr;
        if (middle < buildStart - kSectionGuardSeconds && playing)
            into = &introSums;
        else if (middle >= buildStart + kSectionGuardSeconds && middle < firstDropSecond - kSectionGuardSeconds)
            into = &buildSums;
        if (into == nullptr)
            continue;
        for (const auto& [owner, sum] : bin.sums)
        {
            auto& total = into->sums[owner];
            for (size_t b = 0; b < total.power.size(); ++b)
            {
                total.power[b] += sum.power[b];
                total.side[b] += sum.side[b];
            }
        }
        into->updates += bin.updates;
        into->levelSum += bin.levelSum;
    }
}

ma::EnergyTimeline BrainProcessor::songEnergy() const
{
    ma::EnergyTimeline out;
    if (songBins.empty() || dropUpdates <= 0)
        return out;
    const double dropAverage = dropLevelSum / static_cast<double>(dropUpdates);
    const auto length = static_cast<size_t>(songBins.rbegin()->first + 1);
    out.levelDb.assign(length, NAN);
    // Each channel's group, and what counts in the sum (not buses: their sources are there already).
    std::map<std::uint64_t, int> groupOf;
    std::map<std::uint64_t, bool> counts;
    std::array<bool, ma::kStemGroups> any {};
    for (const auto& channel : mix.channels)
    {
        const int g = groupOfChannel(channel);
        groupOf[channel.owner] = g;
        counts[channel.owner] = inMixSums(channel);
        if (g >= 0)
            any[static_cast<size_t>(g)] = true;
    }
    for (int g = 0; g < ma::kStemGroups; ++g)
        if (any[static_cast<size_t>(g)])
            out.groupDb[static_cast<size_t>(g)].assign(length, NAN);
    static const auto weights = [] {
        std::array<double, ma::bus::kBands> w {};
        for (int b = 0; b < ma::bus::kBands; ++b)
            w[static_cast<size_t>(b)] = std::pow(10.0, ma::kWeightingDb(ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b)) / 10.0);
        return w;
    }();
    for (const auto& [second, bin] : songBins)
    {
        if (second < 0 || bin.updates <= 0)
            continue;
        const auto at = static_cast<size_t>(second);
        const double level = std::max(ma::kEnergyFloorDb, bin.levelSum / static_cast<double>(bin.updates) - dropAverage);
        out.levelDb[at] = level;
        // Each group's share of the sum, as the loudness meter weighs it.
        std::array<double, ma::kStemGroups> power {};
        double total = 0.0;
        for (const auto& [owner, sum] : bin.sums)
        {
            const auto counted = counts.find(owner);
            if (counted == counts.end() || !counted->second)
                continue;
            double p = 0.0;
            for (size_t b = 0; b < sum.power.size() && b < weights.size(); ++b)
                p += sum.power[b] * weights[b];
            total += p;
            const int g = groupOf[owner];
            if (g >= 0)
                power[static_cast<size_t>(g)] += p;
        }
        for (int g = 0; g < ma::kStemGroups; ++g)
        {
            auto& values = out.groupDb[static_cast<size_t>(g)];
            if (!values.empty())
                values[at] = total > 0.0 && power[static_cast<size_t>(g)] > 0.0
                                 ? std::max(ma::kEnergyFloorDb, level + 10.0 * std::log10(power[static_cast<size_t>(g)] / total))
                                 : ma::kEnergyFloorDb;
        }
    }
    out.valid = true;
    return out;
}

bool BrainProcessor::stemsReady() const
{
    return currentStems.valid() && mix.dropSeconds >= kMinimumDropSeconds && dropUpdates > 0;
}

// Phase 29: no stem references, but the drop is measured: the groups get the typical balance.
bool BrainProcessor::typicalLevelsReady() const
{
    return !currentStems.valid() && mix.dropSeconds >= kMinimumDropSeconds && dropUpdates > 0;
}

std::array<ma::SpectrumAnalyzer::Bands, ma::kStemGroups>
BrainProcessor::stemGroupDb(bool withEq, std::array<bool, ma::kStemGroups>& present,
                            std::array<ma::SpectrumAnalyzer::Bands, ma::kStemGroups>* side,
                            const std::vector<ma::EqSettings>* eqs, const SectionSums* section) const
{
    std::array<ma::SpectrumAnalyzer::Bands, ma::kStemGroups> power {}, sidePower {};
    present = {};
    const auto& sums = section != nullptr ? section->sums : drop;
    const double updates = static_cast<double>(std::max(1L, section != nullptr ? section->updates : dropUpdates));
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
        double loss = chainLossDb(channel.owner);
        if (const auto balance = balanceGain.find(channel.owner); balance != balanceGain.end())
            loss += balance->second;
        for (int b = 0; b < ma::bus::kBands; ++b)
        {
            const auto at = static_cast<size_t>(b);
            const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
            const double gain = std::pow(10.0, (ma::cleanupResponseDb(cleanup, hz) + ma::eqResponseDb(eq, hz) + loss) / 10.0);
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

const MixSnapshot::Channel* BrainProcessor::findChannel(std::uint64_t owner) const
{
    for (const auto& channel : mix.channels)
        if (channel.owner == owner)
            return &channel;
    return nullptr;
}

BrainProcessor::SendSourceInfo BrainProcessor::sendSources(std::uint64_t sendOwner) const
{
    SendSourceInfo info;
    if (songBins.empty())
        return info;
    const int first = songBins.begin()->first, last = songBins.rbegin()->first;
    const auto frames = static_cast<size_t>(last - first + 1);
    const auto frameOf = [&](std::uint64_t owner, std::vector<ma::SourceFrame>& out) {
        out.assign(frames, ma::SourceFrame {});
        for (const auto& [second, bin] : songBins)
        {
            const auto found = bin.sums.find(owner);
            if (found == bin.sums.end() || bin.updates <= 0)
                continue;
            ma::SpectrumAnalyzer::Bands power {};
            for (int b = 0; b < ma::bus::kBands; ++b)
                power[static_cast<size_t>(b)] = found->second.power[static_cast<size_t>(b)] / static_cast<double>(bin.updates);
            out[static_cast<size_t>(second - first)] = ma::sourceFrame(power);
        }
    };
    std::vector<ma::SourceFrame> send;
    frameOf(sendOwner, send);
    std::vector<std::vector<ma::SourceFrame>> candidates;
    for (const auto& channel : mix.channels)
    {
        // Sends and buses are not sources (a bus carries its sources, which have Agents of their own).
        if (channel.owner == sendOwner || !inMixSums(channel) || channel.data.role == static_cast<int>(ma::Role::send))
            continue;
        info.owners.push_back(channel.owner);
        candidates.emplace_back();
        frameOf(channel.owner, candidates.back());
    }
    const auto found = ma::findSendSources(send, candidates);
    info.valid = found.valid;
    info.seconds = found.seconds;
    info.fit = found.fit;
    info.share = found.share;
    if (found.dominant >= 0)
        info.dominantOwner = info.owners[static_cast<size_t>(found.dominant)];
    return info;
}

juce::String BrainProcessor::sendSourcesText(const SendSourceInfo& info) const
{
    if (!info.valid)
        return info.seconds < ma::kMinSourceSeconds
                   ? pl("źródło jeszcze nieznane (") + juce::String(info.seconds) + " / " + juce::String(ma::kMinSourceSeconds) + pl(" s utworu)")
                   : pl("źródło nierozpoznane");
    std::vector<std::pair<double, std::uint64_t>> parts;
    for (size_t k = 0; k < info.owners.size(); ++k)
        if (info.share[k] >= 0.15)
            parts.push_back({ info.share[k], info.owners[k] });
    std::sort(parts.begin(), parts.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    juce::StringArray names;
    for (const auto& [share, owner] : parts)
        if (const auto* c = findChannel(owner))
            names.add(quoted(c->name) + " " + juce::String(juce::roundToInt(share * 100.0)) + "%");
    return pl("zasila ją głównie ") + (names.isEmpty() ? pl("kilka kanałów po trochu") : joinNames(names));
}

ma::SpectrumAnalyzer::Bands BrainProcessor::dropSpectrumDb(std::uint64_t owner) const
{
    ma::SpectrumAnalyzer::Bands db {};
    db.fill(-200.0);
    const auto found = drop.find(owner);
    if (found == drop.end() || dropUpdates < static_cast<long>(kMinimumDropSeconds * kUpdateHz) / 2)
        return db;
    for (int b = 0; b < ma::bus::kBands; ++b)
    {
        const double p = found->second.power[static_cast<size_t>(b)] / static_cast<double>(dropUpdates);
        db[static_cast<size_t>(b)] = p > 1e-20 ? 10.0 * std::log10(p) : -200.0;
    }
    return db;
}

double BrainProcessor::chainLossDb(std::uint64_t owner) const
{
    const auto found = drop.find(owner);
    if (found == drop.end() || found->second.chainUpdates < static_cast<long>(kMinimumDropSeconds * kUpdateHz) / 2
        || found->second.chainIn <= 1e-20 || found->second.chainOut <= 1e-20)
        return 0.0;
    // Only what the chain takes away: what it adds (saturation, harmonics) is counted where it lands.
    return std::clamp(10.0 * std::log10(found->second.chainOut / found->second.chainIn), -kMaxChainLossDb, 0.0);
}

void BrainProcessor::planGroupBalance()
{
    balanceGain.clear();
    const double updates = static_cast<double>(std::max(1L, dropUpdates));
    struct Part
    {
        std::uint64_t owner;
        double heardDb, rawDb; // K-weighted over the drop (as Learn measures): as the mix hears it after the chain, and before trim and chain
        bool kick;
    };
    std::array<std::vector<Part>, ma::kStemGroups> parts;
    for (const auto& channel : mix.channels)
    {
        const int group = groupOfChannel(channel);
        const auto found = drop.find(channel.owner);
        // Only the arrangement's parts (music, vocals). In both real songs the stems' own drums and bass
        // balance (hats and the sub far louder than the pro mixes have them) made the mix worse, so
        // there the roles' levels from Learn stay.
        const bool arrangement = group == static_cast<int>(ma::StemGroup::music) || group == static_cast<int>(ma::StemGroup::vocals);
        if (!arrangement || found == drop.end() || !inMixSums(channel))
            continue;
        ma::EqSettings eq {};
        if (channel.data.eqBypass == 0)
            for (int b = 0; b < ma::kEqBands; ++b)
                eq[static_cast<size_t>(b)] = channel.data.eq[b];
        double power = 0.0;
        for (int b = 0; b < ma::bus::kBands; ++b)
        {
            const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
            power += found->second.power[static_cast<size_t>(b)] / updates
                     * std::pow(10.0, (ma::cleanupResponseDb(channel.data.cleanup, hz) + ma::eqResponseDb(eq, hz) + ma::kWeightingDb(hz)) / 10.0);
        }
        if (power <= 1e-12) // under -120 dB: not playing in the drop
            continue;
        const double loss = chainLossDb(channel.owner);
        const double heard = 10.0 * std::log10(power) + loss;
        parts[static_cast<size_t>(group)].push_back({ channel.owner, heard, heard - channel.data.trimDb - loss });
    }
    for (const auto& group : parts)
    {
        if (group.size() < 2)
            continue;
        // The group keeps its loudness, so the group levels see the same group as before.
        double heard = 0.0, raw = 0.0;
        for (const auto& part : group)
        {
            heard += std::pow(10.0, part.heardDb / 10.0);
            raw += std::pow(10.0, part.rawDb / 10.0);
        }
        const double shift = 10.0 * std::log10(heard / raw);
        for (const auto& part : group)
        {
            const double gain = std::clamp(part.rawDb + shift - part.heardDb, -kMaxBalanceDb, kMaxBalanceDb);
            if (std::abs(gain) >= kMinBalanceDb)
                balanceGain[part.owner] = std::round(gain * 2.0) / 2.0;
        }
    }
}

ma::StemShapeInput BrainProcessor::stemShapeInput() const
{
    ma::StemShapeInput in;
    if (!stemsReady())
        return in;
    in.target = &keyedStems;
    if (mix.sketch.active())
        in.arrangementHole = mix.sketch.analysis.hole;
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

void BrainProcessor::setSketchMode(bool on)
{
    sketchOn = on;
    parameters.state.setProperty("sketchMode", on, nullptr);
}

void BrainProcessor::setTonalAfterLimiter(bool on)
{
    afterLimiterOn = on;
    parameters.state.setProperty("tonalAfterLimiter", on, nullptr);
}

bool BrainProcessor::limiterTiltKnown() const
{
    return previewSeconds.load() >= kMinimumSeconds;
}

ma::Curve BrainProcessor::limiterTilt() const
{
    ma::Curve tilt {};
    if (!limiterTiltKnown())
        return tilt;
    // Only the shape counts (the level is the master target's): the mean over the range the tonal
    // balance is normalised on comes off first. Our limiter is a plain broadband one; a mastering
    // maximizer is gentler on the low end, so what is left is capped.
    double sum = 0.0;
    int count = 0;
    for (int b = 0; b < numMasterBands(); ++b)
    {
        const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::kTonalScale, b);
        if (hz >= 60.0 && hz <= 8000.0)
        {
            sum += previewTilt[static_cast<size_t>(b)].load();
            ++count;
        }
    }
    const double mean = count > 0 ? sum / count : 0.0;
    for (int b = 0; b < numMasterBands(); ++b)
        tilt[static_cast<size_t>(b)] = juce::jlimit(-kMaxTiltDb, kMaxTiltDb, previewTilt[static_cast<size_t>(b)].load() - mean);
    return tilt;
}

constexpr std::uint32_t kLostRefreshMs = 2000;
constexpr size_t kMaxLostListed = 5;
constexpr double kMonoFoldLowHz = 150.0, kMonoFoldHighHz = 8000.0;
constexpr double kMonoFoldAudibleDb = -30.0; // a part this far under the mix does not count
constexpr double kMonoFoldWarnDb = 3.0, kMonoFoldProblemDb = 6.0;
// Phase 39: what is left between the mix and the target as bells for Ozone's EQ on the master: each
// region outside the corridor (its own width), then each of TBC's ranges still off the centre that no
// region covers; at most +-4 dB each (more belongs in the mix).
juce::StringArray BrainProcessor::ozoneCurve(const ma::TonalComparison& tonal) const
{
    juce::StringArray bells;
    if (!tonal.valid)
        return bells;
    const auto bell = [&](double lowHz, double highHz, double gainDb) {
        const double ratio = std::max(1.05, highHz / lowHz);
        const double q = juce::jlimit(0.3, 4.0, std::sqrt(ratio) / (ratio - 1.0));
        bells.add(hzText(std::sqrt(lowHz * highHz)) + " " + signedOneDecimal(juce::jlimit(-4.0, 4.0, gainDb)) + " dB, Q "
                  + juce::String(q, 1));
    };
    for (const auto& region : tonal.regions)
        if (std::abs(region.deviationDb) >= 0.75)
            bell(region.lowHz, region.highHz, -region.deviationDb);
    const auto zones = ma::zoneOffsets(tonal, activeTarget());
    for (int z = 0; z < ma::kTonalZones; ++z)
    {
        const auto& zone = zones[static_cast<size_t>(z)];
        const bool covered = std::any_of(tonal.regions.begin(), tonal.regions.end(), [&](const ma::TonalRegion& region) {
            return region.lowHz < zone.highHz && region.highHz > zone.lowHz;
        });
        if (zone.valid && !covered && std::abs(zone.offsetDb) > 0.75)
            bell(zone.lowHz, zone.highHz, -zone.offsetDb);
    }
    return bells;
}

juce::String BrainProcessor::builtinNote() const
{
    return currentTarget.valid() ? juce::String()
                                 : pl(" To wbudowany cel z dwóch masterów house, szeroki jak korytarz TBC; własny („Nowy cel”, "
                                      "2-3 referencje) zawsze ma pierwszeństwo.");
}

ma::Curve BrainProcessor::tonalMasterSpectrum() const
{
    auto master = averageSpectrum();
    if (afterLimiterOn && limiterTiltKnown())
    {
        const auto tilt = limiterTilt();
        for (size_t b = 0; b < master.size(); ++b)
            if (master[b] > -150.0)
                master[b] += tilt[b];
    }
    return master;
}

void BrainProcessor::updateGhostNoise()
{
    // The ghosts' power summed into two layers: the drums (pulsed on the beat) and the rest.
    std::array<double, ma::SpectrumAnalyzer::kMaxBands> drums {}, rest {};
    double drumsWidth = 0.0, restWidth = 0.0, restPower = 0.0;
    const bool active = mix.sketch.active();
    if (active)
        for (const auto& ghost : mix.sketch.analysis.ghosts)
        {
            auto& into = ghost.group == static_cast<int>(ma::StemGroup::drums) ? drums : rest;
            double power = 0.0;
            for (size_t b = 0; b < into.size(); ++b)
                if (ghost.db[b] > -150.0)
                {
                    const double p = std::pow(10.0, ghost.db[b] / 10.0);
                    into[b] += p;
                    power += p;
                }
            const auto& group = mix.sketch.arrangement.groups[static_cast<size_t>(std::clamp(ghost.group, 0, ma::kStemGroups - 1))];
            const double width = group.widthValid ? group.width : (ghost.group == static_cast<int>(ma::StemGroup::music) ? 0.3 : 0.1);
            if (ghost.group == static_cast<int>(ma::StemGroup::drums))
                drumsWidth = std::max(drumsWidth, width);
            else
            {
                restWidth += width * power; // power-weighted
                restPower += power;
            }
        }
    for (size_t b = 0; b < drums.size(); ++b)
    {
        ghostDrumsDb[b].store(drums[b] > 0.0 ? static_cast<float>(10.0 * std::log10(drums[b])) : -200.0f);
        ghostRestDb[b].store(rest[b] > 0.0 ? static_cast<float>(10.0 * std::log10(rest[b])) : -200.0f);
    }
    ghostDrumsWidth.store(static_cast<float>(drumsWidth));
    ghostRestWidth.store(static_cast<float>(restPower > 0.0 ? restWidth / restPower : 0.0));
    ghostVersion.fetch_add(1);
    // Ghosts are drop parts: they play while the mix is at the drop's level.
    const double dropAverage = dropUpdates > 0 ? dropLevelSum / static_cast<double>(dropUpdates) : NAN;
    const double level = shortTermLufs();
    ghostAudible.store(listenGhostsOn.load() && active && std::isfinite(dropAverage) && std::isfinite(level)
                       && level >= dropAverage - kGhostDropRangeDb && songSeconds.load() >= 0.0);
}

void BrainProcessor::updateSketch()
{
    auto& sketch = mix.sketch;
    sketch = {};
    if (!sketchOn || useProfiles || mix.dropSeconds < kMinimumDropSeconds || dropUpdates == 0)
        return;
    if (currentStems.valid())
        sketch.arrangement = ma::arrangementFromStems(keyedStems);
    if (!sketch.arrangement.valid())
        sketch.arrangement = ma::typicalArrangement();

    ma::SketchInput in;
    in.groupDb = stemGroupDb(true, in.present, nullptr, nullptr);
    sketch.groupDb = in.groupDb;
    for (const auto& channel : mix.channels)
    {
        const int group = groupOfChannel(channel);
        const auto found = drop.find(channel.owner);
        if (group < 0 || found == drop.end())
            continue;
        ma::EqSettings eq {};
        if (channel.data.eqBypass == 0)
            for (int b = 0; b < ma::kEqBands; ++b)
                eq[static_cast<size_t>(b)] = channel.data.eq[b];
        ma::SketchChannel own;
        own.group = group;
        for (int b = 0; b < ma::bus::kBands; ++b)
        {
            const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
            const double p = found->second.power[static_cast<size_t>(b)] / static_cast<double>(dropUpdates);
            // Phase 22: with what its chain adds, so a part filled by saturation is no longer a ghost.
            own.db[static_cast<size_t>(b)] = p > 1e-20 ? 10.0 * std::log10(p) + ma::cleanupResponseDb(channel.data.cleanup, hz) + ma::eqResponseDb(eq, hz)
                                                             + chainAddsDb(channel, b)
                                                       : -200.0;
        }
        in.channels.push_back(own);
    }
    for (int g = 0; g < ma::kStemGroups; ++g)
        for (int r = 0; r < ma::kStemRegions; ++r)
            in.cutOnPurpose[static_cast<size_t>(g)][static_cast<size_t>(r)] = cutOnPurpose(g, r);
    sketch.analysis = ma::analyseSketch(in, sketch.arrangement);
    if (!sketch.active())
        return;

    if (mix.tonal.valid)
    {
        sketch.tonal = ma::compareToTarget(ma::addGhosts(tonalMasterSpectrum(), sketch.analysis), activeTarget());
        sketch.arrangementRegion = ma::arrangementRegions(mix.tonal, sketch.tonal);
    }

    // Who will cover a part that is not there yet.
    const auto& ghosts = sketch.analysis.ghosts;
    std::vector<ma::MaskingChannel> masking(mix.channels.size() + ghosts.size());
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        const auto& channel = mix.channels[c];
        masking[c].valid = channel.spectrumValid && inMixSums(channel);
        masking[c].priority = channel.data.priority;
        masking[c].background = channel.data.role == static_cast<int>(ma::Role::send);
        for (int b = 0; b < ma::bus::kBands; ++b)
            masking[c].rawDb[static_cast<size_t>(b)] = channel.heardDb[static_cast<size_t>(b)];
    }
    for (size_t k = 0; k < ghosts.size(); ++k)
    {
        auto& m = masking[mix.channels.size() + k];
        m.valid = true;
        m.priority = ghosts[k].priority;
        for (int b = 0; b < ma::bus::kBands; ++b)
            m.rawDb[static_cast<size_t>(b)] = ghosts[k].db[static_cast<size_t>(b)];
    }
    for (const auto& issue : ma::analyseMasking(masking).issues)
        if (static_cast<size_t>(issue.masker) < mix.channels.size() && static_cast<size_t>(issue.masked) >= mix.channels.size())
            sketch.collisions.push_back({ static_cast<size_t>(issue.masker), static_cast<size_t>(issue.masked) - mix.channels.size(),
                                          issue.lowHz, issue.highHz, issue.score });
}

void BrainProcessor::addGhostGroups(std::array<ma::SpectrumAnalyzer::Bands, ma::kStemGroups>& groups,
                                    std::array<bool, ma::kStemGroups>& present, std::array<bool, ma::kStemGroups>& ghostOnly) const
{
    ghostOnly = {};
    if (!mix.sketch.active())
        return;
    for (const auto& ghost : mix.sketch.analysis.ghosts)
    {
        const auto g = static_cast<size_t>(ghost.group);
        for (size_t b = 0; b < groups[g].size(); ++b)
        {
            const double p = (groups[g][b] > -199.0 ? std::pow(10.0, groups[g][b] / 10.0) : 0.0)
                             + (ghost.db[b] > -199.0 ? std::pow(10.0, ghost.db[b] / 10.0) : 0.0);
            groups[g][b] = p > 1e-20 ? 10.0 * std::log10(p) : -200.0;
        }
        if (!present[g])
            ghostOnly[g] = present[g] = true;
    }
}

juce::String BrainProcessor::ghostName(const ma::SketchGhost& ghost)
{
    const int r = ghost.region;
    switch (static_cast<ma::StemGroup>(ghost.group))
    {
        case ma::StemGroup::drums:
            return r < 0 ? pl("perkusja") : r <= 1 ? pl("stopa") : r <= 3 ? pl("werbel albo clap") : pl("hi-haty, shakery");
        case ma::StemGroup::bass:
            return r < 0 ? pl("bas") : r == 0 ? pl("sub bas") : r == 1 ? pl("dół basu (alikwoty nad subem)") : pl("górna część basu");
        case ma::StemGroup::vocals: return pl("wokal");
        case ma::StemGroup::music: break;
    }
    return r < 0 ? pl("akordy, pad albo lead") : r <= 2 ? pl("pad albo akordy") : r == 3 ? pl("akordy albo stab")
                 : r == 4 ? pl("lead, arpeggio albo plucki") : pl("góra padu albo FX");
}

juce::String BrainProcessor::ghostBrief(const ma::SketchGhost& ghost) const
{
    const auto& group = mix.sketch.arrangement.groups[static_cast<size_t>(ghost.group)];
    const auto name = ghostName(ghost);
    auto text = name.substring(0, 1).toUpperCase() + name.substring(1);
    if (ghost.region >= 0)
    {
        text += pl(" w ") + hzText(ma::stemRegionEdgeHz(ghost.region)) + pl("–") + hzText(ma::stemRegionEdgeHz(ghost.region + 1)) + pl(": grupa ") + quoted(stemGroupName(ghost.group)) + pl(" ma tam ok. ")
                + juce::String(juce::roundToInt(ghost.shortDb)) + pl(" dB mniej niż ") + (mix.sketch.arrangement.fromReferences ? pl("referencje") : pl("typowy drop"))
                + pl(", a żaden jej kanał tam nie gra. EQ tego nie zrobi");
        // Phase 22: a channel of the group with its body just below (a sine bass under 90-250 Hz) can
        // fill it with its own overtones; that comes before a new part.
        bool saturated = false;
        const auto source = overtoneSource(ghost, saturated);
        if (source.isEmpty())
            text += pl(". Dograj partię");
        else if (saturated)
            text += pl(". ") + quoted(source) + pl(" ma już saturację, a to pasmo dalej jest puste: mocniejszy drive, „Harmoniczne” "
                                                    "albo druga warstwa basu (np. mid bas)");
        else
            text += pl(". Najpierw saturacja albo „Harmoniczne” na ") + quoted(source)
                    + pl(": gra tuż niżej, więc jego alikwoty trafią w to pasmo („Analizuj” (cały łańcuch) dobierze saturację sam, "
                         "gdy brzmienie jest za gładkie). Jeśli to nie wystarczy, dograj partię");
    }
    else
    {
        text += pl(": ok. ") + juce::String(juce::roundToInt(-ghost.levelDb)) + pl(" dB ciszej niż cały drop (ważone uchem), głównie ")
                + hzText(ghost.lowHz) + pl("–") + hzText(ghost.highHz);
        if (group.widthValid)
            text += group.width >= 0.15 ? pl(", szeroko (side ok. ") + juce::String(juce::roundToInt(group.width * 100.0)) + "%)"
                                        : pl(", raczej w środku (side ok. ") + juce::String(juce::roundToInt(group.width * 100.0)) + "%)";
        const auto key = trackKey();
        if (ma::stemGroupHasKey(ghost.group) && key.valid())
            text += pl(", w tonacji ") + keyText(key);
        juce::StringArray sections;
        for (int s = 0; s < ma::kStemSections; ++s)
            if (group.inSection[static_cast<size_t>(s)])
                sections.add(sectionText(static_cast<ma::StemSection>(s)).name);
        if (!sections.isEmpty())
            text += pl("; w referencjach gra też w sekcjach: ") + sections.joinIntoString(", ");
    }
    return text + ".";
}

// Phase 22: the channel of a region ghost's group whose loudest broad region sits one or two regions
// below the ghost (so its 2nd to 4th harmonics land in it), not one that is meant to be dirty.
juce::String BrainProcessor::overtoneSource(const ma::SketchGhost& ghost, bool& saturated) const
{
    saturated = false;
    // Only the bass: a missing snare or pad is another part, not the kick's or the lead's overtones.
    if (ghost.region <= 0 || ghost.group != static_cast<int>(ma::StemGroup::bass))
        return {};
    juce::String best;
    double bestDb = -INFINITY;
    for (const auto& channel : mix.channels)
    {
        if (!channel.spectrumValid || !inMixSums(channel) || groupOfChannel(channel) != ghost.group || channel.data.intentionalDrive != 0)
            continue;
        std::array<double, ma::kStemRegions> power {};
        for (int b = 0; b < ma::bus::kBands; ++b)
        {
            const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
            for (int r = 0; r < ma::kStemRegions; ++r)
                if (hz >= ma::stemRegionEdgeHz(r) && hz < ma::stemRegionEdgeHz(r + 1))
                    power[static_cast<size_t>(r)] += std::pow(10.0, (channel.heardDb[static_cast<size_t>(b)] + chainAddsDb(channel, b)) / 10.0);
        }
        const auto loudest = static_cast<int>(std::max_element(power.begin(), power.end()) - power.begin());
        const double db = 10.0 * std::log10(power[static_cast<size_t>(loudest)] + 1e-30);
        if (loudest < ghost.region && ghost.region - loudest <= 2 && db > bestDb)
        {
            bestDb = db;
            best = channel.name;
            saturated = channel.data.saturation.on > 0.5f;
        }
    }
    return best;
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
                           + pl(". W mono te pasma osłabną. „Analizuj” (cały łańcuch) zdejmie tam side na winnym kanale.");
        }
        report.push_back(perBand);

        // Each part folded to mono (club, phone, a mono speaker): how much more it loses than the mix.
        MixSnapshot::ReportItem folded;
        folded.title = pl("Ślady w mono");
        struct Fold
        {
            juce::String name;
            double mid = 0.0, side = 0.0;
        };
        std::vector<Fold> folds;
        double mixMid = 0.0, mixSide = 0.0;
        for (const auto& channel : mix.channels)
        {
            if (!channel.spectrumValid || !channel.stereoValid || !inMixSums(channel))
                continue;
            Fold fold { juce::String::fromUTF8(channel.data.name) };
            for (int b = 0; b < ma::bus::kBands; ++b)
            {
                const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
                if (hz < kMonoFoldLowHz || hz > kMonoFoldHighHz)
                    continue;
                // As it plays: after the Agent's side bells, widener and width slider.
                const double m = std::pow(10.0, channel.midDb[static_cast<size_t>(b)] / 10.0);
                fold.mid += m;
                fold.side += ma::processedSidePower(channel.data.stereo, channel.data.width, hz, m,
                                                    std::pow(10.0, channel.sideDb[static_cast<size_t>(b)] / 10.0));
            }
            mixMid += fold.mid;
            mixSide += fold.side;
            folds.push_back(fold);
        }
        const double mixTotal = mixMid + mixSide;
        if (folds.empty() || mixTotal <= 1e-20)
        {
            folded.detail = pl("Czekam na muzykę.");
        }
        else
        {
            const double mixLoss = 10.0 * std::log10(std::max(mixMid, 1e-20) / mixTotal);
            juce::StringArray worse, bad;
            for (const auto& fold : folds)
            {
                const double total = fold.mid + fold.side;
                if (total <= 1e-20 || 10.0 * std::log10(total / mixTotal) < kMonoFoldAudibleDb)
                    continue;
                const double extra = 10.0 * std::log10(std::max(fold.mid, 1e-20) / total) - mixLoss;
                if (extra > -kMonoFoldWarnDb)
                    continue;
                const auto line = quoted(fold.name) + " " + juce::String(extra, 1) + " dB";
                (extra <= -kMonoFoldProblemDb ? bad : worse).add(line);
            }
            folded.status = !bad.isEmpty() ? Status::problem : !worse.isEmpty() ? Status::warning : Status::good;
            if (bad.isEmpty() && worse.isEmpty())
                folded.detail = pl("Po zsumowaniu do mono żaden ślad nie cichnie wyraźnie bardziej niż cały miks (150 Hz–8 kHz).");
            else
            {
                juce::StringArray all;
                all.addArray(bad);
                all.addArray(worse);
                folded.detail = pl("W mono (klub, telefon, jeden głośnik) cichną bardziej niż reszta miksu: ") + all.joinIntoString(", ")
                                + pl(". Ich szerokość to głównie side, którego mono nie słyszy. Zawęź je (suwak „Szerokość” w Agencie poniżej 100%) "
                                     "albo zamień poszerzacz/chorus na krótki delay po jednej stronie.");
            }
        }
        report.push_back(folded);
    }

    // Agents connected.
    {
        MixSnapshot::ReportItem item;
        item.title = pl("Agenci");
        item.group = MixSnapshot::ReportGroup::setup;
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

    // Phase 23: what the Brain works from, and what each input is for.
    if (!mix.channels.empty())
    {
        MixSnapshot::ReportItem item;
        item.title = pl("Z czego korzysta Mózg");
        item.group = MixSnapshot::ReportGroup::setup;
        juce::StringArray noStats, noPeak, keyed;
        for (const auto& channel : mix.channels)
        {
            if (channel.data.statsValid == 0)
                noStats.add(channel.name);
            else if (channel.data.learnedTruePeakDb < -100.0f)
                noPeak.add(channel.name);
            if (channel.data.sidechainConnected != 0)
                keyed.add(channel.name);
        }
        const auto yes = pl("jest: "), no = pl("brak: ");
        juce::StringArray lines;
        lines.add((noStats.isEmpty() && noPeak.isEmpty() ? yes + pl("„Naucz poziom” na wszystkich kanałach")
                   : no + pl("„Naucz poziom” na ") + joinNames(noStats.isEmpty() ? noPeak : noStats)
                       + (noStats.isEmpty() ? pl(" (projekt sprzed fazy 23, brak szczytu)") : juce::String()))
                  + pl(" → kompresja, clipper, wyrównanie basu, rola, strój stopy"));
        lines.add((currentTarget.valid() ? yes + pl("cel tonalny ") + quoted(juce::String::fromUTF8(currentTarget.name.c_str()))
                                         : no + pl("własny cel tonalny (do tego czasu wbudowany „Typowy house”)"))
                  + pl(" → balans tonalny, korekcje barwy"));
        lines.add((currentStems.valid() ? yes + pl("referencje ze stemów ") + quoted(juce::String::fromUTF8(currentStems.name.c_str()))
                                        : no + pl("referencje ze stemów (proporcje grup i szkic: typowy house)"))
                  + pl(" → proporcje grup, intro, build, break, przebieg"));
        lines.add((keyed.isEmpty() ? no + pl("sidechain stopy") : yes + pl("sidechain stopy na ") + joinNames(keyed))
                  + pl(" → ustępowanie na każde uderzenie, faza stopa-bas"));
        const bool songKnown = mix.energy.comparison.valid
                               && std::count_if(mix.energy.ours.levelDb.begin(), mix.energy.ours.levelDb.end(),
                                                [](double l) { return std::isfinite(l) && l < -6.0; })
                                      >= 4;
        lines.add((songKnown ? yes + pl("przebieg utworu (pozycja z FL)") : no + pl("przebieg utworu (puść całość w trybie Song)"))
                  + pl(" → sekcje, automatyka, mapa energii"));
        lines.add((mix.dropSeconds >= kMinimumDropSeconds ? yes : no) + pl("pomiar dropu (") + juce::String(juce::roundToInt(mix.dropSeconds))
                  + pl(" s) → kolizje, szkic, propozycja"));
        item.detail = lines.joinIntoString("\n");
        item.status = noStats.isEmpty() && noPeak.isEmpty() ? Status::good : Status::warning;
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
            // A limited trim is only worth a warning while nothing in the chain takes the peaks.
            if (channel.data.trimLimited != 0 && channel.data.compressor.on < 0.5f && channel.data.clipper.on < 0.5f)
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
                            << joinNames(limited) << pl(". Kompresor albo clipper ustawi im „Analizuj” (cały łańcuch), potem „Zastosuj”.");
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
            roles.group = MixSnapshot::ReportGroup::setup;
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
        item.group = MixSnapshot::ReportGroup::setup;
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
        if (!activeTarget().valid())
        {
            item.detail = currentTarget.name.empty()
                              ? pl("Raz na zawsze: w widoku „Balans tonalny” kliknij „Nowy cel” i nagraj 2-3 utwory referencyjne puszczone "
                                   "przez master. Cel zostaje na dysku, a każdy nowy projekt dostaje go sam.")
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
        else
        {
            // Phase 17: in a sketch the mix is judged with its ghosts in; what they explain is the arrangement's.
            const bool sketch = mix.sketch.active() && mix.sketch.tonal.valid;
            const auto& regions = sketch ? mix.sketch.tonal.regions : mix.tonal.regions;
            auto rangeText = [](const ma::TonalRegion& region) {
                return (region.hole() ? pl("brakuje ") : pl("za dużo ")) + hzText(region.lowHz) + pl("–") + hzText(region.highHz);
            };
            juce::StringArray arrangement;
            for (size_t i = 0; sketch && i < mix.tonal.regions.size() && i < mix.sketch.arrangementRegion.size(); ++i)
                if (mix.sketch.arrangementRegion[i] && arrangement.size() < 3)
                    arrangement.add(rangeText(mix.tonal.regions[i]));
            const auto arrangementText = arrangement.isEmpty() ? juce::String()
                                                               : pl(" W samym szkicu (bez duchów): ") + arrangement.joinIntoString(", ")
                                                                     + pl(". To aranż, nie EQ (widok „Aranż”).");
            // Phase 24: where each of TBC's ranges sits against the corridor's centre, and on what curve.
            const auto& judged = sketch ? mix.sketch.tonal : mix.tonal;
            const auto zones = ma::zoneOffsets(judged, activeTarget());
            juce::StringArray offsets;
            bool offCentre = false;
            for (int z = 0; z < ma::kTonalZones; ++z)
            {
                const auto& zone = zones[static_cast<size_t>(z)];
                if (!zone.valid)
                    continue;
                offsets.add(zoneName(z) + " " + signedOneDecimal(zone.offsetDb));
                offCentre = offCentre || std::abs(zone.offsetDb) > 0.75;
            }
            const auto curveText = afterLimiterOn && limiterTiltKnown()
                                       ? pl(" Krzywa jak po limiterze do ") + juce::String(masterTargetLufs(), 1)
                                             + pl(" LUFS (szacunek z podglądu mastera, referencje są masterami).")
                                       : pl(" Krzywa miksu tak, jak gra (bez limitera).");
            const auto zonesText = offsets.isEmpty() ? juce::String()
                                                     : pl(" Względem środka korytarza: ") + offsets.joinIntoString(", ") + " dB."
                                                           + (offCentre ? pl(" „Analizuj” dociągnie zakresy dalej niż 0,75 dB od środka.")
                                                                        : juce::String());
            if (regions.empty())
            {
                item.status = Status::good;
                item.detail = (sketch ? pl("Z partiami-duchami miks mieści się w korytarzu celu ") : pl("Cały miks mieści się w korytarzu celu "))
                              + quoted(juce::String::fromUTF8(activeTarget().name.c_str())) + "." + zonesText + curveText + arrangementText + builtinNote();
            }
            else
            {
                // Phase 22: only what EQ can fix counts as a mix problem. An excess can always be cut;
                // a hole only where some channel carries content to lift. A hole nothing plays in is
                // the arrangement's or the sound's (a sine bass has nothing at 90-250 Hz to boost).
                double worst = 0.0;
                juce::StringArray parts, unfixable;
                for (const auto& region : regions)
                {
                    if (region.hole() && !carriedByAChannel(mix.channels, region))
                    {
                        // Phase 39: who could fill it: the part playing an octave under it, through overtones.
                        if (unfixable.size() < 3)
                        {
                            juce::String source;
                            double best = 0.0;
                            for (const auto& channel : mix.channels)
                            {
                                if (!channel.spectrumValid || !inMixSums(channel))
                                    continue;
                                double below = 0.0;
                                for (int b = 0; b < ma::bus::kBands; ++b)
                                {
                                    const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
                                    if (hz >= region.lowHz / 2.0 && hz <= region.highHz / 2.0)
                                        below += std::pow(10.0, channel.heardDb[static_cast<size_t>(b)] / 10.0);
                                }
                                if (below > best)
                                {
                                    best = below;
                                    source = channel.name;
                                }
                            }
                            unfixable.add(rangeText(region)
                                          + (source.isEmpty() ? juce::String()
                                                              : pl(" (wypełni je saturacja albo harmoniczne na ") + quoted(source)
                                                                    + pl(", gra oktawę niżej; zakładka „Barwa” w jego Agencie)")));
                        }
                        continue;
                    }
                    worst = std::max(worst, std::abs(region.deviationDb));
                    if (parts.size() < 3)
                        parts.add(rangeText(region));
                }
                const auto prefix = sketch ? pl("Z partiami-duchami: ") : juce::String();
                const auto notMix = unfixable.isEmpty()
                                        ? juce::String()
                                        : (parts.isEmpty() ? juce::String() : pl(" ")) + pl("Poza zasięgiem EQ: ")
                                              + unfixable.joinIntoString(", ")
                                              + pl(". Tam żaden kanał nie gra, EQ nie ma czego podbić; jeśli nic nie gra też oktawę niżej, to nowa partia (widok „Aranż”).");
                if (parts.isEmpty())
                {
                    item.status = Status::warning;
                    item.detail = prefix + pl("EQ nie ma tu nic do zrobienia.") + notMix + arrangementText;
                }
                else
                {
                    item.status = worst >= 3.0 ? Status::problem : Status::warning;
                    item.detail = prefix + parts.joinIntoString(", ") + pl(". „Analizuj” podpowie, co zrobić.") + notMix + curveText + arrangementText;
                    const auto bells = ozoneCurve(judged);
                    if (!bells.isEmpty())
                        item.detail += pl(" Czego kanały nie uniosą, zostaje na EQ w Ozone: ") + bells.joinIntoString("; ") + ".";
                }
            }
        }
        report.push_back(item);
    }

    // Phase 17: is the drop's arrangement complete, and what to add if not.
    if (sketchOn)
    {
        MixSnapshot::ReportItem item;
        item.title = pl("Aranż (tryb szkicu)");
        item.group = MixSnapshot::ReportGroup::arrangement;
        const auto source = mix.sketch.arrangement.fromReferences
                                ? pl("referencji ") + quoted(juce::String::fromUTF8(currentStems.name.c_str()))
                                : pl("typowego dropu house'u (bez referencji ze stemów)");
        if (useProfiles)
            item.detail = pl("Tryb szkicu ocenia to, co gra w dropie, a nie nauczone profile.");
        else if (mix.dropSeconds < kMinimumDropSeconds)
            item.detail = pl("Puść najgłośniejszą część (drop): sprawdzę, czy ma wszystkie partie. ")
                          + juce::String(juce::roundToInt(mix.dropSeconds)) + " / " + juce::String(juce::roundToInt(kMinimumDropSeconds)) + " s.";
        else if (!mix.sketch.active())
        {
            item.status = Status::good;
            item.detail = pl("Drop ma wszystkie partie, które ma drop ") + source + ".";
        }
        else
        {
            item.status = Status::warning;
            juce::StringArray names, briefs;
            for (const auto& ghost : mix.sketch.analysis.ghosts)
            {
                names.addIfNotAlreadyThere(ghostName(ghost));
                briefs.add(pl("• ") + ghostBrief(ghost));
            }
            item.detail = pl("Szkic, brakuje: ") + names.joinIntoString(", ") + pl(". Mózg wstawia za nie partie-duchy z ") + source
                          + pl(": balans tonalny, proporcje grup i kolizje liczy z nimi, a „Analizuj” (cały łańcuch) zostawia im miejsce i nie łata "
                               "EQ-em dziur, które wypełni aranż.\n")
                          + briefs.joinIntoString("\n");
            juce::StringArray covers;
            for (const auto& collision : mix.sketch.collisions)
                if (covers.size() < 3 && collision.ghost < mix.sketch.analysis.ghosts.size())
                    covers.add(quoted(mix.channels[collision.masker].name) + pl(" zasłoni: ") + ghostName(mix.sketch.analysis.ghosts[collision.ghost])
                               + pl(" w ") + hzText(collision.lowHz) + pl("–") + hzText(collision.highHz) + " ("
                               + juce::String(juce::roundToInt(collision.score * 100.0)) + "%)");
            if (!covers.isEmpty())
                item.detail += pl("\nJuż teraz: ") + covers.joinIntoString(", ") + pl(". „Analizuj” (cały łańcuch) zrobi na to miejsce (do 3 dB).");
        }
        juce::StringArray added;
        if (!plannedOwners.empty())
            for (const auto& channel : mix.channels)
                if (channel.spectrumValid && inMixSums(channel)
                    && std::find(plannedOwners.begin(), plannedOwners.end(), channel.owner) == plannedOwners.end())
                    added.add(quoted(channel.name));
        if (!added.isEmpty())
        {
            item.status = Status::warning;
            item.detail += (item.detail.isEmpty() ? juce::String() : pl("\n")) + pl("Doszły kanały: ") + joinNames(added)
                           + pl(". Kliknij „Analizuj” (cały łańcuch), żeby plan objął je razem z resztą.");
        }
        report.push_back(item);
    }

    // Kick and bass in time: dynamic give-way and polarity, measured by Agents with a sidechain.
    {
        juce::StringArray noSignal, cancelling, ducking, keyed;
        bool kick = false, bass = false, anySidechain = false;
        for (const auto& channel : mix.channels)
        {
            kick = kick || channel.data.role == 1;
            bass = bass || channel.data.role == 2;
            const bool connected = channel.data.sidechainConnected != 0;
            anySidechain = anySidechain || connected;
            // Phase 30: without a cable the duck listens to the kick's Agent through the shared memory.
            if (channel.data.duck.on > 0.5f && !connected && channel.data.duckKeyFound != 0)
                keyed.add(channel.name + pl(" do ") + juce::String(channel.data.duckReductionDb, 1) + " dB");
            else if (channel.data.duck.on > 0.5f && !connected)
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
                          + pl(". „Analizuj” odwróci polaryzację, porównaj „Przed / po”.");
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
        else if (!keyed.isEmpty())
        {
            item.status = Status::good;
            item.detail = pl("Ustępuje stopie bez kabla (Agent stopy podaje obwiednię przez wspólną pamięć): ") + joinNames(keyed)
                          + pl(". Fazę stopa-bas sprawdzi tylko sidechain: podłącz stopę do Agenta basu, jeśli chcesz ją znać.");
        }
        else if (kick && bass)
        {
            item.detail = pl("Podłącz stopę jako sidechain do Agenta basu, żeby sprawdzić fazę i ustępować dynamicznie.");
        }
        if (!keyed.isEmpty() && anySidechain && item.status == Status::good)
            item.detail += pl(" Bez kabla: ") + joinNames(keyed) + ".";
        if (item.detail.isNotEmpty())
            report.push_back(item);
    }

    // Phase 38: nothing lost anywhere in the song. From the song's seconds, as the mix hears them now
    // (each second's raw + trim power plus the channel's present EQ and cleanup), worked out every 2 s.
    {
        const auto nowMs = juce::Time::getMillisecondCounter();
        if (lostAtMs == 0 || nowMs - lostAtMs >= kLostRefreshMs)
        {
            lostAtMs = nowMs;
            lostSpans.clear();
            lostNames.clear();
            int last = -1;
            for (const auto& [second, bin] : songBins)
                if (bin.updates > 0)
                    last = std::max(last, second);
            std::vector<ma::AudibilityChannel> in;
            for (const auto& channel : mix.channels)
            {
                if (!inMixSums(channel) || last < 0)
                    continue;
                ma::AudibilityChannel a;
                a.valid = true;
                a.background = channel.data.role == static_cast<int>(ma::Role::send);
                a.secondsDb.resize(static_cast<size_t>(last + 1));
                for (auto& sec : a.secondsDb)
                    sec.fill(-200.0);
                for (const auto& [second, bin] : songBins)
                {
                    const auto found = bin.sums.find(channel.owner);
                    if (bin.updates <= 0 || found == bin.sums.end() || second < 0)
                        continue;
                    auto& sec = a.secondsDb[static_cast<size_t>(second)];
                    for (int b = 0; b < ma::bus::kBands; ++b)
                    {
                        const auto at = static_cast<size_t>(b);
                        const double p = found->second.power[at] / static_cast<double>(bin.updates);
                        const double eq = channel.spectrumValid && channel.heardDb[at] > -150.0 && channel.rawDb[at] > -150.0
                                              ? channel.heardDb[at] - channel.rawDb[at]
                                              : 0.0;
                        sec[at] = p > 1e-20 ? 10.0 * std::log10(p) + eq : -200.0;
                    }
                }
                in.push_back(std::move(a));
                lostNames.push_back(channel.name);
            }
            lostSpans = ma::findLostSpans(in);
        }
        MixSnapshot::ReportItem item;
        item.title = pl("Słyszalność w czasie");
        const auto clock = [](int second) {
            return juce::String(second / 60) + ":" + juce::String(second % 60).paddedLeft('0', 2);
        };
        if (lostNames.empty())
        {
            item.detail = pl("Puść cały utwór od początku: sprawdzę, czy w którymś fragmencie jakiś ślad nie ginie.");
        }
        else if (lostSpans.empty())
        {
            item.status = Status::good;
            item.detail = pl("W odsłuchanych fragmentach żaden ślad nie ginie na dłużej niż 3 s (każdy w swoim głównym paśmie).");
        }
        else
        {
            item.status = Status::warning;
            auto spans = lostSpans;
            std::stable_sort(spans.begin(), spans.end(), [](const auto& a, const auto& b) {
                return a.spanShareDb - a.typicalShareDb < b.spanShareDb - b.typicalShareDb;
            });
            juce::StringArray parts;
            for (size_t k = 0; k < spans.size() && k < kMaxLostListed; ++k)
            {
                const auto& span = spans[k];
                auto part = quoted(lostNames[static_cast<size_t>(span.channel)]) + " " + clock(span.fromSecond) + pl("–")
                            + clock(span.toSecond + 1) + " (" + hzText(span.lowHz) + pl("–") + hzText(span.highHz) + ", "
                            + juce::String(span.typicalShareDb - span.spanShareDb, 1) + pl(" dB słabiej niż zwykle");
                if (span.coveredBy >= 0)
                    part += pl(", zasłania go ") + quoted(lostNames[static_cast<size_t>(span.coveredBy)]);
                parts.add(part + ")");
            }
            if (spans.size() > kMaxLostListed)
                parts.add(pl("i ") + juce::String(static_cast<int>(spans.size() - kMaxLostListed)) + pl(" innych"));
            item.detail = pl("Ginie na chwilę: ") + parts.joinIntoString("; ")
                          + pl(". Mózg ustawia balans na dropie, te fragmenty popraw automatyką głośności w FL: podnieś ten ślad "
                               "albo ścisz zasłaniający na czas fragmentu (przed Agentem, w głośności kanału).");
        }
        report.push_back(item);
    }

    // Phase 34: depth. How loud the sends (reverb, delay) are against the dry parts, 200 Hz-8 kHz.
    {
        double sends = 0.0, dry = 0.0;
        juce::StringArray names;
        for (const auto& channel : mix.channels)
        {
            if (!channel.spectrumValid || !inMixSums(channel))
                continue;
            const bool send = channel.data.role == static_cast<int>(ma::Role::send);
            if (send)
                names.add(quoted(channel.name));
            for (int b = 0; b < ma::bus::kBands; ++b)
            {
                const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
                if (hz >= 200.0 && hz <= 8000.0)
                    (send ? sends : dry) += std::pow(10.0, channel.heardDb[static_cast<size_t>(b)] / 10.0);
            }
        }
        MixSnapshot::ReportItem item;
        item.title = pl("Głębia (pogłos, delay)");
        item.group = MixSnapshot::ReportGroup::arrangement;
        const auto times = reverbAdviceText(tempoBpm());
        if (dry <= 0.0)
            ;
        else if (names.isEmpty())
            item.detail = pl("Żaden kanał nie ma roli „Wysyłka”. Jeśli pogłos i delay grają na osobnych kanałach miksera, ustaw na nich Agenta "
                             "z tą rolą: Mózg przyciemni ich górę i każe im ustępować głównemu głosowi.");
        else
        {
            const double share = 10.0 * std::log10(std::max(sends, 1e-30) / dry);
            item.status = share > -6.0 ? Status::warning : Status::good;
            item.detail = pl("Wysyłki ") + joinNames(names) + pl(" grają ") + juce::String(share, 1) + pl(" dB względem suchych śladów (200 Hz–8 kHz)")
                          + (share > -6.0 ? pl(": głośno, ogony rozmywają miks; ścisz wysyłki albo skróć pogłos.")
                                          : pl("."));
        }
        // Phase 37: what feeds each send and what it gives way to (from the last whole-mix analysis).
        juce::StringArray feeds;
        for (const auto& channel : mix.channels)
        {
            const auto note = lastSendNotes.find(channel.owner);
            if (note == lastSendNotes.end() || channel.data.role != static_cast<int>(ma::Role::send))
                continue;
            auto line = quoted(channel.name) + ": " + sendSourcesText(note->second.sources);
            if (const auto* key = findChannel(note->second.keyOwner))
                line += pl(", ustępuje ") + quoted(key->name);
            feeds.add(line);
        }
        if (item.detail.isNotEmpty() && !feeds.isEmpty())
            item.detail += " " + feeds.joinIntoString("; ") + ".";
        if (item.detail.isNotEmpty() && times.isNotEmpty())
            item.detail += " " + times + ". " + delayAdviceText(tempoBpm()) + ".";
        if (item.detail.isNotEmpty())
            report.push_back(item);
    }

    // Phase 35: the bass's low end against the kick's, in time.
    {
        juce::StringArray early, late, cancel, noCable, inTime, apart;
        for (const auto& channel : mix.channels)
        {
            const auto advice = alignAdvice(channel.data);
            using K = AlignAdvice::Kind;
            const auto name = quoted(channel.name);
            switch (advice.kind)
            {
                case K::notBass: break;
                case K::noCable: noCable.add(name); break;
                case K::notMeasured: break;
                case K::inTime: inTime.add(name); break;
                case K::unclear: apart.add(name); break;
                case K::polarity: cancel.add(name); break;
                case K::early:
                    if (std::abs(channel.data.align.delayMs - advice.delayMs) >= kAlignMinLagMs)
                        early.add(name + " (" + juce::String(-advice.lagMs, 1) + pl(" ms przed stopą)"));
                    else
                        inTime.add(name);
                    break;
                case K::late: late.add(name + " (" + juce::String(advice.lagMs, 1) + pl(" ms za stopą)")); break;
            }
        }
        MixSnapshot::ReportItem item;
        item.title = pl("Stopa i bas: wyrównanie w czasie");
        if (!late.isEmpty() || !cancel.isEmpty())
        {
            item.status = Status::warning;
            juce::StringArray parts;
            if (!late.isEmpty())
                parts.add(pl("dół spóźniony: ") + joinNames(late) + pl(", przesuń bas o tyle wcześniej w FL"));
            if (!cancel.isEmpty())
                parts.add(pl("znosi się ze stopą: ") + joinNames(cancel) + pl(", odwróć polaryzację w Agencie basu"));
            item.detail = parts.joinIntoString("; ") + ".";
        }
        else if (!early.isEmpty())
        {
            item.status = Status::warning;
            item.detail = pl("Dół basu przed stopą: ") + joinNames(early) + pl(". „Analizuj” (cały łańcuch) opóźni bas.");
        }
        else if (!inTime.isEmpty() || !apart.isEmpty())
        {
            item.status = Status::good;
            juce::StringArray parts;
            if (!inTime.isEmpty())
                parts.add(pl("dół basu idzie razem ze stopą: ") + joinNames(inTime));
            if (!apart.isEmpty())
                parts.add(pl("nie grają razem w dole (bas ustępuje stopie albo gra między uderzeniami), nie ma czego wyrównywać: ")
                          + joinNames(apart));
            item.detail = parts.joinIntoString("; ") + ".";
            item.detail = item.detail.substring(0, 1).toUpperCase() + item.detail.substring(1);
        }
        else if (!noCable.isEmpty())
            item.detail = pl("Żeby to zmierzyć, wyślij stopę jako sidechain do kanału basu: ") + joinNames(noCable) + ".";
        if (item.detail.isNotEmpty())
            report.push_back(item);
    }

    // Phase 14: the kick's tuning against the key and the bass.
    if (const auto* kick = kickChannel())
    {
        MixSnapshot::ReportItem item;
        item.title = pl("Stopa a tonacja");
        item.group = MixSnapshot::ReportGroup::arrangement;
        const auto tuning = kickTuning();
        if (kick->data.statsValid == 0)
            item.detail = pl("Kliknij „Naucz poziom” na stopie: nauka mierzy, na jakiej nucie brzmi jej ogon.");
        else if (kick->data.tailPitchHz <= 0.0f)
        {
            item.status = Status::good;
            item.detail = pl("Ogon stopy nie ma wyraźnej nuty, strojenie nie gra roli.");
        }
        else if (!tuning.valid)
            item.detail = pl("Stopa brzmi na ") + kickNoteText(kick->data.tailPitchHz)
                          + pl(". Tonacja jeszcze nieznana: Mózg ją rozpozna po ok. 20 s muzyki albo ustaw ją ręcznie w widoku „Referencje”.");
        else
        {
            item.status = tuning.needsTuning ? (tuning.fit == ma::KickTuning::Fit::outOfScale || tuning.clashesWithBass ? Status::problem
                                                                                                                   : Status::warning)
                                             : Status::good;
            item.detail = kickTuningText(tuning);
        }
        report.push_back(item);
    }

    // Translation to small speakers, cars and clubs.
    {
        MixSnapshot::ReportItem item;
        item.title = pl("Translacja na inne systemy");
        const int preview = previewSystemIndex();
        if (listenGhosts())
        {
            item.status = Status::warning;
            item.detail = pl("W odsłuchu grają partie-duchy (szum w kształcie brakujących partii, tylko w dropie). Wyłącz przed eksportem (zakładka „Aranż”).");
        }
        else if (preview != 0)
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
                              + pl(". „Analizuj” dobierze kroki. Posłuchaj w „Odsłuch: Telefon”.");
            }
            else if (!weak.isEmpty())
            {
                item.status = Status::warning;
                item.detail = pl("Na małych głośnikach słabnie: ") + joinNames(weak) + pl(". „Analizuj” dobierze kroki.");
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
                item.detail = parts.joinIntoString(", ") + pl(". „Analizuj” (cały łańcuch) dopasuje poziomy grup i barwę szerokimi korekcjami.");
            }
        }
        report.push_back(item);
    }

    // Phase 12 and 16: the intro, the build and the break against the references' own. Advice
    // only: levels inside a section are the arrangement's and the volume automation's, not the EQ's.
    for (const auto section : { ma::StemSection::intro, ma::StemSection::build, ma::StemSection::breakdown })
    {
        const auto& target = keyedSectionStems[static_cast<size_t>(section)];
        if (!target.valid())
            continue;
        const auto& ours = mix.section(section);
        const auto text = sectionText(section);
        MixSnapshot::ReportItem item;
        item.title = text.title;
        item.group = MixSnapshot::ReportGroup::arrangement;
        const bool anyDrums = std::any_of(mix.channels.begin(), mix.channels.end(),
                                          [](const auto& c) { return groupOfChannel(c) == static_cast<int>(ma::StemGroup::drums); });
        if (section == ma::StemSection::breakdown && !anyDrums)
            item.detail = pl("Żaden kanał nie ma roli stopy ani perkusji, więc nie odróżnię breaku od dropu.");
        else if (!ours.stems.valid)
            item.detail = text.waiting + " " + juce::String(juce::roundToInt(ours.seconds)) + " / "
                          + juce::String(juce::roundToInt(section == ma::StemSection::build ? kMinimumBuildSeconds : kMinimumDropSeconds)) + " s.";
        else
        {
            juce::StringArray parts;
            for (int g = 0; g < ma::kStemGroups; ++g)
            {
                const auto& c = ours.stems.groups[static_cast<size_t>(g)];
                if (c.compared && std::abs(c.levelDeviationDb) >= 1.0)
                    parts.add(stemGroupName(g) + pl(" o ") + juce::String(std::abs(c.levelDeviationDb), 1)
                              + (c.levelDeviationDb > 0.0 ? pl(" dB za głośno") : pl(" dB za cicho")));
                else if (c.extra)
                    parts.add(stemGroupName(g) + pl(" gra, ") + text.inReferences + pl(" milknie"));
                else if (c.missing && !(mix.sketch.active() && mix.sketch.analysis.missing[static_cast<size_t>(g)]))
                    parts.add(stemGroupName(g) + pl(" milknie, ") + text.inReferences + pl(" gra")); // a sketch's missing part is in "Aranż"
            }
            const auto& contrast = target.contrast(section);
            const double below = ours.belowDropDb;
            if (contrast.valid && std::isfinite(below) && std::abs(contrast.outside(below)) >= 1.0)
                parts.add(text.name + pl(" jest ") + juce::String(below, 1) + pl(" dB ciszej niż drop, w referencjach ")
                          + juce::String(contrast.low, 1) + pl("–") + juce::String(contrast.high, 1) + " dB: "
                          + (contrast.outside(below) < 0.0 ? text.tooLoud : text.tooQuiet));
            item.status = parts.isEmpty() ? Status::good : Status::warning;
            item.detail = parts.isEmpty() ? text.fine
                                          : parts.joinIntoString(", ") + pl(". To sprawa aranżu i automatyki głośności, nie EQ.");
            // Phase 19: the automation itself.
            const auto automation = automationText(section);
            if (automation.isNotEmpty())
                item.detail += " " + automation;
        }
        report.push_back(item);
    }

    // Phase 20: the song's energy over time against the references'.
    report.push_back(energyItem());

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
            int open = 0, ducked = 0, unmasked = 0, atLimit = 0, pending = 0, levelStep = 0, settled = 0;
            bool strong = false;
            for (const auto& issue : mix.masking.issues)
                switch (issueHandling(issue))
                {
                    case IssueHandling::open:
                        ++open;
                        strong = strong || issue.score >= 0.5;
                        break;
                    case IssueHandling::ducked: ++ducked; break;
                    case IssueHandling::unmasked: ++unmasked; break;
                    case IssueHandling::cutAtLimit: ++atLimit; break;
                    case IssueHandling::pending: ++pending; break;
                    case IssueHandling::levelStep: ++levelStep; break;
                    case IssueHandling::settled: ++settled; break;
                }
            auto collisions = [](int count) {
                return juce::String(count) + (count == 1 ? pl(" kolizja") : count < 5 ? pl(" kolizje") : pl(" kolizji"));
            };
            juce::StringArray parts;
            if (open > 0)
                parts.add(collisions(open) + pl(" do sprawdzenia („Analizuj”)"));
            if (pending > 0)
                parts.add(collisions(pending) + pl(" ma zmianę w propozycji, czeka na „Zastosuj”"));
            if (atLimit > 0)
                parts.add(collisions(atLimit) + pl(" ma cięcie na limicie, a zniknie po ściszeniu kanału („Analizuj” doda to do propozycji)"));
            if (levelStep > 0)
                parts.add(collisions(levelStep) + pl(" zniknie po ściszeniu kanału o więcej niż 2 dB (to zmienia balans, decyzja Twoja)"));
            if (ducked > 0)
                parts.add(collisions(ducked) + pl(" obsługuje ustępowanie na stopę"));
            if (unmasked > 0)
                parts.add(collisions(unmasked) + pl(" obsługuje ustępowanie bez kabla (zasłaniający kanał przycisza się, gdy tamten gra)"));
            if (settled > 0)
                parts.add(collisions(settled) + pl(" zostaje, bo miks zrobił, ile mógł: to rejestr albo brzmienie (aranż)"));
            // Phase 23: amber and red only for what is still to do in the mix.
            item.status = open > 0 ? (strong ? Status::problem : Status::warning)
                          : pending + atLimit > 0 ? Status::warning
                                                               : Status::good;
            item.detail = parts.joinIntoString(", ") + pl(". Szczegóły w liście poniżej mapy.");
        }
        report.push_back(item);
    }

    // Phase 35: one go / no-go line before mastering. Red in the mix or the setup blocks the export,
    // amber is worth a look, the arrangement is the song's and does not stop the mix going to Ozone.
    {
        MixSnapshot::ReportItem gate;
        gate.gate = true;
        gate.group = MixSnapshot::ReportGroup::setup;
        gate.title = pl("Gotowe dla Ozone?");
        juce::StringArray blockers, checks, waiting;
        if (listenGhosts())
            blockers.add(pl("wyłącz partie-duchy w odsłuchu (zakładka „Aranż”)"));
        if (previewSystemIndex() != 0)
            blockers.add(pl("przełącz odsłuch na „Pełne pasmo”"));
        for (const auto& item : report)
        {
            if (item.group == MixSnapshot::ReportGroup::arrangement)
                continue;
            if (item.status == Status::problem)
                blockers.add(item.title);
            else if (item.status == Status::warning)
                checks.add(item.title);
            else if (item.status == Status::unknown && item.group == MixSnapshot::ReportGroup::mix)
                waiting.add(item.title);
        }
        if (!std::isfinite(truePeakDb()))
        {
            gate.detail = pl("Puść utwór od początku do końca: sprawdzę szczyt, drop i resztę raportu.");
        }
        else if (!blockers.isEmpty())
        {
            gate.status = Status::problem;
            gate.detail = pl("Jeszcze nie. Najpierw: ") + blockers.joinIntoString(", ") + "."
                          + (checks.isEmpty() ? juce::String() : pl(" Do sprawdzenia: ") + checks.joinIntoString(", ") + ".");
        }
        else if (!checks.isEmpty())
        {
            gate.status = Status::warning;
            gate.detail = pl("Prawie. Nic nie blokuje eksportu, ale sprawdź: ") + checks.joinIntoString(", ") + "."
                          + (waiting.isEmpty() ? juce::String() : pl(" Jeszcze bez pomiaru: ") + waiting.joinIntoString(", ") + ".");
        }
        else if (mix.dropSeconds < kMinimumDropSeconds)
        {
            gate.detail = pl("Puść drop do końca: bez niego nie ocenię proporcji i barwy.");
        }
        else
        {
            gate.status = Status::good;
            gate.detail = pl("Tak. Wyeksportuj miks (24 lub 32 bit, bez limitera na sumie) i zabierz „Raport dla Ozone”.")
                          + (waiting.isEmpty() ? juce::String() : pl(" Bez pomiaru zostało: ") + waiting.joinIntoString(", ") + ".");
        }
        report.insert(report.begin(), gate);
    }
}

namespace
{
bool collisionStillThere(const ma::MaskingResult& result, int masker, int masked)
{
    return std::any_of(result.issues.begin(), result.issues.end(), [&](const auto& i) { return i.masker == masker && i.masked == masked; });
}

// How much quieter the masker has to be for the collision to go, in 0.5 dB steps; 0 when even
// `limitDb` is not enough.
double levelToClear(const std::vector<ma::MaskingChannel>& masking, int masker, int masked, double limitDb)
{
    for (double x = 0.5; x <= limitDb + 1e-9; x += 0.5)
    {
        auto trial = masking;
        for (auto& db : trial[static_cast<size_t>(masker)].rawDb)
            db -= x;
        if (!collisionStillThere(ma::analyseMasking(trial), masker, masked))
            return x;
    }
    return 0.0;
}

// Phase 30: the masker's level with a dip of `depthDb` at `settings` (the bell at its full depth).
std::vector<ma::MaskingChannel> withDip(const std::vector<ma::MaskingChannel>& masking, int masker, const ma::UnmaskSettings& settings, double depthDb)
{
    auto trial = masking;
    ma::EqSettings bell {};
    bell[0] = { settings.hz, static_cast<float>(-depthDb), settings.q };
    for (int b = 0; b < ma::bus::kBands; ++b)
        trial[static_cast<size_t>(masker)].rawDb[static_cast<size_t>(b)]
            += ma::eqResponseDb(bell, ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b));
    return trial;
}

// The dip's bell reaches about an octave / Q to each side of its centre.
bool dipCovers(const ma::UnmaskSettings& dip, double lowHz, double highHz)
{
    const double reach = std::pow(2.0, 1.0 / std::max(0.3, static_cast<double>(dip.q)));
    return dip.hz / reach < highHz && dip.hz * reach > lowHz;
}

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
    if (duckedCollision(issue.masker, issue.masked, issue.lowHz, issue.highHz))
        return IssueHandling::ducked;
    if (unmaskedCollision(issue.masker, issue.masked, issue.lowHz, issue.highHz))
        return IssueHandling::unmasked;
    // Phase 23: the last analysis says whether something is still to do.
    const auto maskerOwner = mix.channels[static_cast<size_t>(issue.masker)].owner;
    const auto maskedOwner = mix.channels[static_cast<size_t>(issue.masked)].owner;
    if (currentProposal.made && std::any_of(currentProposal.items.begin(), currentProposal.items.end(), [&](const auto& item) {
            return isPendingFor(item, maskerOwner, maskedOwner, issue.lowHz, issue.highHz);
        }))
        return IssueHandling::pending;
    const auto step = std::find_if(collisionSteps.begin(), collisionSteps.end(),
                                   [&](const auto& c) { return c.masker == maskerOwner && c.masked == maskedOwner; });
    if (step != collisionSteps.end() && !step->clearedByProposal && step->unmask)
        return IssueHandling::settled; // the dip was left unticked (or did not go in): the user's call
    if (step != collisionSteps.end() && !step->clearedByProposal)
    {
        // Phase 28: after "Zastosuj" a level step that went in and did not quite clear it is the
        // mix having done what it can; one left unticked is still the user's call.
        if (step->levelDb > 0.0 && !step->applied)
            return IssueHandling::levelStep;
        return IssueHandling::settled;
    }
    bool atLimit = false;
    if (masker.eqBypass == 0)
    {
        const double limit = ma::SolverOptions {}.maxCutDb - 0.5;
        for (const auto& band : masker.eq)
            atLimit = atLimit || (band.gainDb <= -limit && band.hz >= issue.lowHz && band.hz <= issue.highHz);
    }
    // Phase 28: the analysis expected the proposal to clear it, or the collision is new, and the
    // cut is as deep as it goes: whether a small level step would do is measured here, so amber
    // only means there is something "Analizuj" would still do.
    if (atLimit || step != collisionSteps.end())
        return !groupLevelsPlanned && liveLevelStep(issue) > 0.0 ? IssueHandling::cutAtLimit : IssueHandling::settled;
    return IssueHandling::open;
}

// Phase 28: the level step a collision would need on the mix as it plays now (cached a while, the
// report and the editor ask every tick).
double BrainProcessor::liveLevelStep(const ma::MaskingIssue& issue) const
{
    const auto key = std::make_pair(mix.channels[static_cast<size_t>(issue.masker)].owner, mix.channels[static_cast<size_t>(issue.masked)].owner);
    const auto now = juce::Time::getMillisecondCounter();
    const auto cached = liveSteps.find(key);
    if (cached != liveSteps.end() && now - cached->second.second < kLiveStepMs)
        return cached->second.first;
    const double needed = levelToClear(maskingInput(false), issue.masker, issue.masked, kMaxCollisionLevelDb);
    liveSteps[key] = { needed, now };
    return needed;
}

// Phase 23: a change in the proposal, not yet in the Agent, that works on this collision.
bool BrainProcessor::isPendingFor(const Proposal::Item& item, std::uint64_t masker, std::uint64_t masked, double lowHz, double highHz)
{
    if (item.advisory || item.selected == item.inPlace || item.owner != masker)
        return false;
    switch (item.action)
    {
        case Proposal::Item::Action::solver:
            return item.maskedOwner == masked || (item.band.hz >= lowHz && item.band.hz <= highHz);
        case Proposal::Item::Action::duck:
        case Proposal::Item::Action::channelLevel:
            return true;
        case Proposal::Item::Action::unmask:
            return item.maskedOwner == masked;
        case Proposal::Item::Action::chain:
            return item.tool == Proposal::Item::ChainTool::cleanup;
        case Proposal::Item::Action::polarity:
        case Proposal::Item::Action::harmonics:
        case Proposal::Item::Action::transients:
        case Proposal::Item::Action::level:
        case Proposal::Item::Action::balance:
            return false;
    }
    return false;
}

// The proposal's bands this channel's selected items refer to; the rest switch off. Without
// any change to its EQ (`changes` false) the Agent keeps what it has.
ma::EqSettings BrainProcessor::proposedEqFor(const MixSnapshot::Channel& channel, bool& changes) const
{
    const auto proposed = std::find_if(currentProposal.eq.begin(), currentProposal.eq.end(),
                                       [&](const auto& e) { return e.first == channel.owner; });
    ma::EqSettings eq {};
    changes = false;
    for (const auto& item : currentProposal.items)
    {
        const bool eqKind = item.action == Proposal::Item::Action::solver
                            && (item.change.kind == ma::SolverChange::Kind::eqCut
                                || item.change.kind == ma::SolverChange::Kind::tonalBoost
                                || item.change.kind == ma::SolverChange::Kind::tonalCut
                                || item.change.kind == ma::SolverChange::Kind::stemBoost
                                || item.change.kind == ma::SolverChange::Kind::stemCut);
        if (!eqKind || item.owner != channel.owner)
            continue;
        // Phase 23: a band already in place changes nothing while kept; unticking it removes it.
        if (item.selected != item.inPlace)
            changes = true;
        if (!item.selected)
            continue;
        if (proposed != currentProposal.eq.end())
            eq[static_cast<size_t>(item.change.eqSlot)] = proposed->second[static_cast<size_t>(item.change.eqSlot)];
    }
    return eq;
}

// Phase 28: what the mix will hear from a channel once the ticked proposal is applied: its gain
// (global and group levels; a bus keeps its trim), the new cleanup and the new EQ. Translation
// steps and dynamics are left out (small, and not where collisions are decided).
std::array<double, ma::bus::kBands> BrainProcessor::predictedHeard(const MixSnapshot::Channel& channel) const
{
    auto heard = channel.heardDb;
    if (!currentProposal.made)
        return heard;
    double gain = 0.0;
    const int group = groupOfChannel(channel);
    for (const auto& item : currentProposal.items)
    {
        if (!item.selected || item.advisory)
            continue;
        if (item.action == Proposal::Item::Action::solver && item.change.kind == ma::SolverChange::Kind::globalGain)
            gain += item.change.gainDb;
        else if (item.action == Proposal::Item::Action::level && group >= 0 && item.group == group)
            gain += item.gainDb;
        else if (item.action == Proposal::Item::Action::balance && item.owner == channel.owner)
            gain += item.gainDb;
    }
    if (!inMixSums(channel))
        gain = 0.0;
    auto cleanup = channel.data.cleanup;
    for (const auto& item : currentProposal.items)
        if (item.selected && item.owner == channel.owner && item.action == Proposal::Item::Action::chain
            && item.tool == Proposal::Item::ChainTool::cleanup)
            cleanup = item.cleanup;
    ma::EqSettings now {};
    if (channel.data.eqBypass == 0)
        for (int b = 0; b < ma::kEqBands; ++b)
            now[static_cast<size_t>(b)] = channel.data.eq[b];
    bool eqChanges = false;
    auto eq = proposedEqFor(channel, eqChanges);
    if (!eqChanges)
        eq = now;
    for (int b = 0; b < ma::bus::kBands; ++b)
    {
        const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
        auto& db = heard[static_cast<size_t>(b)];
        db += gain + ma::cleanupResponseDb(cleanup, hz) - ma::cleanupResponseDb(channel.data.cleanup, hz) + ma::eqResponseDb(eq, hz)
              - ma::eqResponseDb(now, hz);
    }
    return heard;
}

std::vector<ma::MaskingChannel> BrainProcessor::maskingInput(bool afterProposal) const
{
    std::vector<ma::MaskingChannel> masking(mix.channels.size());
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        const auto& channel = mix.channels[c];
        masking[c].valid = channel.spectrumValid && inMixSums(channel);
        masking[c].priority = channel.data.priority;
        masking[c].background = channel.data.role == static_cast<int>(ma::Role::send);
        masking[c].rawDb = afterProposal ? predictedHeard(channel) : channel.heardDb;
    }
    return masking;
}

bool BrainProcessor::duckedCollision(int maskerIndex, int maskedIndex, double lowHz, double highHz) const
{
    const auto& masker = mix.channels[static_cast<size_t>(maskerIndex)].data;
    const auto& masked = mix.channels[static_cast<size_t>(maskedIndex)].data;
    // The sidechain is the kick (that is how the duck proposals set it up).
    return masked.role == 1 && masker.duck.on > 0.5f && (masker.sidechainConnected != 0 || masker.duckKeyFound != 0)
           && duckCovers(masker.duck, lowHz, highHz);
}

bool BrainProcessor::unmaskedCollision(int maskerIndex, int maskedIndex, double lowHz, double highHz) const
{
    const auto& masker = mix.channels[static_cast<size_t>(maskerIndex)].data;
    const auto key = mix.channels[static_cast<size_t>(maskedIndex)].data.keyId;
    if (key == 0)
        return false;
    for (int d = 0; d < ma::kMaxUnmask; ++d)
        if (ma::unmaskOn(masker.unmask[d]) && masker.unmaskKey[d] == key && masker.unmaskKeyFound[d] != 0
            && dipCovers(masker.unmask[d], lowHz, highHz))
            return true;
    return false;
}

// Phase 30: a collision EQ and a small level step do not clear: the masker dips in the masked
// channel's part of the range while that one plays (keyed over the bus, no cable). Sized on the
// predicted mix: the smallest depth (2-6 dB) that clears it while both play, or 6 dB when that
// at least takes the score down by 0.15.
void BrainProcessor::planUnmask(const std::vector<ma::MaskingChannel>& masking, const ma::MaskingIssue& issue, CollisionStep& step,
                                std::map<int, std::vector<Proposal::Item>>& dips) const
{
    const auto& masker = mix.channels[static_cast<size_t>(issue.masker)];
    const auto& masked = mix.channels[static_cast<size_t>(issue.masked)];
    const auto maskerRole = static_cast<ma::Role>(juce::jlimit(0, ma::kRoles - 1, static_cast<int>(masker.data.role)));
    // Agents from before phase 30 neither publish a key nor dip; a kick or percussion dipping
    // under a sustained part would lose its hit.
    if (masked.data.keyId == 0 || masker.data.keyId == 0 || maskerRole == ma::Role::kick || maskerRole == ma::Role::percussion
        || !inMixSums(masker) || !inMixSums(masked))
        return;

    // Where the masked channel is in the range: its loudest part (as the ear weighs it), within 12 dB.
    const auto perceived = [&](int b) {
        return masking[static_cast<size_t>(issue.masked)].rawDb[static_cast<size_t>(b)]
               - ma::equalLoudnessWeightDb(ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b));
    };
    double top = -200.0;
    for (int b = issue.lowBand; b <= issue.highBand; ++b)
        top = std::max(top, perceived(b));
    int lo = -1, hi = -1;
    for (int b = issue.lowBand; b <= issue.highBand; ++b)
        if (perceived(b) >= top - 12.0)
        {
            lo = lo < 0 ? b : lo;
            hi = b;
        }
    if (lo < 0)
        return;
    double lowHz = std::max(kUnmaskLowestHz, ma::SpectrumAnalyzer::bandEdgeHz(ma::BandScale::erb, lo));
    double highHz = std::min(kUnmaskHighestHz, ma::SpectrumAnalyzer::bandEdgeHz(ma::BandScale::erb, hi + 1));
    if (highHz < lowHz * 1.5)
    {
        const double middle = std::sqrt(lowHz * highHz);
        lowHz = middle / std::sqrt(1.5);
        highHz = middle * std::sqrt(1.5);
    }
    const double ratio = highHz / lowHz;
    ma::UnmaskSettings dip;
    dip.on = 1.0f;
    dip.hz = static_cast<float>(std::sqrt(lowHz * highHz));
    dip.q = static_cast<float>(juce::jlimit(0.5, 2.0, std::sqrt(ratio) / (ratio - 1.0)));
    dip.keyBands = static_cast<float>(ma::keyBandsFor(lowHz, highHz));

    const double before = ma::analyseMasking(masking).amount[static_cast<size_t>(issue.masker)][static_cast<size_t>(issue.masked)];
    double depth = 0.0, after = before;
    bool clears = false;
    for (double d = 2.0; d <= kMaxUnmaskDb + 1e-9; d += 1.0)
    {
        const auto result = ma::analyseMasking(withDip(masking, issue.masker, dip, d));
        depth = d;
        after = result.amount[static_cast<size_t>(issue.masker)][static_cast<size_t>(issue.masked)];
        if (!collisionStillThere(result, issue.masker, issue.masked))
        {
            clears = true;
            break;
        }
    }
    if (!clears && after > before - 0.15)
        return;
    dip.depthDb = static_cast<float>(depth);

    Proposal::Item item;
    item.action = Proposal::Item::Action::unmask;
    item.owner = masker.owner;
    item.maskedOwner = masked.owner;
    item.unmask = dip;
    item.unmaskKey = masked.data.keyId;
    item.partial = !clears;
    item.text = quoted(masker.name) + pl(": ustępuje ") + quoted(masked.name) + pl(" wokół ") + hzText(dip.hz) + " (" + hzText(lowHz) + pl("–")
                + hzText(highHz) + pl(") do ") + juce::String(depth, 0) + pl(" dB, tylko gdy ") + quoted(masked.name)
                + pl(" gra. Zasłania go tam (") + juce::String(juce::roundToInt(issue.score * 100.0)) + "%)"
                + (clears ? pl(", a ani EQ, ani mały krok poziomu tego nie usuną; tak zniknie")
                          : pl(", a ani EQ, ani mały krok poziomu tego nie usuną; tak będzie dużo słabiej (")
                                + juce::String(juce::roundToInt(after * 100.0)) + "%)")
                + pl(". Bez kabla: Agenty dogadują się same");
    step.unmask = true;
    dips[issue.masker].push_back(item);
}

void BrainProcessor::planCollisionSteps()
{
    // Called again by the whole-mix pass on the real mix: start over.
    auto& items = currentProposal.items;
    items.erase(std::remove_if(items.begin(), items.end(),
                               [](const auto& i) {
                                   return i.action == Proposal::Item::Action::channelLevel || i.action == Proposal::Item::Action::unmask;
                               }),
                items.end());
    collisionSteps.clear();
    liveSteps.clear();

    // Phase 28: judged on the mix as it will be once the proposal is in, so one "Analizuj" already
    // knows which collisions its EQ cuts will not clear and makes those maskers quieter in the same
    // proposal (before, that took a second analysis after "Zastosuj").
    const auto masking = maskingInput(true);
    const auto predicted = ma::analyseMasking(masking);
    for (const auto& issue : mix.masking.issues)
        if (!duckedCollision(issue.masker, issue.masked, issue.lowHz, issue.highHz)
            && !unmaskedCollision(issue.masker, issue.masked, issue.lowHz, issue.highHz)
            && !collisionStillThere(predicted, issue.masker, issue.masked))
        {
            CollisionStep step;
            step.masker = mix.channels[static_cast<size_t>(issue.masker)].owner;
            step.masked = mix.channels[static_cast<size_t>(issue.masked)].owner;
            step.clearedByProposal = true;
            collisionSteps.push_back(step);
        }

    std::map<int, std::pair<double, juce::StringArray>> quieter; // masker: dB, what it frees
    std::map<int, std::vector<Proposal::Item>> dips;              // phase 30: masker: new dips
    for (const auto& issue : predicted.issues)
    {
        const auto& masker = mix.channels[static_cast<size_t>(issue.masker)];
        const auto& masked = mix.channels[static_cast<size_t>(issue.masked)];
        if (duckedCollision(issue.masker, issue.masked, issue.lowHz, issue.highHz)
            || unmaskedCollision(issue.masker, issue.masked, issue.lowHz, issue.highHz))
            continue;
        const bool duckPlanned = std::any_of(items.begin(), items.end(), [&](const auto& i) {
            return i.selected && i.action == Proposal::Item::Action::duck && i.owner == masker.owner && i.maskedOwner == masked.owner;
        });
        if (duckPlanned)
            continue; // the duck takes it on every hit
        // Phase 30: while the group levels are planned, a channel's own level step fights them (the
        // next analysis puts the group back, the step comes again), so the masker gives way
        // dynamically instead. Otherwise a small step is an easy yes, and past it the dip.
        const double needed = groupLevelsPlanned ? 0.0 : levelToClear(masking, issue.masker, issue.masked, kMaxCollisionLevelDb);
        CollisionStep step;
        step.masker = masker.owner;
        step.masked = masked.owner;
        step.levelDb = needed;
        if (needed <= 0.0 || needed > kSmallLevelStepDb)
            planUnmask(masking, issue, step, dips);
        collisionSteps.push_back(step);
        if (needed > 0.0 && !step.unmask)
        {
            auto& entry = quieter[issue.masker];
            entry.first = std::max(entry.first, needed);
            entry.second.add(quoted(masked.name) + pl(" w ") + hzText(issue.lowHz) + pl("–") + hzText(issue.highHz) + " ("
                             + juce::String(juce::roundToInt(issue.score * 100.0)) + "%)");
        }
    }
    for (const auto& [index, entry] : quieter)
    {
        const auto& channel = mix.channels[static_cast<size_t>(index)];
        Proposal::Item item;
        item.action = Proposal::Item::Action::channelLevel;
        item.owner = channel.owner;
        item.gainDb = -entry.first;
        // A small step is an easy yes; a bigger one changes the balance, so it is the user's call.
        item.selected = entry.first <= kSmallLevelStepDb;
        item.text = quoted(channel.name) + pl(": poziom ") + juce::String(item.gainDb, 1) + pl(" dB, bo dalej zasłania ")
                    + entry.second.joinIntoString(", ") + pl(", a EQ zrobił już, ile mógł. Tyle wystarczy, żeby kolizja zniknęła")
                    + (item.selected ? juce::String() : pl(" (zmienia balans, więc zaznacz sam, jeśli chcesz)"));
        items.push_back(item);
    }

    // Phase 30: each masker has kMaxUnmask dips. Dips it already runs for other channels stay; the
    // new ones go by the masked channel's importance, then how much they free.
    for (auto& [index, list] : dips)
    {
        const auto& channel = mix.channels[static_cast<size_t>(index)];
        std::stable_sort(list.begin(), list.end(), [&](const auto& a, const auto& b) {
            const auto priority = [&](std::uint64_t owner) {
                for (const auto& c : mix.channels)
                    if (c.owner == owner)
                        return static_cast<int>(c.data.priority);
                return 99;
            };
            return priority(a.maskedOwner) < priority(b.maskedOwner);
        });
        std::array<bool, ma::kMaxUnmask> taken {};
        for (int d = 0; d < ma::kMaxUnmask; ++d)
        {
            const bool keyedToNew = std::any_of(list.begin(), list.end(), [&](const auto& i) { return i.unmaskKey == channel.data.unmaskKey[d]; });
            taken[static_cast<size_t>(d)] = ma::unmaskOn(channel.data.unmask[d]) && channel.data.unmaskKeyFound[d] != 0 && !keyedToNew;
        }
        for (auto& item : list)
        {
            int free = -1;
            for (int d = 0; d < ma::kMaxUnmask && free < 0; ++d)
                if (!taken[static_cast<size_t>(d)])
                    free = d;
            if (free < 0)
            {
                // No dip left: the collision stays as the level step would have left it.
                for (auto& step : collisionSteps)
                    if (step.masker == item.owner && step.masked == item.maskedOwner)
                        step.unmask = false;
                continue;
            }
            taken[static_cast<size_t>(free)] = true;
            item.unmaskIndex = free;
            items.push_back(item);
        }
    }
}

juce::String BrainProcessor::settledAdvice(const ma::MaskingIssue& issue) const
{
    const auto& masker = mix.channels[static_cast<size_t>(issue.masker)];
    const auto role = static_cast<ma::Role>(juce::jlimit(0, ma::kRoles - 1, static_cast<int>(masker.data.role)));
    // mixcheck (a "KickSnare" stem): a kick covering something well above its own register carries
    // another hit in the same track, which no setting on one channel can separate.
    if (role == ma::Role::kick && issue.lowHz >= 1000.0)
        return pl("W śladzie ") + quoted(masker.name) + pl(" gra też coś wysokiego (werbel, clap?): rozdziel to na osobny kanał z własnym Agentem.");
    // Hits cover only for a moment; moving them an octave says nothing about a drum.
    if (role == ma::Role::kick || role == ma::Role::percussion)
        return pl("Uderzenia zasłaniają tylko na chwilę: zostaw albo wybierz próbkę ") + quoted(masker.name) + pl(" o innym brzmieniu.");
    return pl("To ten sam rejestr: przenieś ") + quoted(masker.name) + pl(" oktawę wyżej, zmień brzmienie albo zostaw (lekkie nakładanie jest normalne).");
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
        case IssueHandling::unmasked: text += pl(": ustępuje mu dynamicznie, tylko gdy tamten gra (bez kabla); w średniej tego nie widać"); break;
        case IssueHandling::cutAtLimit:
            text += pl(": cięcie już na limicie; zniknie, gdy ") + quoted(masker.name) + pl(" będzie ciszej o ")
                    + juce::String(liveLevelStep(issue), 1) + pl(" dB. Kliknij „Analizuj”, doda to do propozycji");
            break;
        case IssueHandling::pending: text += pl(": w propozycji jest na to zmiana, kliknij „Zastosuj”"); break;
        case IssueHandling::levelStep:
        {
            const auto step = std::find_if(collisionSteps.begin(), collisionSteps.end(),
                                           [&](const auto& c) { return c.masker == masker.owner && c.masked == masked.owner; });
            text += pl(": EQ zrobił, ile mógł; zniknie, gdy ") + quoted(masker.name) + pl(" będzie ciszej o ")
                    + juce::String(step != collisionSteps.end() ? step->levelDb : 0.0, 1)
                    + (currentProposal.made ? pl(" dB (jest w propozycji, odznaczone, bo zmienia balans: zaznacz, jeśli chcesz)")
                                            : pl(" dB (to zmienia balans, więc zostawiłem to Tobie)"));
            break;
        }
        case IssueHandling::settled:
            text += pl(": miks zrobił, ile mógł. ") + settledAdvice(issue);
            break;
        case IssueHandling::open: text += pl(": kliknij „Analizuj”"); break;
    }
    return text;
}

// Phase 27: the translation steps (room cuts, presence, overtones) are chosen after the solver and
// move the balance too. When they move it noticeably, the proposal is made once more with them
// counted in the solver's tonal prediction, so one "Zastosuj" lands where the balance was aimed.
void BrainProcessor::propose(const std::array<double, ma::kStemGroups>* groupGain)
{
    groupLevelsPlanned = groupGain != nullptr;
    translationExtra.clear();
    proposeOnce(groupGain);
    if (!mix.tonal.valid || translationDelta.empty())
        return;
    double largest = 0.0;
    for (int b = 0; b < ma::bus::kBands; ++b)
    {
        double before = 0.0, after = 0.0;
        for (const auto& channel : mix.channels)
        {
            if (!channel.spectrumValid || !inMixSums(channel))
                continue;
            const double heard = channel.heardDb[static_cast<size_t>(b)];
            const auto delta = translationDelta.find(channel.owner);
            before += std::pow(10.0, heard / 10.0);
            after += std::pow(10.0, (heard + (delta != translationDelta.end() ? delta->second[static_cast<size_t>(b)] : 0.0)) / 10.0);
        }
        if (before > 1e-20 && after > 1e-20)
            largest = std::max(largest, std::abs(10.0 * std::log10(after / before)));
    }
    if (largest < kTranslationShiftDb)
        return;
    translationExtra = translationDelta;
    proposeOnce(groupGain);
}

void BrainProcessor::proposeOnce(const std::array<double, ma::kStemGroups>* groupGain)
{
    currentProposal = {};
    toolWhy.clear();
    currentProposal.made = true;

    const double peak = truePeakDb();
    const double over = std::isfinite(peak) ? peak - peakCeilingDb() : 0.0;
    double headroomGain = over > 0.0 ? -std::ceil((over + 0.5) * 2.0) / 2.0 : 0.0;
    // mixcheck: the cut for the ceiling is sized on the peaks before the planned clippers, which
    // then take the kick's and hats' peaks down too, so the master landed ~5 dB under the ceiling.
    // While refining, what the clippers freed comes back (never more than was taken, 1 dB kept).
    if (over <= 0.0 && std::isfinite(peak) && refine.active && refine.globalGainDb < 0.0)
    {
        const double room = std::floor((-over - kRestoreMarginDb) * 2.0) / 2.0;
        const double back = std::min(-refine.globalGainDb, room);
        if (back >= 0.5)
            headroomGain = back;
    }
    const auto stems = stemShapeInput();
    const auto solved = solveNow(headroomGain, stems.target != nullptr ? &stems : nullptr, groupGain);
    currentProposal.crowdingBefore = solved.crowdingBefore;
    currentProposal.crowdingAfter = solved.crowdingAfter;
    for (size_t c = 0; c < mix.channels.size(); ++c)
        currentProposal.eq.emplace_back(mix.channels[c].owner, solved.eq[c]);

    const auto roles = ma::ui::roleNames();
    for (const auto& change : solved.changes)
    {
        Proposal::Item item;
        item.change = change;
        const auto targetName = quoted(juce::String::fromUTF8(activeTarget().name.c_str()));
        const auto range = hzText(change.lowHz) + pl("–") + hzText(change.highHz);
        if (change.kind == ma::SolverChange::Kind::globalGain)
        {
            if (change.gainDb > 0.0)
                item.text = pl("Wszystkie kanały: +") + juce::String(change.gainDb, 1) + pl(" dB z powrotem: po zastosowaniu szczyt sumy jest ")
                            + juce::String(-over, 1) + pl(" dB pod limitem pre-master, bo clippery ścięły szczyty. Oddaję część ściszenia, zapas 1.5 dB zostaje");
            else
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
                        + hzText(item.band.hz) + " (Q " + juce::String(item.band.q, 1) + pl("), bo ");
            if (change.centre && std::abs(change.deviationDb) < 1e-9)
                item.text += centreText(change, targetName);
            else
            {
                item.text += pl("w ") + range + (boost ? pl(" brakuje ") : pl(" jest ")) + juce::String(std::abs(change.deviationDb), 1)
                             + (boost ? pl(" dB do celu ") : pl(" dB za dużo względem celu ")) + targetName;
                if (change.centre)
                    item.text += pl(", i dalej do środka: ") + centreText(change, targetName);
            }
        }
        else if (change.kind == ma::SolverChange::Kind::eqCut && static_cast<size_t>(change.maskedChannel) >= mix.channels.size())
        {
            // Phase 17: room for a part the sketch does not have yet.
            const auto& masker = mix.channels[static_cast<size_t>(change.channel)];
            const auto& ghost = mix.sketch.analysis.ghosts[static_cast<size_t>(change.maskedChannel) - mix.channels.size()];
            item.owner = masker.owner;
            item.band = solved.eq[static_cast<size_t>(change.channel)][static_cast<size_t>(change.eqSlot)];
            // Room for a ghost is at most 3 dB; a band that ends deeper is shared with a real collision's cut.
            const double own = std::max(static_cast<double>(item.band.gainDb), change.gainDb < 0.0 ? change.gainDb : -1e9);
            item.text = quoted(masker.name) + ": " + juce::String(own, 1) + pl(" dB przy ") + hzText(item.band.hz)
                        + " (Q " + juce::String(item.band.q, 1) + pl("), zostawia miejsce na partię, której jeszcze nie ma: ")
                        + ghostName(ghost) + pl(" w ") + hzText(change.lowHz) + pl("–") + hzText(change.highHz) + " ("
                        + juce::String(juce::roundToInt(change.scoreBefore * 100.0)) + pl("% → ")
                        + juce::String(juce::roundToInt(change.scoreAfter * 100.0)) + "%)";
            if (item.band.gainDb < own - 0.05)
                for (const auto& other : solved.changes)
                    if (other.kind == ma::SolverChange::Kind::eqCut && other.channel == change.channel && other.eqSlot == change.eqSlot
                        && static_cast<size_t>(other.maskedChannel) < mix.channels.size())
                    {
                        item.text += pl(". To samo pasmo pogłębia cięcie dla ")
                                     + quoted(mix.channels[static_cast<size_t>(other.maskedChannel)].name) + pl(", razem ")
                                     + juce::String(item.band.gainDb, 1) + " dB";
                        break;
                    }
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
        // Phase 23: the solver works from pre-EQ spectra, so after "Zastosuj" it proposes what already runs.
        // Such a band stays (apply keeps it) but is no new change.
        if (item.owner != 0 && !item.advisory && change.kind != ma::SolverChange::Kind::globalGain)
            if (const auto channel = std::find_if(mix.channels.begin(), mix.channels.end(), [&](const auto& c) { return c.owner == item.owner; });
                channel != mix.channels.end() && eqHasBand(*channel, item.band))
            {
                item.inPlace = true;
                item.text = pl("Już działa: ") + item.text;
            }
        currentProposal.items.push_back(item);
    }
    // Phase 17: what the sketch lacks, first, as advice: the plan above already counts with it.
    if (mix.sketch.active())
    {
        std::vector<Proposal::Item> parts;
        for (const auto& ghost : mix.sketch.analysis.ghosts)
        {
            Proposal::Item item;
            item.advisory = true;
            item.selected = false;
            item.text = pl("Szkic, brakująca partia. ") + ghostBrief(ghost);
            parts.push_back(item);
        }
        currentProposal.items.insert(currentProposal.items.begin(), parts.begin(), parts.end());
    }
    proposeDynamics();
    proposeHarmonics();
    planCollisionSteps();
}

namespace
{
bool sameCleanup(const ma::CleanupSettings& a, const ma::CleanupSettings& b)
{
    return std::abs(a.hpfHz - b.hpfHz) < 1.0f && std::abs(a.monoBelowHz - b.monoBelowHz) < 1.0f
           // A broad mud cut (Q 1) a neighbouring band away is the same cut: the loudest band of a
           // melodic part moves with the notes played while it was measured.
           && std::abs(a.mudDb - b.mudDb) < 0.1f && (a.mudDb > -0.05f || std::abs(std::log2(a.mudHz / b.mudHz)) < 0.34f)
           && std::equal(a.notches.begin(), a.notches.end(), b.notches.begin(), [](const auto& x, const auto& y) {
                  return std::abs(x.db - y.db) < 0.1f && (x.db > -0.05f || std::abs(std::log2(x.hz / y.hz)) < 0.02);
              });
}

bool sameCompressor(const ma::CompressorSettings& a, const ma::CompressorSettings& b)
{
    return (a.on > 0.5f) == (b.on > 0.5f) && std::abs(a.thresholdDb - b.thresholdDb) < 0.5f && std::abs(a.ratio - b.ratio) < 0.1f
           && std::abs(a.releaseMs - b.releaseMs) < 5.0f && std::abs(a.makeupDb - b.makeupDb) < 0.3f;
}

// Phase 32: the same dynamic EQ, within what a parameter round trip and a re-measure change.
bool sameDynamicEq(const ma::DynamicEqSettings& a, const ma::DynamicEqSettings& b)
{
    for (int k = 0; k < ma::kDynamicBands; ++k)
    {
        const auto& x = a.bands[static_cast<size_t>(k)];
        const auto& y = b.bands[static_cast<size_t>(k)];
        if (ma::dynamicBandOn(x) != ma::dynamicBandOn(y))
            return false;
        if (ma::dynamicBandOn(x) && (std::abs(x.thresholdDb - y.thresholdDb) > 1.0f || std::abs(x.rangeDb - y.rangeDb) > 0.5f
                                     || std::abs(std::log2(x.hz / std::max(y.hz, 1.0f))) > 0.1f))
            return false;
    }
    if (ma::riderOn(a.rider) != ma::riderOn(b.rider))
        return false;
    return !ma::riderOn(a.rider) || (std::abs(a.rider.targetDb - b.rider.targetDb) < 1.0f && std::abs(a.rider.rangeDb - b.rider.rangeDb) < 0.5f);
}

juce::String dynamicBandName(int probe)
{
    switch (static_cast<ma::DynamicProbe>(probe))
    {
        case ma::DynamicProbe::boom: return pl("dudnienie ");
        case ma::DynamicProbe::harsh: return pl("ostrość ");
        case ma::DynamicProbe::sibilance: return pl("de-esser ");
    }
    return {};
}

juce::String dynamicEqText(const ma::ChainPlan& plan)
{
    juce::StringArray parts;
    for (int p = 0; p < ma::kDynamicBands; ++p)
    {
        const auto& band = plan.dynamicEq.bands[static_cast<size_t>(p)];
        if (!ma::dynamicBandOn(band))
            continue;
        parts.add(dynamicBandName(p) + hzText(band.hz) + pl(" do -") + juce::String(band.rangeDb, 1)
                  + pl(" dB, tylko gdy wystaje (w szczytach ") + juce::String(plan.probeSpikeDb[static_cast<size_t>(p)], 1)
                  + pl(" dB ponad zwykły poziom)") + (plan.probeCoversImportant[static_cast<size_t>(p)] ? pl(", zasłania tam ważniejszy kanał") : juce::String()));
    }
    if (ma::riderOn(plan.dynamicEq.rider))
        parts.add(pl("rider ±") + juce::String(plan.dynamicEq.rider.rangeDb, 1) + pl(" dB (frazy różnią się o ")
                  + juce::String(plan.riderSpreadDb, 1) + pl(" dB), przed kompresorem"));
    return parts.isEmpty() ? pl("dynamiczne EQ wyłączone: nic już nie wystaje chwilami") : pl("dynamiczne EQ: ") + parts.joinIntoString("; ");
}

juce::String depthText(const ma::ChainPlan& plan)
{
    if (ma::depthOn(plan.depth))
        return pl("góra ścięta od ") + juce::String(plan.depth.highCutHz / 1000.0f, 0) + pl(" kHz: wysyłka jest jasna jak reszta miksu (")
               + juce::String(plan.sendTiltDb - plan.mixTiltDb, 1) + pl(" dB względem niej w 8–16 kHz), ciemniejszy ogon stoi za suchymi śladami");
    return pl("góra bez cięcia: wysyłka jest już ") + juce::String(plan.mixTiltDb - plan.sendTiltDb, 1)
           + pl(" dB ciemniejsza od miksu w 8–16 kHz, stoi za suchymi śladami");
}

// Phase 33: why a channel stays in the centre, and where it goes.
juce::String panReasonText(ma::PanPlan::Reason reason, double inputBalanceDb)
{
    using R = ma::PanPlan::Reason;
    switch (reason)
    {
        case R::centreRole: return pl("stopa, bas, busy i wysyłki trzymają środek");
        case R::mainPart: return pl("główny wokal albo lead trzyma środek");
        case R::carriesLow:
            return pl("dużo grania poniżej 150 Hz (np. pętla ze stopą): przesunięcie zabrałoby stopę ze środka");
        case R::prePanned:
            return pl("wejście już przechyla się w ") + (inputBalanceDb > 0.0 ? pl("lewo") : pl("prawo")) + " o "
                   + juce::String(std::abs(inputBalanceDb), 1) + pl(" dB, to panorama producenta");
        case R::wide: return pl("kanał jest już szeroki, przesunięcie tylko by go przechyliło");
        case R::quiet: return pl("za cichy w miksie, żeby jego miejsce coś zmieniało");
        case R::alone: return pl("nie ma go czym zrównoważyć po drugiej stronie, więc zostaje w środku");
        case R::user: return pl("panorama ustawiona ręcznie w Agencie");
        case R::paired:
        case R::balance:
        case R::none: break;
    }
    return {};
}

juce::String panText(const ma::PanPlan& plan, const juce::String& partner, double inputBalanceDb)
{
    const double p = plan.settings.position;
    if (!ma::panOn(plan.settings))
        return pl("panorama na środku: ") + panReasonText(plan.reason, inputBalanceDb);
    auto text = pl("panorama ") + juce::String(juce::roundToInt(std::abs(p) * 100.0)) + (p < 0.0 ? pl("% w lewo") : pl("% w prawo"));
    if (plan.reason == ma::PanPlan::Reason::paired && partner.isNotEmpty())
        text += pl(" naprzeciw ") + partner + pl(" (grają w tym samym zakresie, wspólne ") + juce::String(juce::roundToInt(plan.overlap * 100.0))
                + pl("% widma: rozstawione nie zasłaniają się)");
    else
        text += pl(", po stronie, która trzyma miks w równowadze lewo/prawo");
    if (plan.limited)
        text += pl(", bliżej środka, żeby miks nie przechylił się na bok");
    if (plan.settings.monoBelowHz > 0.0f)
        text += pl(", poniżej ") + juce::String(juce::roundToInt(plan.settings.monoBelowHz)) + pl(" Hz zostaje w środku");
    return text;
}

juce::String stereoFixText(const ma::StereoPlan::Fix& fix)
{
    const auto range = hzText(fix.lowHz) + pl("–") + hzText(fix.highHz);
    if (fix.toReference)
    {
        const auto widths = pl(" (korelacja ") + juce::String(fix.coherenceBefore, 2) + pl(" → ") + juce::String(fix.coherenceAfter, 2)
                            + pl(", grupa w referencjach ") + juce::String(fix.referenceCoherence, 2) + pl("; nigdy poniżej 0,2)");
        if (fix.widener)
            return pl("poszerzenie od ") + hzText(fix.lowHz) + pl(": kanał jest prawie mono, a jego grupa gra w referencjach szerzej. "
                                                                 "Do side idzie kopia środka opóźniona o kilka ms, ")
                   + juce::String(fix.gainDb, 1) + pl(" dB pod środkiem; w mono nic się nie zmienia") + widths;
        return pl("side +") + juce::String(fix.gainDb, 1) + pl(" dB w ") + range + pl(": jego grupa gra tu w referencjach szerzej") + widths;
    }
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

// Phase 28: the kick and percussion parts whose hits are the master's highest peaks (within 6 dB
// of the highest held output peak), what their clippers take now, and how hard the master
// preview's limiter works: the planner clips them further when a loud master would flatten them.
void BrainProcessor::markMasterPeakCarriers(std::vector<ma::ChainChannel>& chain, ma::ChainContext& context)
{
    const auto stats = previewStats();
    if (stats.seconds < kMinimumPreviewSeconds)
        return;
    context.limiterReductionDb = std::max(0.0, stats.typicalReductionDb);
    // Judged on the peaks as they would be without the clippers: a kick clipped for the master
    // last time must not stop counting as a carrier because of it (the plan would swing back).
    std::vector<double> unclipped(mix.channels.size(), -200.0);
    double highest = -200.0;
    for (size_t c = 0; c < mix.channels.size() && c < chain.size(); ++c)
    {
        const auto& channel = mix.channels[c];
        const auto held = peakHold.find(channel.owner);
        if (!inMixSums(channel) || held == peakHold.end())
            continue;
        const auto& clipper = channel.data.clipper;
        if (clipper.on > 0.5f && chain[c].truePeakDb > -100.0)
            chain[c].currentClipDb = std::max(0.0, chain[c].truePeakDb - clipper.ceilingDb);
        // The Learn pass's true peak (after the trim) is the part before its clipper; the held
        // output peak can be higher (EQ, saturation, attack emphasis after the clipper).
        unclipped[c] = chain[c].truePeakDb > -100.0 ? std::max(static_cast<double>(held->second), chain[c].truePeakDb) : held->second;
        highest = std::max(highest, unclipped[c]);
    }
    loudMasterPlan.loudest = {};
    for (size_t c = 0; c < mix.channels.size() && c < chain.size(); ++c)
        if (unclipped[c] >= highest - 1e-9 && highest > -200.0)
            loudMasterPlan.loudest = mix.channels[c].name;
    for (size_t c = 0; c < mix.channels.size() && c < chain.size(); ++c)
        if (unclipped[c] > -200.0 && unclipped[c] >= highest - kPeakCarrierRangeDb
            && (chain[c].role == ma::Role::kick || chain[c].role == ma::Role::percussion))
            chain[c].carriesMasterPeaks = true;
}

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
        in.currentCleanup = channel.data.cleanup;
        in.intentionalDrive = channel.data.intentionalDrive != 0;
        in.contentRole = static_cast<ma::Role>(juce::jlimit(0, static_cast<int>(ma::Role::send), static_cast<int>(channel.data.detectedRole)));
        in.contentConfidence = channel.data.detectedConfidence;
        for (int r = 0; r < ma::kMaxResonances; ++r)
            in.resonances[static_cast<size_t>(r)] = channel.data.resonances[r];
        in.probesValid = channel.data.probesValid != 0;
        for (int p = 0; p < ma::kDynamicBands; ++p)
        {
            in.probeP50[static_cast<size_t>(p)] = channel.data.probeP50[p];
            in.probeP95[static_cast<size_t>(p)] = channel.data.probeP95[p];
        }
    }
    ma::ChainContext context;
    context.bpm = tempoBpm();
    // Phase 39: where the target wants more, cleanup does not take it away.
    if (mix.tonal.valid && activeTarget().valid())
    {
        context.tonalKnown = true;
        for (int b = 0; b < ma::bus::kBands; ++b)
            context.mixMinusCentreDb[static_cast<size_t>(b)]
                = mix.tonal.mixDb[static_cast<size_t>(b)] > -149.0 ? mix.tonal.mixDb[static_cast<size_t>(b)] - activeTarget().centre[static_cast<size_t>(b)] : 0.0;
    }
    const double peak = truePeakDb();
    context.headroomShortDb = std::isfinite(peak) ? std::max(0.0, peak - peakCeilingDb()) : 0.0;
    loudMasterPlan = {};
    markMasterPeakCarriers(chain, context);
    const auto plans = ma::planChains(chain, context);
    bool allCapped = true;
    for (size_t c = 0; c < plans.size(); ++c)
        if (chain[c].carriesMasterPeaks && plans[c].dynamicsKnown && plans[c].peakKnown)
        {
            loudMasterPlan.carriers.add(quoted(mix.channels[c].name));
            loudMasterPlan.owners.push_back(mix.channels[c].owner);
            loudMasterPlan.clipDb = std::max(loudMasterPlan.clipDb, plans[c].loudMasterClipDb);
            allCapped = allCapped && plans[c].loudMasterCapped;
        }
    loudMasterPlan.capped = !loudMasterPlan.owners.empty() && allCapped;
    loudMasterPlan.made = context.limiterReductionDb >= 0.0;

    // Phase 8: keep every band in phase, channel by channel.
    std::vector<ma::StereoChannel> stereoIn(mix.channels.size());
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        const auto& channel = mix.channels[c];
        auto& in = stereoIn[c];
        in.role = chain[c].role;
        in.priority = channel.data.priority;
        in.valid = channel.spectrumValid && channel.stereoValid;
        in.group = ma::stemGroupForRole(in.role);
        in.userWidth = ma::widthOn(channel.data.width);
        in.current = channel.data.stereo;
        in.width = channel.data.width;
        for (int b = 0; b < ma::bus::kBands; ++b)
        {
            in.midDb[static_cast<size_t>(b)] = channel.midDb[static_cast<size_t>(b)];
            in.sideDb[static_cast<size_t>(b)] = channel.sideDb[static_cast<size_t>(b)];
        }
    }
    // Phase 25: with stem references, every part to its group's width in them.
    const auto width = keyedStems.valid() ? ma::widthTargetFrom(keyedStems) : ma::WidthTarget {};
    const auto stereoPlans = ma::planStereo(stereoIn, mix.master, &width);
    lastPlans.clear();
    lastStereoPlans.clear();
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        lastPlans[mix.channels[c].owner] = plans[c];
        lastStereoPlans[mix.channels[c].owner] = stereoPlans[c];
    }

    // Phase 33: every channel's place between the speakers, from the same pre-pan mid/side.
    std::vector<ma::PanChannel> panIn(mix.channels.size());
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        const auto& channel = mix.channels[c];
        auto& in = panIn[c];
        in.role = chain[c].role;
        in.priority = channel.data.priority;
        in.valid = channel.spectrumValid && channel.stereoValid;
        in.midDb = stereoIn[c].midDb;
        in.sideDb = stereoIn[c].sideDb;
        in.balanceKnown = channel.data.balanceValid != 0;
        in.inputBalanceDb = channel.data.inputBalanceDb;
        in.current = channel.data.pan;
        // A pan the Brain did not set (or that was moved since) is the user's.
        in.userPan = ma::panOn(channel.data.pan) && (channel.data.brainTools & ma::bus::toolPan) == 0;
    }
    lastPanResult = ma::planPan(panIn);
    lastPanPlans.clear();
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        PanNote note;
        note.plan = lastPanResult.plans[c];
        note.inputBalanceDb = panIn[c].inputBalanceDb;
        if (note.plan.partner >= 0 && note.plan.partner < static_cast<int>(mix.channels.size()))
            note.partner = quoted(mix.channels[static_cast<size_t>(note.plan.partner)].name);
        lastPanPlans[mix.channels[c].owner] = note;
    }

    // Collisions, kick and bass, translation: the usual proposal, made on the mix as it will
    // sound with the planned cleanup, so the solver does not cut what the high-pass removes.
    const auto saved = mix;
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        applyCleanup(mix.channels[c], plans[c].cleanup, plans[c].depth);
        mix.channels[c].data.cleanup = plans[c].cleanup; // the stem references see it too
        // Phase 31: a planned enhancer the Agent does not run yet is heard by the rest of the proposal
        // (collisions, translation, balance), and the translation steps do not add a second one.
        auto& channel = mix.channels[c];
        if (plans[c].harmonics.on > 0.5f && channel.data.harmonics.on < 0.5f && channel.data.intentionalDrive == 0)
        {
            ma::SpectrumAnalyzer::Bands heard {};
            for (int b = 0; b < ma::bus::kBands; ++b)
                heard[static_cast<size_t>(b)] = channel.heardDb[static_cast<size_t>(b)];
            const auto with = ma::predictHarmonics(heard, plans[c].harmonics);
            for (int b = 0; b < ma::bus::kBands; ++b)
            {
                const double delta = with[static_cast<size_t>(b)] - heard[static_cast<size_t>(b)];
                for (auto* curve : { &channel.rawDb, &channel.bandsDb, &channel.heardDb, &channel.outputDb })
                    (*curve)[static_cast<size_t>(b)] += delta;
            }
            channel.data.harmonics = plans[c].harmonics;
        }
    }
    std::vector<ma::MaskingChannel> masking(mix.channels.size());
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        masking[c].valid = mix.channels[c].spectrumValid && inMixSums(mix.channels[c]);
        masking[c].priority = mix.channels[c].data.priority;
        masking[c].background = mix.channels[c].data.role == static_cast<int>(ma::Role::send);
        for (int b = 0; b < ma::bus::kBands; ++b)
            masking[c].rawDb[static_cast<size_t>(b)] = mix.channels[c].heardDb[static_cast<size_t>(b)];
    }
    mix.masking = ma::analyseMasking(masking);

    // Phase 9 and 18: the stem groups' levels against the references. Planned before the EQ, so
    // the solver's tonal stage works on what the level moves leave instead of filling the same
    // hole twice, then again with the proposed EQ (its cuts and boosts move the groups too).
    planGroupBalance(); // inside each group first: the group levels then place the group as it will play
    const bool typicalLevels = typicalLevelsReady();
    ma::StemTarget levelTarget;
    auto planLevels = [&](bool withProposal) {
        std::vector<ma::EqSettings> eqs;
        for (const auto& channel : mix.channels)
        {
            const auto found = std::find_if(currentProposal.eq.begin(), currentProposal.eq.end(),
                                            [&](const auto& e) { return e.first == channel.owner; });
            const bool changes = withProposal && std::any_of(currentProposal.items.begin(), currentProposal.items.end(), [&](const auto& item) {
                return item.owner == channel.owner && item.action == Proposal::Item::Action::solver;
            });
            ma::EqSettings eq {};
            if (changes && found != currentProposal.eq.end())
                eq = found->second;
            else if (channel.data.eqBypass == 0)
                for (int b = 0; b < ma::kEqBands; ++b)
                    eq[static_cast<size_t>(b)] = channel.data.eq[b]; // apply keeps what is there
            eqs.push_back(eq);
        }
        std::array<bool, ma::kStemGroups> present {}, ghostOnly {};
        // Phase 39: before EQ. The tonal stage's bells (applied in earlier rounds or proposed now) shape the
        // master towards the target; counted here they came back as level (a sub cut lifted with the trim).
        auto groups = stemGroupDb(false, present, nullptr, &eqs);
        addGhostGroups(groups, present, ghostOnly);
        // Phase 29: without stem references, the typical house balance of the groups.
        if (typicalLevels)
            levelTarget = ma::typicalLevelTarget(present);
        auto plan = ma::planStemLevels(groups, present, typicalLevels ? levelTarget : keyedStems, &ghostOnly);
        if (typicalLevels)
            ma::anchorStemLevels(plan, static_cast<int>(ma::StemGroup::drums));
        return plan;
    };
    const bool levelsPlanned = stemsReady() || typicalLevels;
    ma::StemLevelPlan levels;
    std::array<double, ma::kStemGroups> groupGain {};
    if (levelsPlanned)
        groupGain = planLevels(false).gainDb;
    propose(levelsPlanned ? &groupGain : nullptr);
    if (levelsPlanned)
    {
        levels = planLevels(true);
        double moved = 0.0;
        for (int g = 0; g < ma::kStemGroups; ++g)
            moved = std::max(moved, std::abs(levels.gainDb[static_cast<size_t>(g)] - groupGain[static_cast<size_t>(g)]));
        if (moved > 0.5)
        {
            groupGain = levels.gainDb;
            propose(&groupGain);
            levels = planLevels(true);
        }
    }

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

        // The mix above carries the planned cleanup already (for the solver): compare with the Agent's own.
        if (!sameCleanup(plan.cleanup, saved.channels[c].data.cleanup))
        {
            item.tool = Proposal::Item::ChainTool::cleanup;
            item.cleanup = plan.cleanup;
            const auto text = cleanupText(plan.cleanup, plan.highPassReason, plan.mudCrowd, &plan);
            item.text = name + (text.isEmpty() ? pl("porządki wyłączone, nie są potrzebne") : text);
            chainItems.push_back(item);
        }
        if (plan.glue && !sameCompressor(plan.compressor, channel.data.compressor))
        {
            item.tool = Proposal::Item::ChainTool::compressor;
            item.compressor = plan.compressor;
            item.text = name + glueText(plan, bpm);
            chainItems.push_back(item);
        }
        else if (plan.compressor.on > 0.5f && !sameCompressor(plan.compressor, channel.data.compressor))
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
            && (channel.data.clipper.on < 0.5f || std::abs(plan.clipper.ceilingDb - channel.data.clipper.ceilingDb) > 1.0f)) // phase 23: a trim move shifts the ceiling a little; not a new change
        {
            item.tool = Proposal::Item::ChainTool::clipper;
            item.clipper = plan.clipper;
            item.text = name + pl("clipper ścina ") + juce::String(plan.clipDb, 1) + pl(" dB szczytów (sufit ")
                        + juce::String(plan.clipper.ceilingDb, 1) + pl(" dBFS): szczyt jest ") + juce::String(plan.crestDb, 1)
                        + pl(" dB nad głośną częścią, wystarczy ") + juce::String(plan.maxCrestDb, 0)
                        + pl(". Zapas na mastering bez słyszalnej straty");
            if (plan.loudMasterClipDb > 0.0)
                item.text = name + pl("clipper ścina ") + juce::String(plan.clipDb, 1) + pl(" dB szczytów (sufit ")
                            + juce::String(plan.clipper.ceilingDb, 1) + pl(" dBFS), z tego ") + juce::String(plan.loudMasterClipDb, 1)
                            + pl(" dB pod głośny master: jego uderzenia to najwyższe szczyty miksu, a limiter przy ")
                            + juce::String(masterTargetLufs(), 1) + pl(" LUFS ścina teraz ") + juce::String(previewStats().typicalReductionDb, 1)
                            + pl(" dB. Clipper zetnie je krócej i czyściej niż limiter, uderzenie zostanie");
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

        // What the plan does not need comes off too, when the plan knows enough to say so:
        // a compressor ticked by hand on percussion is not left squashing the hits.
        const bool intentional = channel.data.intentionalDrive != 0;
        if (plan.compressor.on < 0.5f && channel.data.compressor.on > 0.5f && plan.dynamicsKnown && !intentional)
        {
            item.tool = Proposal::Item::ChainTool::compressor;
            item.compressor = channel.data.compressor;
            item.compressor.on = 0.0f;
            item.text = name + pl("kompresor wyłączony: ")
                        + (plan.percussive ? pl("perkusji nie kompresuję, uderzenia mają zostać")
                           : plan.overCompressed ? pl("już ściśnięty u źródła")
                                                 : pl("dynamika w normie (") + juce::String(plan.spreadDb, 1) + pl(" dB, cel ")
                                                       + juce::String(plan.targetSpreadDb, 1) + " dB)");
            chainItems.push_back(item);
        }
        if (plan.clipper.on < 0.5f && channel.data.clipper.on > 0.5f && plan.dynamicsKnown && plan.peakKnown && !intentional)
        {
            item.tool = Proposal::Item::ChainTool::clipper;
            item.clipper = channel.data.clipper;
            item.clipper.on = 0.0f;
            item.text = name + pl("clipper wyłączony: szczyty w normie (") + juce::String(plan.crestDb, 1) + pl(" dB nad głośną częścią)");
            chainItems.push_back(item);
        }
        if (plan.evener.on < 0.5f && channel.data.evener.on > 0.5f && channel.data.role != static_cast<int>(ma::Role::bass))
        {
            item.tool = Proposal::Item::ChainTool::evener;
            item.evener = channel.data.evener;
            item.evener.on = 0.0f;
            item.text = name + pl("wyrównanie nut wyłączone: jest tylko dla basu");
            chainItems.push_back(item);
        }

        // Phase 31: the harmonic enhancer the plan chose for a near-sine bass.
        const auto& agentHarmonics = saved.channels[c].data.harmonics; // the mix above carries the planned one already
        if (plan.harmonics.on > 0.5f && !intentional
            && (agentHarmonics.on < 0.5f || std::abs(plan.harmonics.amountDb - agentHarmonics.amountDb) > 1.0f
                || std::abs(plan.harmonics.hz - agentHarmonics.hz) > 5.0f))
        {
            Proposal::Item harmonicsItem;
            harmonicsItem.action = Proposal::Item::Action::harmonics;
            harmonicsItem.owner = channel.owner;
            harmonicsItem.harmonics = plan.harmonics;
            harmonicsItem.text = name + harmonicsText(plan);
            chainItems.push_back(harmonicsItem);
        }

        // Phase 13: saturation, only on the roles the plan looks after; elsewhere (and with
        // "Celowy przester") the Agent's own setting stays.
        if (plan.saturationConsidered && !intentional)
        {
            const auto& want = plan.saturation;
            const auto& have = channel.data.saturation;
            const bool wantOn = want.on > 0.5f, haveOn = have.on > 0.5f;
            const bool same = wantOn == haveOn
                              && (!wantOn
                                  || (juce::roundToInt(want.mode) == juce::roundToInt(have.mode) && std::abs(want.driveDb - have.driveDb) < 0.5f
                                      && std::abs(want.mix - have.mix) < 0.03f));
            if (!same)
            {
                item.tool = Proposal::Item::ChainTool::saturation;
                item.saturation = wantOn ? want : have;
                item.saturation.on = wantOn ? 1.0f : 0.0f;
                item.text = name + saturationText(plan);
                chainItems.push_back(item);
            }
        }

        // Phase 32: dynamic bands and the rider where the plan looks after them; a setting the Brain
        // made and the plan no longer wants goes off, one the user made stays.
        if (plan.dynamicConsidered && !intentional)
        {
            const auto& have = channel.data.dynamicEq;
            const bool brainMade = (channel.data.brainTools & ma::bus::toolDynamicEq) != 0;
            const bool wantOn = ma::dynamicEqOn(plan.dynamicEq);
            if ((wantOn && !sameDynamicEq(plan.dynamicEq, have)) || (!wantOn && ma::dynamicEqOn(have) && brainMade))
            {
                item.tool = Proposal::Item::ChainTool::dynamicEq;
                item.dynamicEq = plan.dynamicEq;
                item.text = name + dynamicEqText(plan);
                chainItems.push_back(item);
            }
        }
    }
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        const auto& channel = mix.channels[c];
        const auto& stereoPlan = stereoPlans[c];
        // Phase 26: a channel on its width slider is the user's; the slider keeps it in phase itself.
        if (!channel.spectrumValid || !channel.stereoValid || ma::widthOn(channel.data.width))
            continue;
        // Parameters round what the plan sets (step and skew), so "the same" allows for that.
        const bool same = std::equal(stereoPlan.settings.side.begin(), stereoPlan.settings.side.end(), channel.data.stereo.side.begin(),
                                     [](const auto& a, const auto& b) {
                                         const bool offA = std::abs(a.db) < 0.05f, offB = std::abs(b.db) < 0.05f;
                                         return (offA && offB)
                                                || (!offA && !offB && std::abs(a.db - b.db) < 0.1f && std::abs(a.q - b.q) < 0.1f
                                                    && std::abs(std::log2(a.hz / b.hz)) < 0.02);
                                     });
        const auto& want = stereoPlan.settings.widen;
        const auto& have = channel.data.stereo.widen;
        const bool sameWiden = ma::widenOn(stereoPlan.settings) == ma::widenOn(channel.data.stereo)
                               && (!ma::widenOn(stereoPlan.settings)
                                   || (std::abs(want.amountDb - have.amountDb) < 0.6f && std::abs(std::log2(want.lowHz / have.lowHz)) < 0.05f
                                       && std::abs(want.delayMs - have.delayMs) < 0.2f));
        if ((same && sameWiden) || (stereoPlan.fixes.empty() && !ma::isOn(channel.data.stereo)))
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
    // Phase 33: the places that differ from where the channels sit now.
    for (const auto& channel : mix.channels)
    {
        const auto note = lastPanPlans.find(channel.owner);
        if (note == lastPanPlans.end() || !note->second.plan.considered || note->second.plan.reason == ma::PanPlan::Reason::user)
            continue;
        const auto& want = note->second.plan.settings;
        const auto& have = channel.data.pan;
        if (std::abs(want.position - have.position) < 0.025f
            && (!ma::panOn(want) || std::abs(want.monoBelowHz - have.monoBelowHz) < 2.5f))
            continue;
        Proposal::Item item;
        item.action = Proposal::Item::Action::chain;
        item.tool = Proposal::Item::ChainTool::pan;
        item.owner = channel.owner;
        item.pan = want;
        item.text = quoted(channel.name) + ": " + panText(note->second.plan, note->second.partner, note->second.inputBalanceDb);
        chainItems.push_back(item);
    }

    // Phase 34: depth. Sends darker than the dry parts, and out of the way while the main voice sings.
    const auto* voice = mainVoice();
    bool anySend = false;
    lastSendNotes.clear();
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        const auto& channel = mix.channels[c];
        const auto& plan = plans[c];
        if (!channel.spectrumValid || channel.data.role != static_cast<int>(ma::Role::send))
            continue;
        anySend = true;
        const auto name = quoted(channel.name);
        const bool brainMade = (channel.data.brainTools & ma::bus::toolDepth) != 0;
        if (plan.depthConsidered
            && ((ma::depthOn(plan.depth) && std::abs(plan.depth.highCutHz - channel.data.depth.highCutHz) > 50.0f)
                || (!ma::depthOn(plan.depth) && ma::depthOn(channel.data.depth) && brainMade)))
        {
            Proposal::Item item;
            item.action = Proposal::Item::Action::chain;
            item.tool = Proposal::Item::ChainTool::depth;
            item.owner = channel.owner;
            item.depth = plan.depth;
            item.text = name + ": " + depthText(plan);
            chainItems.push_back(item);
        }

        // Phase 36/37: what the send gives way to, and how.
        auto& note = lastSendNotes[channel.owner];
        note = {};
        note.sources = sendSources(channel.owner);
        const int mode = channel.data.sendDuckTo;
        note.mode = mode;
        const bool brainDuck = (channel.data.brainTools & ma::bus::toolDuck) != 0 && channel.data.duck.on > 0.5f;
        const auto turnOff = [&](const juce::String& why) {
            if (!brainDuck)
            {
                noteTool(channel.owner, ma::bus::toolDuck, ma::bus::ToolDecision::notNeeded, why);
                return;
            }
            Proposal::Item item;
            item.action = Proposal::Item::Action::duck;
            item.owner = channel.owner;
            item.duck = channel.data.duck;
            item.duck.on = 0.0f;
            item.duckSync = channel.data.duckSync;
            item.text = name + pl(": ustępowanie wyłączone, ") + why;
            chainItems.push_back(item);
        };
        if (channel.data.sidechainConnected != 0)
        {
            noteTool(channel.owner, ma::bus::toolDuck, ma::bus::ToolDecision::yours,
                     pl("sidechain podłączony kablem: ustępuje temu, co do niego wysłałeś"));
            continue;
        }
        if (mode == ma::bus::sendDuckNone)
        {
            turnOff(pl("bo w Agencie wysyłki wybrałeś „Nic”"));
            continue;
        }
        // The dominant source, when the fit knows it.
        const MixSnapshot::Channel* source = nullptr;
        if (note.sources.valid)
            for (size_t k = 0; k < note.sources.owners.size(); ++k)
                if (note.sources.owners[k] == note.sources.dominantOwner)
                    source = findChannel(note.sources.owners[k]);
        const bool sourceIsVoice = source != nullptr && voice != nullptr && source->owner == voice->owner;
        const auto sourceGroup = source != nullptr ? groupOfChannel(*source) : -1;
        // Who it gives way to: the voice (in its band) or its own source.
        enum class Target { voice, drums, music, none } target = Target::none;
        if (mode == ma::bus::sendDuckVoice)
            target = voice != nullptr ? Target::voice : Target::none;
        else if (mode == ma::bus::sendDuckSource)
            target = source == nullptr ? Target::none
                     : sourceIsVoice ? Target::voice
                     : sourceGroup == static_cast<int>(ma::StemGroup::drums) ? Target::drums
                                                                             : Target::music;
        else if (source != nullptr && sourceGroup == static_cast<int>(ma::StemGroup::drums))
            target = Target::drums; // a drum room: the hits stay dry, the tail blooms after them
        else if (voice != nullptr)
            target = Target::voice; // the vocal's, and the music's while someone sings over it
        else if (source != nullptr)
            target = Target::music;
        if (target == Target::none)
        {
            if (mode == ma::bus::sendDuckSource)
                noteTool(channel.owner, ma::bus::toolDuck, ma::bus::ToolDecision::notChecked,
                         note.sources.seconds < ma::kMinSourceSeconds
                             ? pl("jeszcze nie wiem, co zasila tę wysyłkę: puść dłuższy fragment utworu (")
                                   + juce::String(note.sources.seconds) + " / " + juce::String(ma::kMinSourceSeconds) + " s)"
                             : pl("nie rozpoznaję, co zasila tę wysyłkę (jej przebieg nie pasuje do żadnego kanału)"));
            else
                turnOff(pl("bo nie ma głównego wokalu ani leadu, któremu pogłos miałby ustępować, ani rozpoznanego źródła"));
            continue;
        }
        const auto* key = target == Target::voice ? voice : source;
        if (key == nullptr || key->data.keyId == 0 || channel.data.keyId == 0)
            continue;
        note.keyOwner = key->owner;
        ma::SendDuck duckPlan;
        bool whole = false;
        double attackMs = 10.0;
        int sync = 4; // 1/4: the room comes back between phrases, not between syllables
        juce::String how;
        if (target == Target::voice)
        {
            duckPlan = ma::planSendDuck(dropSpectrumDb(channel.owner), dropSpectrumDb(key->owner));
            how = pl(" (tylko w paśmie głosu, przestrzeń i dół zostają), gdy ten gra")
                  + (duckPlan.valid ? pl(" (ogon w 1–5 kHz ") + juce::String(duckPlan.tailVsVoiceDb, 1) + pl(" dB względem głosu, po ustąpieniu ")
                                      + juce::String(duckPlan.tailVsVoiceDb - duckPlan.depthDb, 1) + " dB)"
                                : pl(" (drop jeszcze niezmierzony, więc łagodnie)"));
        }
        else if (target == Target::drums)
        {
            // Quiet on the hit, the tail blooms right after it.
            duckPlan = ma::planSendDuck(dropSpectrumDb(channel.owner), dropSpectrumDb(key->owner), 200.0, 8000.0, 200.0, 8000.0,
                                    ma::kSendDuckSourceMaxDb);
            whole = true;
            attackMs = 2.0;
            sync = 2; // 1/16
            how = pl(" na każde uderzenie (cały pogłos, szybki powrót 1/16): uderzenie zostaje suche i krótkie, ogon rozkwita po nim");
        }
        else
        {
            duckPlan = ma::planSendDuck(dropSpectrumDb(channel.owner), dropSpectrumDb(key->owner), 300.0, 6000.0, 300.0, 6000.0,
                                    ma::kSendDuckSourceMaxDb);
            sync = 3; // 1/8
            how = pl(", gdy ten gra (w jego paśmie): nuty zostają czyste, ogon wypełnia przerwy");
        }
        const auto& have = channel.data.duck;
        const bool wantWhole = whole;
        if (have.on > 0.5f && (have.wholeSignal > 0.5f) == wantWhole && channel.data.duckKey == key->data.keyId
            && std::abs(have.depthDb - duckPlan.depthDb) < 1.0 && (wantWhole || std::abs(std::log2(have.hz / duckPlan.hz)) < 0.3))
        {
            noteTool(channel.owner, ma::bus::toolDuck, ma::bus::ToolDecision::kept,
                     pl("już ustępuje ") + quoted(key->name) + pl(" o ") + juce::String(have.depthDb, 1) + " dB" + how + "; "
                         + sendSourcesText(note.sources));
            continue;
        }
        Proposal::Item item;
        item.action = Proposal::Item::Action::duck;
        item.owner = channel.owner;
        item.maskedOwner = key->owner;
        auto& duck = item.duck;
        duck.on = 1.0f;
        duck.wholeSignal = whole ? 1.0f : 0.0f;
        duck.hz = static_cast<float>(duckPlan.hz);
        duck.q = static_cast<float>(duckPlan.q);
        duck.depthDb = static_cast<float>(duckPlan.depthDb);
        duck.thresholdDb = -30.0f;
        duck.attackMs = static_cast<float>(attackMs);
        duck.releaseMs = static_cast<float>(bpm > 0.0 ? ma::syncedReleaseMs(bpm, ma::bus::duckSyncFraction(sync)) : 60000.0 / 124.0 * 4.0 * ma::bus::duckSyncFraction(sync));
        item.duckSync = bpm > 0.0 ? sync : 0;
        item.duckKey = key->data.keyId;
        item.text = name + pl(": ustępuje ") + quoted(key->name) + pl(" o ") + juce::String(duck.depthDb, 1) + " dB"
                    + (whole ? juce::String() : pl(" wokół ") + hzText(duck.hz)) + how + pl("; powrót ")
                    + juce::String(juce::roundToInt(duck.releaseMs)) + pl(" ms; ") + sendSourcesText(note.sources)
                    + pl(". Bez kabla, przez wspólną pamięć");
        chainItems.push_back(item);
    }
    // The times for the user's own reverb and delay plugins.
    if ((anySend || voice != nullptr) && bpm > 0.0)
    {
        for (const auto& text : { reverbAdviceText(bpm), delayAdviceText(bpm) })
        {
            Proposal::Item item;
            item.advisory = true;
            item.selected = false;
            item.text = text + pl(". Ustaw to w swojej wtyczce");
            chainItems.push_back(item);
        }
    }

    // Phase 35: the bass's low end onto the kick's, in time (measured by the bass Agent with the kick on its sidechain).
    for (const auto& channel : mix.channels)
    {
        if (channel.data.role != static_cast<int>(ma::Role::bass))
            continue;
        const auto advice = alignAdvice(channel.data);
        const bool brainMade = (channel.data.brainTools & ma::bus::toolAlign) != 0;
        const auto& have = channel.data.align;
        ma::AlignSettings want = have;
        bool change = false;
        if (advice.kind == AlignAdvice::Kind::early)
        {
            want.delayMs = advice.delayMs;
            change = std::abs(want.delayMs - have.delayMs) >= kAlignMinLagMs; // within half a millisecond it stays
        }
        else if ((advice.kind == AlignAdvice::Kind::inTime || advice.kind == AlignAdvice::Kind::late)
                 && ma::alignOn(have) && brainMade)
        {
            want.delayMs = 0.0f;
            change = true;
        }
        if (change)
        {
            Proposal::Item item;
            item.action = Proposal::Item::Action::chain;
            item.tool = Proposal::Item::ChainTool::align;
            item.owner = channel.owner;
            item.align = want;
            item.text = quoted(channel.name) + ": "
                        + (ma::alignOn(want) ? alignText(advice) : pl("opóźnienie wyłączone, ") + alignText(advice));
            chainItems.push_back(item);
        }
        if (advice.kind == AlignAdvice::Kind::late)
        {
            Proposal::Item item;
            item.advisory = true;
            item.selected = false;
            item.owner = channel.owner;
            item.text = quoted(channel.name) + ": " + alignText(advice);
            chainItems.push_back(item);
        }
    }

    // Phase 9: each stem group's level against the references, with the planned cleanup.
    if (levelsPlanned)
    {
        const auto setName = typicalLevels ? pl("(brak referencji, więc typowe proporcje house'u)")
                                           : quoted(juce::String::fromUTF8(currentStems.name.c_str()));
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
            const auto& corridor = (typicalLevels ? levelTarget : keyedStems).level[static_cast<size_t>(g)];
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
                            + juce::String(gain, 1)
                            + (typicalLevels ? pl(" dB, razem z resztą względem perkusji (stopa zostaje tam, gdzie ustawiła ją nauka)")
                                             : pl(" dB, żeby cały miks został tak samo głośny po zmianie innych grup"));
            chainItems.insert(chainItems.begin(), item);
        }
    }
    // Each part back to the producer's balance inside its group.
    for (const auto& channel : mix.channels)
    {
        const auto balance = balanceGain.find(channel.owner);
        if (balance == balanceGain.end())
            continue;
        const int group = groupOfChannel(channel);
        Proposal::Item item;
        item.action = Proposal::Item::Action::balance;
        item.owner = channel.owner;
        item.gainDb = balance->second;
        item.text = quoted(channel.name) + ": " + (balance->second > 0.0 ? "+" : "") + juce::String(balance->second, 1)
                    + pl(" dB, wraca do proporcji ze śladu względem reszty grupy ") + stemGroupName(group)
                    + pl(" (nauka wyrównała każdą partię do celu roli, a producent ustawił je inaczej)");
        chainItems.push_back(item);
    }
    balanceGain.clear();
    mix = saved;

    currentProposal.items.insert(currentProposal.items.begin(), chainItems.begin(), chainItems.end());
    currentProposal.wholeMix = true;
    planCollisionSteps(); // on the real mix, with the chain items in
    lastVerdicts = currentProposal.verdicts;
    plannedOwners.clear();
    for (const auto& channel : mix.channels)
        if (channel.spectrumValid && inMixSums(channel))
            plannedOwners.push_back(channel.owner);
}

const MixSnapshot::Channel* BrainProcessor::mainVoice() const
{
    const auto loudness = [](const MixSnapshot::Channel& c) {
        double p = 0.0;
        for (int b = 0; b < ma::bus::kBands; ++b)
        {
            const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
            if (hz >= 300.0 && hz <= 5000.0)
                p += std::pow(10.0, c.heardDb[static_cast<size_t>(b)] / 10.0);
        }
        return p;
    };
    for (const auto role : { ma::Role::vocal, ma::Role::lead })
    {
        const MixSnapshot::Channel* found = nullptr;
        for (const auto& channel : mix.channels)
            if (channel.spectrumValid && channel.data.role == static_cast<int>(role) && (found == nullptr || loudness(channel) > loudness(*found)))
                found = &channel;
        if (found != nullptr)
            return found;
    }
    return nullptr;
}

const MixSnapshot::Channel* BrainProcessor::kickChannel() const
{
    // The kick with the clearest tail, if there are several kick channels.
    const MixSnapshot::Channel* found = nullptr;
    for (const auto& channel : mix.channels)
        if (channel.data.role == static_cast<int>(ma::Role::kick)
            && (found == nullptr || (found->data.tailPitchHz <= 0.0f && channel.data.tailPitchHz > 0.0f)))
            found = &channel;
    return found;
}

ma::KickTuning BrainProcessor::kickTuning() const
{
    const auto* kick = kickChannel();
    if (kick == nullptr || kick->data.tailPitchHz <= 0.0f)
        return {};
    double bassHz = 0.0;
    for (const auto& channel : mix.channels)
        if (channel.data.role == static_cast<int>(ma::Role::bass) && channel.data.lowNoteHz > 0.0f)
        {
            bassHz = channel.data.lowNoteHz;
            break;
        }
    return ma::analyseKickTuning(kick->data.tailPitchHz, trackKey(), bassHz);
}

juce::String BrainProcessor::kickNoteText(double hz)
{
    const double exact = 69.0 + 12.0 * std::log2(hz / 440.0);
    const int note = static_cast<int>(std::lround(exact));
    const int cents = static_cast<int>(std::lround((exact - note) * 100.0));
    return juce::String(ma::noteNameOf(note)) + " (" + juce::String(hz, 1) + " Hz"
           + (std::abs(cents) >= 5 ? (cents > 0 ? ", +" : ", ") + juce::String(cents) + pl(" centów") : juce::String()) + ")";
}

juce::String BrainProcessor::kickTuningText(const ma::KickTuning& t) const
{
    static const char* const intervals[] = { "pryma (oktawa)", "mała sekunda", "wielka sekunda", "tercja mała", "tercja wielka", "kwarta",
                                             "tryton", "kwinta", "seksta mała", "seksta wielka", "septyma mała", "septyma wielka" };
    const auto key = juce::String(ma::keyName(t.key));
    auto text = pl("Stopa brzmi na ") + kickNoteText(t.kickHz) + pl(", tonacja ") + key + ": ";
    switch (t.fit)
    {
        case ma::KickTuning::Fit::tonic: text += pl("to tonika"); break;
        case ma::KickTuning::Fit::fifth: text += pl("to kwinta"); break;
        case ma::KickTuning::Fit::inScale: text += pl("w skali, ale nie na tonice ani kwincie"); break;
        case ma::KickTuning::Fit::outOfScale: text += pl("ta nuta nie należy do skali"); break;
    }
    if (t.bassKnown)
    {
        const auto bassNote = juce::String(ma::noteNameOf(static_cast<int>(std::lround(69.0 + 12.0 * std::log2(t.bassHz / 440.0)))));
        text += pl(". Z basem (zwykle ") + bassNote + pl(") to ") + pl(intervals[t.bassInterval])
                + (t.clashesWithBass ? pl(", więc na każdym uderzeniu dół dudni") : juce::String());
    }
    if (!t.needsTuning && !t.couldBeBetter)
        return text + pl(". Strój w porządku.");

    auto plural = [](int n) {
        n = std::abs(n);
        return n == 1 ? pl(" półton") : (n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 10 || n % 100 >= 20)) ? pl(" półtony") : pl(" półtonów");
    };
    const int fine = static_cast<int>(std::lround(t.shiftCents - 100.0 * t.shiftSemitones));
    juce::StringArray move;
    if (t.shiftSemitones != 0)
        move.add((t.shiftSemitones > 0 ? "+" : "") + juce::String(t.shiftSemitones) + plural(t.shiftSemitones));
    if (std::abs(fine) >= 5)
        move.add((fine > 0 ? "+" : "") + juce::String(fine) + pl(" centów"));
    text += (t.needsTuning ? pl(". Przestrój sampel stopy o ") : pl(". Można lepiej: przestrojenie o "))
            + move.joinIntoString(pl(" i ")) + pl(" da ") + juce::String(ma::noteNameOf(t.targetMidiNote)) + " ("
            + juce::String(t.targetHz, 1) + " Hz, "
            + ((((t.targetMidiNote % 12) + 12) % 12 - t.key.tonic + 12) % 12 == 0 ? pl("tonika") : pl("kwinta"))
            + pl("). W FL: „Pitch” albo „Fine tune” w kanale samplera stopy");
    return text;
}

juce::String BrainProcessor::glueText(const ma::ChainPlan& plan, double bpm)
{
    using Content = ma::ChainPlan::BusContent;
    const auto& comp = plan.compressor;
    const auto content = plan.busContent == Content::drums ? pl("perkusji") : plan.busContent == Content::vocals ? pl("wokali") : pl("muzyki");
    return pl("klej busa ") + content + ": " + juce::String(comp.ratio, 1) + pl(":1 od ") + juce::String(comp.thresholdDb, 1)
           + pl(" dB, atak ") + juce::String(juce::roundToInt(comp.attackMs)) + pl(" ms (przepuszcza początek uderzeń), powrót ")
           + juce::String(juce::roundToInt(comp.releaseMs)) + " ms" + (bpm > 0.0 ? pl(" pod tempo") : juce::String())
           + pl(", ok. ") + juce::String(plan.glueReductionDb, 1) + pl(" dB redukcji w głośnych miejscach (cel ")
           + juce::String(plan.glueTargetDb, 1) + pl(" dB), makeup +") + juce::String(comp.makeupDb, 1)
           + pl(" dB. Źródła grają razem jak jedna partia");
}

juce::String BrainProcessor::saturationText(const ma::ChainPlan& plan) const
{
    static const char* const modes[] = { "taśma", "lampa", "miękki clip", "twardy clip", "fałdowanie" };
    const auto& sat = plan.saturation;
    const auto cover = plan.saturationWouldCover >= 0 && static_cast<size_t>(plan.saturationWouldCover) < mix.channels.size()
                           ? quoted(mix.channels[static_cast<size_t>(plan.saturationWouldCover)].name)
                           : juce::String();
    if (sat.on < 0.5f && plan.harmonics.on > 0.5f)
        return pl("saturacja wyłączona: sub to prawie czysty sinus, alikwoty daje enhancer harmonicznych, a sub zostaje czysty");
    if (sat.on < 0.5f)
        return cover.isNotEmpty() ? pl("saturacja wyłączona: jej alikwoty zasłoniłyby ") + cover
                                  : pl("saturacja wyłączona: alikwotów dość (") + juce::String(plan.overtonesDb, 1)
                                        + (plan.overtonesAboveSub ? pl(" dB powyżej 150 Hz względem suba, cel ") : pl(" dB pod podstawą, cel "))
                                        + juce::String(plan.overtonesTargetDb, 0) + " dB)";
    auto text = pl("saturacja ") + pl(modes[juce::jlimit(0, 4, juce::roundToInt(sat.mode))]) + pl(", drive +")
                + juce::String(sat.driveDb, 0) + pl(" dB, miks ") + juce::String(juce::roundToInt(sat.mix * 100.0f))
                + (plan.overtonesAboveSub ? pl("%: treść powyżej 150 Hz ") + juce::String(plan.overtonesDb, 1) + pl(" dB pod subem")
                                          : pl("%: alikwoty (2.–8. harmoniczna) ") + juce::String(plan.overtonesDb, 1) + pl(" dB pod podstawą ")
                                                + hzText(plan.fundamentalHz))
                + pl(", po saturacji ok. ") + juce::String(plan.overtonesAfterDb, 1) + pl(" dB (cel ")
                + juce::String(plan.overtonesTargetDb, 0) + pl(" dB). Gęstszy i słyszalny na małych głośnikach");
    if (cover.isNotEmpty())
        text += pl(". Łagodniej, niż by chciał, żeby nie zasłonić ") + cover;
    return text;
}

juce::String BrainProcessor::harmonicsText(const ma::ChainPlan& plan) const
{
    const auto& h = plan.harmonics;
    auto text = pl("harmoniczne z suba poniżej ") + hzText(h.hz) + ", " + juce::String(h.amountDb, 0) + pl(" dB: ")
                + (plan.sineSub ? pl("bas to prawie czysty sinus (treść powyżej 150 Hz ") : pl("treść powyżej 150 Hz "))
                + juce::String(plan.overtonesDb, 1) + pl(" dB pod subem), po nich ok. ") + juce::String(plan.harmonicsAfterDb, 1)
                + pl(" dB (cel ") + juce::String(plan.overtonesTargetDb, 0)
                + pl(" dB). Słychać go na małych głośnikach, a sub zostaje czysty, bez przesteru");
    if (!plan.sineSub)
        text += pl(" (saturacja zasłoniłaby ważniejszy kanał)");
    if (plan.harmonicsWouldCover >= 0 && static_cast<size_t>(plan.harmonicsWouldCover) < mix.channels.size())
        text += pl(". Łagodniej, niż by chciał, żeby nie zasłonić ") + quoted(mix.channels[static_cast<size_t>(plan.harmonicsWouldCover)].name);
    return text;
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

    if (!inMixSums(channel))
        verdict.reasons.add(pl("Bus nie liczy się w kolizjach, translacji ani korekcjach EQ: jego źródła mają własnych Agentów "
                               "i tam Mózg je ustawia. Tu dostaje tylko klej."));

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
    else if (plan.glue)
    {
        verdict.reasons.add(pl("Dynamika: ") + glueText(plan, tempoBpm()) + ".");
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
                            + juce::String(plan.clipDb, 1) + " dB"
                            + (plan.loudMasterClipDb > 0.0 ? pl(", z tego ") + juce::String(plan.loudMasterClipDb, 1)
                                                                 + pl(" dB pod głośny master (to on daje najwyższe szczyty miksu)")
                                                           : juce::String())
                            + (plan.loudMasterCapped ? pl("; więcej clippera nie dam, bo uderzenie by się spłaszczyło") : juce::String()) + ".");
    else if (plan.dynamicsKnown && plan.peakKnown && plan.crestDb > 0.0 && channel.data.intentionalDrive == 0)
        verdict.reasons.add(pl("Szczyty w normie (") + juce::String(plan.crestDb, 1) + pl(" dB nad głośną częścią)."));
    else if (plan.dynamicsKnown && !plan.peakKnown && channel.data.intentionalDrive == 0)
        verdict.reasons.add(pl("Szczyty nieznane (projekt zapisany przed fazą 23): kliknij „Naucz poziom” w Agencie, żeby Mózg ocenił clipper."));

    if (channel.stereoValid && ma::widthOn(channel.data.width))
        verdict.reasons.add(pl("Stereo: szerokość ustawiona suwakiem w Agencie (") + juce::String(juce::roundToInt(channel.data.widthRequest))
                            + pl("%), Mózg jej nie zmienia. Najsłabsze pasmo ma korelację ") + juce::String(channel.data.widthWorstCoherence, 2)
                            + (channel.data.widthLimitedBySum != 0 ? pl("; suma miksu ograniczyła poszerzenie.") : pl(".")));
    else if (!channel.stereoValid)
        verdict.reasons.add(pl("Stereo: brak pomiaru (kanał mono albo Agent jeszcze nie wysłał danych)."));
    else if (stereoPlan.fixes.empty())
        verdict.reasons.add(pl("Stereo: każde pasmo w fazie, nic do zmiany."));
    else
        for (const auto& fix : stereoPlan.fixes)
            verdict.reasons.add(pl("Stereo: ") + stereoFixText(fix) + ".");

    if (plan.saturation.on > 0.5f)
        verdict.reasons.add(pl("Saturacja: ") + saturationText(plan).fromFirstOccurrenceOf("saturacja ", false, false) + ".");
    else if (plan.saturationConsidered && channel.data.intentionalDrive == 0)
        verdict.reasons.add(pl("Saturacja: ") + saturationText(plan).fromFirstOccurrenceOf(": ", false, false) + ".");

    if (kickChannel() == &channel)
    {
        const auto tuning = kickTuning();
        if (tuning.valid)
        {
            verdict.reasons.add(pl("Strój: ") + kickTuningText(tuning) + ".");
            if (tuning.needsTuning)
                attention.add(pl("rozstrojona względem tonacji"));
        }
    }

    if (plan.evener.on > 0.5f)
        verdict.reasons.add(pl("Linia basu wokół ") + noteName(plan.evener.referenceHz) + pl(": niskie nuty wyrównane dla ucha."));
    if (ma::dynamicEqOn(plan.dynamicEq))
        verdict.reasons.add(pl("Dynamiczne EQ: ") + dynamicEqText(plan).fromFirstOccurrenceOf(": ", false, false) + ".");
    if (plan.depthConsidered && ma::depthOn(plan.depth))
        verdict.reasons.add(pl("Głębia: ") + depthText(plan) + ".");
    if (const auto advice = alignAdvice(channel.data); advice.kind != AlignAdvice::Kind::notBass)
    {
        verdict.reasons.add(pl("Czas ze stopą: ") + alignText(advice) + ".");
        if (advice.kind == AlignAdvice::Kind::late)
            attention.add(pl("dół spóźniony względem stopy"));
    }
    if (const auto pan = lastPanPlans.find(channel.owner);
        pan != lastPanPlans.end() && pan->second.plan.considered && ma::panOn(pan->second.plan.settings))
        verdict.reasons.add(pl("Panorama: ") + panText(pan->second.plan, pan->second.partner, pan->second.inputBalanceDb)
                                                   .fromFirstOccurrenceOf(pl("panorama "), false, false)
                            + ".");

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
        // Phase 30: the same story as the Kolizje tab.
        juce::String how;
        if (fixed || handling == IssueHandling::pending)
            how = pl(": poprawka jest w propozycji.");
        else
            switch (handling)
            {
                case IssueHandling::ducked: how = pl(": ustępuje mu na każde uderzenie."); break;
                case IssueHandling::unmasked: how = pl(": ustępuje mu dynamicznie, gdy gra (bez kabla), w średnim widmie tego nie widać."); break;
                case IssueHandling::cutAtLimit:
                    how = pl(": cięcie już na limicie; zniknie, gdy ") + quoted(masker.name) + pl(" będzie ciszej o ")
                          + juce::String(liveLevelStep(issue), 1) + pl(" dB („Analizuj” doda to do propozycji).");
                    break;
                case IssueHandling::levelStep: how = pl(": EQ zrobił, ile mógł, reszta to poziom ") + quoted(masker.name) + pl(" (decyzja Twoja)."); break;
                case IssueHandling::settled:
                    how = pl(": miks zrobił, ile mógł. ") + settledAdvice(issue);
                    break;
                case IssueHandling::open:
                case IssueHandling::pending: how = pl(": kliknij „Analizuj”."); break;
            }
        verdict.reasons.add(pl("Zasłania go ") + quoted(masker.name) + pl(" w ") + hzText(issue.lowHz) + pl("–") + hzText(issue.highHz)
                            + " (" + juce::String(juce::roundToInt(issue.score * 100.0)) + "%)" + how);
        if (!fixed && handling != IssueHandling::ducked && handling != IssueHandling::unmasked && handling != IssueHandling::pending
            && issue.score >= 0.5)
            attention.add(handling == IssueHandling::settled ? pl("ginie pod ") + quoted(masker.name) + pl(" (aranż)")
                                                             : pl("ginie pod ") + quoted(masker.name));
    }
    // Phase 30: whom it gives way to (the proposal's dips, or those it already runs).
    {
        juce::StringArray keys;
        for (const auto& item : currentProposal.items)
            if (item.action == Proposal::Item::Action::unmask && item.owner == channel.owner)
            {
                const auto other = std::find_if(mix.channels.begin(), mix.channels.end(), [&](const auto& c) { return c.owner == item.maskedOwner; });
                verdict.reasons.add(pl("Ustępuje ") + (other != mix.channels.end() ? quoted(other->name) : pl("innemu kanałowi")) + pl(" wokół ")
                                    + hzText(item.unmask.hz) + pl(" do ") + juce::String(item.unmask.depthDb, 0)
                                    + pl(" dB, tylko gdy tamten gra (bez kabla)") + (item.partial ? pl("; kolizja będzie dużo słabsza.") : "."));
                keys.add(juce::String::toHexString(static_cast<juce::int64>(item.unmaskKey)));
            }
        for (int d = 0; d < ma::kMaxUnmask; ++d)
        {
            if (!ma::unmaskOn(channel.data.unmask[d])
                || keys.contains(juce::String::toHexString(static_cast<juce::int64>(channel.data.unmaskKey[d]))))
                continue;
            const auto other = std::find_if(mix.channels.begin(), mix.channels.end(),
                                            [&](const auto& c) { return c.data.keyId == channel.data.unmaskKey[d]; });
            verdict.reasons.add(pl("Ustępuje ") + (other != mix.channels.end() ? quoted(other->name) : pl("kanałowi, którego Agent nie działa"))
                                + pl(" wokół ") + hzText(channel.data.unmask[d].hz) + pl(" do ") + juce::String(channel.data.unmask[d].depthDb, 0)
                                + pl(" dB, gdy tamten gra (bez kabla)."));
        }
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
        verdict.summary = pl("Z tym planem ustawiony poprawnie w kontekście miksu.");

    // Phase 23: which lines are about a tool the Brain sets, so the view can say where each stands.
    for (const auto& reason : verdict.reasons)
    {
        int tool = 0;
        if (reason.startsWith(pl("Porządki: ")))
            tool = ma::bus::toolCleanup;
        else if ((reason.startsWith(pl("Dynamika: ")) || reason.startsWith(pl("Ciche fragmenty"))) && plan.compressor.on > 0.5f)
            tool = ma::bus::toolCompressor;
        else if (reason.startsWith(pl("Szczyty: ")) && plan.clipper.on > 0.5f)
            tool = ma::bus::toolClipper;
        else if (reason.startsWith(pl("Linia basu")) && plan.evener.on > 0.5f)
            tool = ma::bus::toolEvener;
        else if (reason.startsWith(pl("Stereo: side")) || reason.startsWith(pl("Stereo: poszerzenie")))
            tool = ma::bus::toolStereo;
        else if (reason.startsWith(pl("Saturacja: ")) && plan.saturation.on > 0.5f)
            tool = ma::bus::toolSaturation;
        else if (reason.startsWith(pl("Ustępuje ")))
            tool = ma::bus::toolUnmask;
        else if (reason.startsWith(pl("Dynamiczne EQ: ")))
            tool = ma::bus::toolDynamicEq;
        else if (reason.startsWith(pl("Panorama: ")))
            tool = ma::bus::toolPan;
        else if (reason.startsWith(pl("Głębia: ")))
            tool = ma::bus::toolDepth;
        else if (reason.startsWith(pl("Czas ze stopą: ")))
            tool = ma::bus::toolAlign;
        verdict.reasonTools.push_back(tool);
    }
    return verdict;
}

// Phase 31: the decision per tool.
namespace
{
int toolIndex(int bit)
{
    for (int i = 0; i < ma::bus::kToolCount; ++i)
        if (bit == (1 << i))
            return i;
    return -1;
}

// UTF-8 copy that never cuts a character in half.
void copyNote(char* out, int size, const juce::String& text)
{
    const char* utf8 = text.toRawUTF8();
    int length = static_cast<int>(std::strlen(utf8));
    if (length > size - 1)
    {
        length = size - 1;
        while (length > 0 && (static_cast<unsigned char>(utf8[length]) & 0xC0) == 0x80)
            --length;
    }
    std::memcpy(out, utf8, static_cast<size_t>(length));
    out[length] = '\0';
}
} // namespace

juce::String BrainProcessor::toolName(int tool)
{
    const auto names = ma::ui::toolNames();
    return tool >= 0 && tool < names.size() ? names[tool] : juce::String();
}

juce::String BrainProcessor::decisionText(ma::bus::ToolDecision decision)
{
    const auto names = ma::ui::decisionNames();
    const int index = static_cast<int>(decision);
    return index > 0 && index < names.size() ? names[index] : juce::String();
}

void BrainProcessor::noteTool(std::uint64_t owner, int toolBit, ma::bus::ToolDecision decision, const juce::String& text)
{
    const int index = toolIndex(toolBit);
    if (index < 0 || owner == 0)
        return;
    auto& note = toolWhy[owner][static_cast<size_t>(index)];
    note.decision = decision;
    note.text = text;
}

void BrainProcessor::analyse(int scope)
{
    if (scope == 1)
        proposeWholeMix();
    else
    {
        lastPlans.clear();
        lastStereoPlans.clear();
        propose();
    }
    buildToolNotes(scope == 1);
    addToolLinesToVerdicts();
    publishToolNotes();
}

void BrainProcessor::buildToolNotes(bool wholeMix)
{
    using D = ma::bus::ToolDecision;
    using Action = Proposal::Item::Action;
    using Tool = Proposal::Item::ChainTool;
    lastToolNotes.clear();
    lastNotesMs = juce::Time::currentTimeMillis();
    lastNotesWholeMix = wholeMix;
    const bool haveKick = std::any_of(mix.channels.begin(), mix.channels.end(),
                                      [](const auto& c) { return c.data.role == static_cast<int>(ma::Role::kick) && c.spectrumValid; });

    for (const auto& channel : mix.channels)
    {
        auto notes = toolWhy.count(channel.owner) > 0 ? toolWhy[channel.owner] : ToolNoteSet {};
        const auto set = [&](int bit, D decision, const juce::String& text) {
            auto& note = notes[static_cast<size_t>(toolIndex(bit))];
            note.decision = decision;
            note.text = text;
        };
        const auto has = [&](int bit) { return notes[static_cast<size_t>(toolIndex(bit))].decision != D::none; };
        // The proposal's own line for a tool, without the channel's name in front.
        const auto prefix = quoted(channel.name) + ": ";
        const auto strip = [&](const juce::String& text) { return text.startsWith(prefix) ? text.substring(prefix.length()) : text; };
        const auto itemFor = [&](auto&& match) -> const Proposal::Item* {
            for (const auto& item : currentProposal.items)
                if (item.owner == channel.owner && !item.advisory && match(item))
                    return &item;
            return nullptr;
        };
        const auto chainItem = [&](Tool tool) {
            return itemFor([&](const auto& i) { return i.action == Action::chain && i.tool == tool; });
        };
        const auto role = static_cast<ma::Role>(juce::jlimit(0, static_cast<int>(ma::Role::send), static_cast<int>(channel.data.role)));
        const bool bus = !inMixSums(channel);
        const bool percussive = role == ma::Role::kick || role == ma::Role::percussion;
        const bool intentional = channel.data.intentionalDrive != 0;
        // A tool switched on: by the Brain (kept unless the analysis changes it) or by hand (left alone).
        const auto runningNote = [&](int bit, bool on, const juce::String& what) {
            if (!on)
                return false;
            if ((channel.data.brainTools & bit) != 0)
                set(bit, D::kept, pl("działa od ostatniego „Zastosuj”") + (what.isNotEmpty() ? pl("; ") + what : juce::String()));
            else if ((channel.data.changedTools & bit) != 0)
                set(bit, D::yours, pl("Mózg to ustawił, potem zmieniono ręcznie; zostaje Twoje"));
            else
                set(bit, D::yours, pl("włączone ręcznie, Mózg tego nie zmienia"));
            return true;
        };

        if (!channel.spectrumValid)
        {
            for (int t = 0; t < ma::bus::kToolCount; ++t)
                notes[static_cast<size_t>(t)] = { D::notChecked, pl("kanał nie grał przy „Analizuj”: puść fragment, w którym gra (najlepiej drop), i analizuj jeszcze raz") };
            lastToolNotes[channel.owner] = notes;
            continue;
        }

        // EQ: the solver's cuts and boosts, the translation's room and presence bells.
        {
            int changes = 0, inPlace = 0;
            const Proposal::Item* first = nullptr;
            for (const auto& item : currentProposal.items)
                if (item.owner == channel.owner && !item.advisory && item.action == Action::solver)
                {
                    if (item.inPlace)
                        ++inPlace;
                    else if (++changes == 1)
                        first = &item;
                }
            const bool eqBands = channel.data.eqBypass == 0
                                 && std::any_of(std::begin(channel.data.eq), std::end(channel.data.eq), [](const auto& b) { return ma::isOn(b); });
            if (changes > 0)
                set(ma::bus::toolEq, D::proposed,
                    (changes == 1 ? strip(first->text)
                                  : juce::String(changes) + (changes < 5 ? pl(" zmiany EQ, m.in. ") : pl(" zmian EQ, m.in. ")) + strip(first->text)));
            else if (bus)
                set(ma::bus::toolEq, D::notNeeded, pl("bus: korekcje ustawiam na jego źródłach, które mają własnych Agentów"));
            else if (inPlace > 0 || eqBands)
                set(ma::bus::toolEq, D::kept, pl("korekcja z poprzedniego „Zastosuj” nadal trafia w cel, nic do zmiany"));
            else
            {
                const bool masks = std::any_of(mix.masking.issues.begin(), mix.masking.issues.end(), [&](const auto& issue) {
                    return issue.score >= 0.3 && mix.channels[static_cast<size_t>(issue.masker)].owner == channel.owner;
                });
                auto why = masks ? pl("jego kolizje biorą na siebie inne narzędzia (ustępowanie, poziom grup) albo inne kanały")
                                 : pl("nie zasłania innych kanałów");
                why += pl(", a balans tonalny nie potrzebuje jego pasm");
                set(ma::bus::toolEq, D::notNeeded, why);
            }
        }

        // The chain: planned by "Analizuj" on the whole mix only.
        const auto plan = lastPlans.find(channel.owner);
        const auto notInScope = pl("sprawdzam przy „Analizuj” w zakresie „Cały miks”; teraz analiza tylko korekcji");
        if (!wholeMix || plan == lastPlans.end())
        {
            for (const int bit : { ma::bus::toolCleanup, ma::bus::toolCompressor, ma::bus::toolClipper, ma::bus::toolEvener,
                                   ma::bus::toolSaturation, ma::bus::toolStereo, ma::bus::toolPan, ma::bus::toolDepth,
                                   ma::bus::toolAlign })
                if (!has(bit))
                    set(bit, D::notChecked, notInScope);
        }
        else
        {
            const auto& p = plan->second;
            const bool cleanupOn = p.cleanup.hpfHz > 0.0f || p.cleanup.monoBelowHz > 0.0f || std::abs(p.cleanup.mudDb) > 0.05f
                                   || std::any_of(p.cleanup.notches.begin(), p.cleanup.notches.end(), [](const auto& n) { return std::abs(n.db) > 0.05f; });
            if (const auto* item = chainItem(Tool::cleanup))
                set(ma::bus::toolCleanup, D::proposed, strip(item->text));
            else if (cleanupOn)
                set(ma::bus::toolCleanup, D::kept, cleanupText(p.cleanup, p.highPassReason, p.mudCrowd, &p));
            else
                set(ma::bus::toolCleanup, D::notNeeded,
                    pl("dół i środek czyste, nic do sprzątania")
                        + (p.resonancesBuried > 0 ? pl("; rezonanse są, ale miks je zakrywa, więc zostają") : juce::String()));

            // Dynamics.
            const auto unknownDynamics = pl("dynamika nieznana: kliknij „Naucz poziom” w Agencie na najgęstszym fragmencie");
            if (const auto* item = chainItem(Tool::compressor))
                set(ma::bus::toolCompressor, D::proposed, strip(item->text));
            else if (intentional)
                set(ma::bus::toolCompressor, D::yours, pl("„Celowy przester”: dynamikę zostawiam tak, jak jest"));
            else if (!p.dynamicsKnown)
                set(ma::bus::toolCompressor, D::notChecked, unknownDynamics);
            else if (p.compressor.on > 0.5f)
                set(ma::bus::toolCompressor, D::kept, p.glue ? glueText(p, tempoBpm())
                                                             : pl("dynamika ") + juce::String(p.spreadDb, 1) + pl(" dB, z nim ok. ")
                                                                   + juce::String(p.spreadAfterDb, 1) + pl(" dB (cel ") + juce::String(p.targetSpreadDb, 1) + " dB)");
            else if (p.overCompressed)
                set(ma::bus::toolCompressor, D::notNeeded, pl("już mocno ściśnięty przed Agentem (szczyt ") + juce::String(p.crestDb, 1)
                                                               + pl(" dB nad głośną częścią), więcej kompresji by go spłaszczyło"));
            else if (p.percussive)
                set(ma::bus::toolCompressor, D::notNeeded, pl("perkusji nie kompresuję: uderzenia mają zostać, szczyty bierze clipper"));
            else if (bus)
                set(ma::bus::toolCompressor, D::notNeeded, pl("źródła tego busa przychodzą już ściśnięte, klej by ich nie poprawił"));
            else
                set(ma::bus::toolCompressor, D::notNeeded, pl("dynamika w normie: ") + juce::String(p.spreadDb, 1) + pl(" dB między głośnymi i cichymi fragmentami (cel ")
                                                               + juce::String(p.targetSpreadDb, 1) + " dB)");

            if (const auto* item = chainItem(Tool::clipper))
                set(ma::bus::toolClipper, D::proposed, strip(item->text));
            else if (intentional)
                set(ma::bus::toolClipper, D::yours, pl("„Celowy przester”: szczyty zostawiam tak, jak są"));
            else if (!p.dynamicsKnown)
                set(ma::bus::toolClipper, D::notChecked, unknownDynamics);
            else if (!p.peakKnown)
                set(ma::bus::toolClipper, D::notChecked, pl("szczyty nieznane: kliknij „Naucz poziom” w Agencie"));
            else if (p.clipper.on > 0.5f)
                set(ma::bus::toolClipper, D::kept, pl("ścina ok. ") + juce::String(p.clipDb, 1) + pl(" dB szczytów, jak trzeba"));
            else if (p.overCompressed)
                set(ma::bus::toolClipper, D::notNeeded, pl("już ściśnięty u źródła, nie ma czego ścinać"));
            else
                set(ma::bus::toolClipper, D::notNeeded, pl("szczyty w normie: ") + juce::String(p.crestDb, 1) + pl(" dB nad głośną częścią, wystarczy ")
                                                            + juce::String(p.maxCrestDb, 0) + pl(" dB")
                                                            + (percussive && loudMasterPlan.made ? pl("; głośny master też nie potrzebuje więcej") : juce::String()));

            if (const auto* item = chainItem(Tool::evener))
                set(ma::bus::toolEvener, D::proposed, strip(item->text));
            else if (role != ma::Role::bass)
                set(ma::bus::toolEvener, D::notNeeded, pl("wyrównuję tylko linię basu (niskie nuty ucho słyszy ciszej)"));
            else if (p.evener.on > 0.5f)
                set(ma::bus::toolEvener, D::kept, pl("linia wokół ") + noteName(p.evener.referenceHz) + pl(" wyrównana"));
            else
                set(ma::bus::toolEvener, D::notChecked, pl("nie znam zwykłej nuty basu: kliknij „Naucz poziom” w Agencie, gdy gra linia basu"));

            if (const auto* item = chainItem(Tool::saturation))
                set(ma::bus::toolSaturation, D::proposed, strip(item->text));
            else if (intentional)
                set(ma::bus::toolSaturation, D::yours, pl("„Celowy przester”: Twoja saturacja zostaje"));
            else if (!p.saturationConsidered)
            {
                const auto why = percussive ? pl("stopa i perkusja: brzmienie trzymają clipper i atak, saturacja zmiękczyłaby uderzenie")
                                 : role == ma::Role::effects || role == ma::Role::send || bus
                                     ? pl("efekty, wysyłki i busy to Twoje brzmienie, ich barwy nie ruszam")
                                     : pl("nie da się zmierzyć podstawy dźwięku (za mało treści), więc nie oceniam alikwotów");
                if (!runningNote(ma::bus::toolSaturation, channel.data.saturation.on > 0.5f, why))
                    set(ma::bus::toolSaturation, D::notNeeded, why);
            }
            else if (p.saturation.on > 0.5f)
                set(ma::bus::toolSaturation, D::kept, saturationText(p));
            else
                set(ma::bus::toolSaturation, D::notNeeded, saturationText(p).fromFirstOccurrenceOf(": ", false, false));

            // Phase 32: dynamic bands and the rider.
            if (const auto* item = chainItem(Tool::dynamicEq))
                set(ma::bus::toolDynamicEq, D::proposed, strip(item->text));
            else if (intentional)
                set(ma::bus::toolDynamicEq, D::yours, pl("„Celowy przester”: brzmienie zostawiam tak, jak jest"));
            else if (!p.dynamicConsidered)
            {
                const auto why = percussive ? pl("stopa i perkusja: uderzenia mają wystawać, dynamiczne EQ by je przytępiło")
                                 : role == ma::Role::bass ? pl("bas: dół trzymają kompresor i wyrównanie nut, nie dynamiczne pasma")
                                 : role == ma::Role::effects || role == ma::Role::send || bus
                                     ? pl("efekty, wysyłki i busy to Twoje brzmienie, ich nie ruszam")
                                     : pl("brak pomiaru chwilowych pasm: kliknij „Naucz poziom” w Agencie, gdy partia gra");
                set(ma::bus::toolDynamicEq, role == ma::Role::vocal || role == ma::Role::lead || role == ma::Role::pad || role == ma::Role::other
                                                ? D::notChecked : D::notNeeded, why);
            }
            else if (ma::dynamicEqOn(p.dynamicEq))
                set(ma::bus::toolDynamicEq, D::kept, dynamicEqText(p).fromFirstOccurrenceOf(": ", false, false));
            else
            {
                juce::StringArray why;
                for (int k = 0; k < ma::kDynamicBands; ++k)
                    if (p.probeBuried[static_cast<size_t>(k)])
                        why.add(dynamicBandName(k).trim() + pl(" wystaje chwilami, ale miks zakrywa ten zakres"));
                why.add(role == ma::Role::vocal ? pl("sybilanty, ostrość i dudnienie nie wystają chwilami ponad zwykły poziom; frazy są równe")
                                                : pl("ostrość nie wystaje chwilami ponad zwykły poziom (sybilanty i rider są dla wokalu)"));
                set(ma::bus::toolDynamicEq, D::notNeeded, why.joinIntoString("; "));
            }

            // Phase 31: the enhancer on a thin bass, or why the plan picked saturation instead.
            if (!intentional && role == ma::Role::bass && p.saturationConsidered)
            {
                const bool thin = p.overtonesDb < p.overtonesTargetDb - 3.0;
                if (p.harmonics.on > 0.5f && channel.data.harmonics.on > 0.5f)
                    set(ma::bus::toolHarmonics, D::kept, harmonicsText(p));
                else if (thin && p.harmonicsWouldCover >= 0 && static_cast<size_t>(p.harmonicsWouldCover) < mix.channels.size())
                    set(ma::bus::toolHarmonics, D::notNeeded, pl("bas jest ubogi w alikwoty, ale harmoniczne zasłoniłyby ")
                                                                  + quoted(mix.channels[static_cast<size_t>(p.harmonicsWouldCover)].name));
                else if (thin && p.saturation.on > 0.5f)
                    set(ma::bus::toolHarmonics, D::notNeeded, pl("bas ma już trochę środka, więc gęstość daje saturacja (lampa); "
                                                                 "enhancer harmonicznych jest na prawie czysty sinus-sub"));
                else if (!thin)
                    set(ma::bus::toolHarmonics, D::notNeeded, pl("alikwotów dość (") + juce::String(p.overtonesDb, 1)
                                                                  + pl(" dB powyżej 150 Hz względem suba, cel ") + juce::String(p.overtonesTargetDb, 0)
                                                                  + pl(" dB), na małych głośnikach go słychać"));
            }

            // Stereo.
            const auto stereoPlan = lastStereoPlans.find(channel.owner);
            if (ma::widthOn(channel.data.width))
                set(ma::bus::toolStereo, D::yours, pl("szerokość ustawiona suwakiem w Agencie (") + juce::String(juce::roundToInt(channel.data.widthRequest))
                                                       + pl("%), Mózg jej nie zmienia"));
            else if (!channel.stereoValid || stereoPlan == lastStereoPlans.end())
                set(ma::bus::toolStereo, D::notChecked, pl("brak pomiaru stereo (kanał mono albo Agent jeszcze nie wysłał danych)"));
            else if (const auto* item = chainItem(Tool::stereo))
                set(ma::bus::toolStereo, D::proposed, strip(item->text));
            else if (!stereoPlan->second.fixes.empty())
            {
                juce::StringArray parts;
                for (const auto& fix : stereoPlan->second.fixes)
                    parts.add(stereoFixText(fix));
                set(ma::bus::toolStereo, D::kept, parts.joinIntoString("; "));
            }
            else
                set(ma::bus::toolStereo, D::notNeeded, keyedStems.valid() ? pl("każde pasmo w fazie, szerokość jak w referencjach")
                                                                          : pl("każde pasmo w fazie, nic do zmiany"));

            // Phase 33: the pan.
            const auto pan = lastPanPlans.find(channel.owner);
            if (pan == lastPanPlans.end() || !pan->second.plan.considered)
                set(ma::bus::toolPan, D::notChecked, pl("brak pomiaru stereo (kanał mono albo Agent jeszcze nie wysłał danych)"));
            else if (pan->second.plan.reason == ma::PanPlan::Reason::user)
                set(ma::bus::toolPan, D::yours, pl("panorama ustawiona ręcznie (") + juce::String(juce::roundToInt(channel.data.pan.position * 100.0f))
                                                    + pl("%), Mózg jej nie zmienia, ale liczy ją w równowadze miksu"));
            else if (const auto* item = chainItem(Tool::pan))
                set(ma::bus::toolPan, D::proposed, strip(item->text));
            else if (ma::panOn(pan->second.plan.settings))
                set(ma::bus::toolPan, D::kept, panText(pan->second.plan, pan->second.partner, pan->second.inputBalanceDb));
            else
                set(ma::bus::toolPan, D::notNeeded, panReasonText(pan->second.plan.reason, pan->second.inputBalanceDb));

            // Phase 34: depth.
            if (role != ma::Role::send)
                set(ma::bus::toolDepth, D::notNeeded, pl("cięcie góry jest dla wysyłek z pogłosem i delayem; głębię tego kanału robi "
                                                         "jego wysyłka (rola „Wysyłka” na kanale z pogłosem)"));
            else if (const auto* item = chainItem(Tool::depth))
                set(ma::bus::toolDepth, D::proposed, strip(item->text));
            else if (!p.depthConsidered)
                set(ma::bus::toolDepth, D::notChecked, pl("brak widma wysyłki albo reszty miksu"));
            else
                set(ma::bus::toolDepth, ma::depthOn(p.depth) ? D::kept : D::notNeeded, depthText(p));

            // Phase 35: the bass against the kick in time.
            const auto advice = alignAdvice(channel.data);
            using K = AlignAdvice::Kind;
            if (const auto* item = chainItem(Tool::align))
                set(ma::bus::toolAlign, D::proposed, strip(item->text));
            else if (advice.kind == K::notBass)
                set(ma::bus::toolAlign, D::notNeeded, alignText(advice));
            else if (ma::alignOn(channel.data.align) && (channel.data.brainTools & ma::bus::toolAlign) == 0)
                set(ma::bus::toolAlign, D::yours, pl("opóźnienie ustawione ręcznie (") + juce::String(channel.data.align.delayMs, 1)
                                                      + pl(" ms); ") + alignText(advice));
            else if (advice.kind == K::noCable || advice.kind == K::notMeasured)
                set(ma::bus::toolAlign, D::notChecked, alignText(advice));
            else if (advice.kind == K::early)
                set(ma::bus::toolAlign, D::kept, alignText(advice));
            else
                set(ma::bus::toolAlign, D::notNeeded, alignText(advice));
        }

        // Duck under the kick.
        if (const auto* item = itemFor([](const auto& i) { return i.action == Action::duck; }))
            set(ma::bus::toolDuck, D::proposed, strip(item->text));
        else if (!has(ma::bus::toolDuck))
        {
            if (role == ma::Role::kick)
                set(ma::bus::toolDuck, D::notNeeded, pl("to stopa: inne kanały ustępują jej, nie ona im"));
            else if (!runningNote(ma::bus::toolDuck, channel.data.duck.on > 0.5f, {}))
            {
                if (!haveKick)
                    set(ma::bus::toolDuck, D::notChecked, pl("w miksie nie ma kanału z rolą „Stopa” (ustaw ją w Agencie stopy)"));
                else
                    set(ma::bus::toolDuck, D::notNeeded, pl("nie zakrywa stopy w dole, więc nie musi ustępować na uderzenia"));
            }
        }

        // Harmonics and attack: the translation steps, the planner (bass), holes in the balance.
        const bool translationKnown = mix.translation.valid && !bus;
        if (const auto* item = itemFor([](const auto& i) { return i.action == Action::harmonics; }))
            set(ma::bus::toolHarmonics, D::proposed, strip(item->text));
        else if (!runningNote(ma::bus::toolHarmonics, channel.data.harmonics.on > 0.5f,
                              has(ma::bus::toolHarmonics) ? notes[static_cast<size_t>(toolIndex(ma::bus::toolHarmonics))].text : juce::String())
                 && !has(ma::bus::toolHarmonics))
            set(ma::bus::toolHarmonics, bus ? D::notNeeded : D::notChecked,
                bus ? pl("bus nie liczy się w translacji: harmoniczne dostają jego źródła")
                    : translationKnown ? pl("nie oceniałem tego kanału") : pl("translacja jeszcze nie zmierzona: graj miks kilka sekund"));
        if (const auto* item = itemFor([](const auto& i) { return i.action == Action::transients; }))
            set(ma::bus::toolTransients, D::proposed, strip(item->text));
        else if (!runningNote(ma::bus::toolTransients, channel.data.transients.on > 0.5f,
                              has(ma::bus::toolTransients) ? notes[static_cast<size_t>(toolIndex(ma::bus::toolTransients))].text : juce::String())
                 && !has(ma::bus::toolTransients))
            set(ma::bus::toolTransients, percussive && !bus ? D::notChecked : D::notNeeded,
                !percussive ? pl("podkreślenie ataku jest dla stopy i perkusji")
                : bus       ? pl("bus nie liczy się w translacji")
                            : pl("translacja jeszcze nie zmierzona: graj miks kilka sekund"));

        // Giving way without a cable.
        {
            bool any = false;
            for (const auto& item : currentProposal.items)
                if (item.owner == channel.owner && !item.advisory && item.action == Action::unmask)
                {
                    if (!any)
                        set(ma::bus::toolUnmask, D::proposed, strip(item.text));
                    any = true;
                }
            bool running = false;
            for (int d = 0; d < ma::kMaxUnmask; ++d)
                running = running || ma::unmaskOn(channel.data.unmask[d]);
            if (!any && !runningNote(ma::bus::toolUnmask, running, {}))
            {
                const bool masks = std::any_of(mix.masking.issues.begin(), mix.masking.issues.end(), [&](const auto& issue) {
                    return issue.score >= 0.3 && mix.channels[static_cast<size_t>(issue.masker)].owner == channel.owner;
                });
                if (percussive)
                    set(ma::bus::toolUnmask, D::notNeeded, pl("stopa i perkusja nie ustępują: uderzenia mają zostać"));
                else if (bus)
                    set(ma::bus::toolUnmask, D::notNeeded, pl("bus nie liczy się w kolizjach"));
                else if (!masks)
                    set(ma::bus::toolUnmask, D::notNeeded, pl("nie zasłania nikogo na tyle, żeby musiał ustępować"));
                else
                    set(ma::bus::toolUnmask, D::notNeeded, pl("jego kolizje załatwia EQ, ustępowanie stopie albo mały krok poziomu"));
            }
        }

        lastToolNotes[channel.owner] = notes;
    }
}

void BrainProcessor::addToolLinesToVerdicts()
{
    if (!currentProposal.wholeMix)
        return;
    // The tools the verdict does not speak of yet, so every option of the Agent has its line.
    for (auto& verdict : currentProposal.verdicts)
    {
        const auto found = lastToolNotes.find(verdict.owner);
        if (found == lastToolNotes.end())
            continue;
        for (const int bit : { ma::bus::toolEq, ma::bus::toolDuck, ma::bus::toolHarmonics, ma::bus::toolTransients, ma::bus::toolUnmask })
        {
            const auto& note = found->second[static_cast<size_t>(toolIndex(bit))];
            if (note.decision == ma::bus::ToolDecision::none || (bit == ma::bus::toolUnmask && note.decision == ma::bus::ToolDecision::proposed))
                continue; // a dip in the proposal has its own line already
            verdict.reasons.add(toolName(toolIndex(bit)) + ": " + decisionText(note.decision) + pl(", ") + note.text + ".");
            verdict.reasonTools.push_back(0);
        }
    }
    lastVerdicts = currentProposal.verdicts;
}

void BrainProcessor::publishToolNotes()
{
    auto* region = session->region();
    if (region == nullptr)
        return;
    for (auto& slot : region->slots)
    {
        const auto owner = slot.owner.load();
        const auto found = owner != 0 ? lastToolNotes.find(owner) : lastToolNotes.end();
        if (found == lastToolNotes.end())
            continue;
        ma::bus::ToolNotes out {};
        out.target = owner;
        out.madeMs = lastNotesMs;
        out.wholeMix = lastNotesWholeMix ? 1 : 0;
        for (int t = 0; t < ma::bus::kToolCount; ++t)
        {
            const auto& note = found->second[static_cast<size_t>(t)];
            out.decision[t] = static_cast<std::int32_t>(note.decision);
            copyNote(out.text[t], ma::bus::kNoteLength, note.text);
        }
        ma::bus::sendNotes(slot, out);
    }
}

std::vector<Proposal::Verdict> BrainProcessor::verdictsNow() const
{
    using Tool = Proposal::Item::ChainTool;
    const auto chainTool = [](int bit) {
        switch (bit)
        {
            case ma::bus::toolCleanup: return Tool::cleanup;
            case ma::bus::toolCompressor: return Tool::compressor;
            case ma::bus::toolClipper: return Tool::clipper;
            case ma::bus::toolEvener: return Tool::evener;
            case ma::bus::toolStereo: return Tool::stereo;
            case ma::bus::toolSaturation: return Tool::saturation;
            case ma::bus::toolDynamicEq: return Tool::dynamicEq;
            case ma::bus::toolPan: return Tool::pan;
            case ma::bus::toolDepth: return Tool::depth;
            case ma::bus::toolAlign: return Tool::align;
            default: return Tool::none;
        }
    };
    auto out = lastVerdicts;
    for (auto& verdict : out)
    {
        const auto channel = std::find_if(mix.channels.begin(), mix.channels.end(), [&](const auto& c) { return c.owner == verdict.owner; });
        verdict.reasonStates.assign(verdict.reasonTools.size(), static_cast<int>(Proposal::VerdictState::none));
        for (size_t r = 0; r < verdict.reasonTools.size(); ++r)
        {
            const int bit = verdict.reasonTools[r];
            if (bit == 0 || channel == mix.channels.end())
                continue;
            auto state = Proposal::VerdictState::notApplied;
            const auto item = std::find_if(currentProposal.items.begin(), currentProposal.items.end(), [&](const auto& i) {
                if (!currentProposal.made || i.owner != verdict.owner)
                    return false;
                if (bit == ma::bus::toolUnmask)
                    return i.action == Proposal::Item::Action::unmask;
                return i.action == Proposal::Item::Action::chain && i.tool == chainTool(bit);
            });
            if (item != currentProposal.items.end())
                state = item->selected ? Proposal::VerdictState::pending : Proposal::VerdictState::unticked;
            else if ((channel->data.changedTools & bit) != 0)
                state = Proposal::VerdictState::changed;
            else if ((channel->data.brainTools & bit) != 0 || (currentProposal.made && currentProposal.wholeMix))
                state = Proposal::VerdictState::running; // or no item because the Agent already had it so
            else if ((channel->data.brainTools & bit) != 0)
                state = Proposal::VerdictState::running;
            else if ((channel->data.changedTools & bit) != 0)
                state = Proposal::VerdictState::changed;
            verdict.reasonStates[r] = static_cast<int>(state);
        }
    }
    return out;
}

void BrainProcessor::proposeHarmonics()
{
    translationDelta.clear();
    if (mix.channels.empty())
        return;

    std::vector<ma::TranslationChannel> base(mix.channels.size());
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        base[c].valid = (mix.channels[c].outputValid || mix.channels[c].spectrumValid) && inMixSums(mix.channels[c]);
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

    // Phase 27: a step may not push the balance further out of the target's corridor than it already
    // is (overtones on the kick and bass used to lift Low past it, and the next round could not undo
    // it). How far outside, summed over the bands, with the steps so far against what plays now.
    const bool tonalKnown = mix.tonal.valid && activeTarget().valid();
    const auto masterAfter = tonalKnown ? tonalMasterSpectrum() : ma::Curve {};
    const auto outside = [&](const std::vector<ma::TranslationChannel>& t) {
        if (!tonalKnown)
            return 0.0;
        auto master = masterAfter;
        for (int b = 0; b < ma::bus::kBands; ++b)
        {
            double was = 0.0, now = 0.0;
            for (size_t c = 0; c < t.size(); ++c)
                if (t[c].valid)
                {
                    was += std::pow(10.0, base[c].rawDb[static_cast<size_t>(b)] / 10.0);
                    now += std::pow(10.0, t[c].rawDb[static_cast<size_t>(b)] / 10.0);
                }
            if (was > 1e-20 && now > 1e-20)
                master[static_cast<size_t>(b)] += 10.0 * std::log10(now / was);
        }
        return ma::outsideCorridorDb(ma::compareToTarget(master, activeTarget()));
    };
    const auto tonalOk = [&](const std::vector<ma::TranslationChannel>& test) {
        return !tonalKnown || outside(test) <= outside(trial) + kTranslationTonalSlackDb;
    };

    // Phase 23: these steps are judged on what the Agents send now (after their EQ), so a room cut made
    // in an earlier round is already in `base`. It stays as it is ("Już działa") instead of being
    // dropped by the new EQ and found again somewhere else, and that channel gives no second one.
    std::vector<bool> hasRoomCut(mix.channels.size(), false);
    for (size_t m = 0; m < mix.channels.size(); ++m)
    {
        const auto& channel = mix.channels[m];
        auto* eq = proposedEq(channel.owner);
        if (channel.data.eqBypass != 0 || !inMixSums(channel) || eq == nullptr)
            continue;
        for (int b = 0; b < ma::kEqBands; ++b)
        {
            const auto have = channel.data.eq[b];
            if (have.gainDb > -1.5f || have.hz < 300.0f || have.hz > 6000.0f)
                continue;
            const bool covered = std::any_of(eq->begin(), eq->end(), [&](const auto& band) {
                return ma::isOn(band) && band.gainDb < 0.0f && std::abs(std::log2(band.hz / have.hz)) < 0.25f;
            });
            const int slot = freeSlot(*eq);
            if (covered || slot < 0)
                continue;
            (*eq)[static_cast<size_t>(slot)] = have;
            Proposal::Item item;
            item.change.kind = ma::SolverChange::Kind::eqCut;
            item.change.channel = static_cast<int>(m);
            item.change.maskedChannel = static_cast<int>(m); // none in particular
            item.change.eqSlot = slot;
            item.owner = channel.owner;
            item.band = have;
            item.inPlace = true;
            item.text = pl("Już działa: ") + quoted(channel.name) + ": " + juce::String(have.gainDb, 1) + pl(" dB przy ") + hzText(have.hz)
                        + " (Q " + juce::String(have.q, 1) + pl("), z poprzedniego „Zastosuj” (miejsce dla innych kanałów albo barwa). Zostaje");
            currentProposal.items.push_back(item);
            hasRoomCut[m] = true;
        }
    }

    for (const size_t c : order)
    {
        if (!before.valid || !trial[c].valid)
            continue;
        auto now = ma::analyseTranslation(trial);
        if (deficit(now, c) <= 0.0)
        {
            // Phase 31: say so on the Agent, so "off" does not read as "never looked at".
            const auto why = pl("na telefonie i laptopie słychać go dobrze (") + shares(now, c) + pl(" dB względem reszty miksu)");
            noteTool(mix.channels[c].owner, ma::bus::toolHarmonics, ma::bus::ToolDecision::notNeeded, why + pl(", alikwotów pod małe głośniki nie trzeba"));
            noteTool(mix.channels[c].owner, ma::bus::toolTransients, ma::bus::ToolDecision::notNeeded, why + pl(", ataku nie trzeba podkreślać"));
            continue;
        }
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
                if (m == c || !trial[m].valid || mix.channels[m].data.priority <= channel.data.priority || hasRoomCut[m])
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
                    if (gain >= 1.0 && harm(now, after, c) < 0.5 * gain && tonalOk(test))
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
                        hasRoomCut[m] = true;
                    }
                }
            }
        }
        if (deficit(now, c) <= 0.0)
        {
            const auto why = pl("ginął na telefonie (") + shares(start, c) + pl(" dB), ale wystarczyło zrobić mu miejsce na mniej ważnym kanale");
            noteTool(channel.owner, ma::bus::toolHarmonics, ma::bus::ToolDecision::notNeeded, why);
            noteTool(channel.owner, ma::bus::toolTransients, ma::bus::ToolDecision::notNeeded, why);
            continue;
        }

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

        const auto accept = [&](const ma::TranslationResult& after, const std::vector<ma::TranslationChannel>& test) {
            const double gain = deficit(now, c) - deficit(after, c);
            return gain >= 1.0 && harm(now, after, c) < 0.5 * gain && tonalOk(test);
        };
        // Phase 31: why a step that was tried did not go in.
        const auto rejected = [&](const ma::TranslationResult& after) {
            const double gain = deficit(now, c) - deficit(after, c);
            const auto opening = pl("ginie na telefonie (") + shares(now, c) + pl(" dB), sprawdziłem: ");
            if (gain < 1.0)
                return opening + pl("nawet najmocniejsze ustawienie dawało mniej niż 1 dB");
            if (harm(now, after, c) >= 0.5 * gain)
                return opening + pl("pomogłoby mu, ale zepchnęłoby inne kanały na telefonie");
            return opening + pl("przesunęłoby balans tonalny poza korytarz celu");
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
                if (!accept(after, test))
                    noteTool(channel.owner, ma::bus::toolTransients, ma::bus::ToolDecision::notNeeded, rejected(after));
                if (accept(after, test))
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
            else
                noteTool(channel.owner, ma::bus::toolTransients, ma::bus::ToolDecision::notNeeded,
                         pl("ginie na telefonie, ale w 1–6 kHz nie ma pasma, w którym atak by się przebił"));
        };
        const auto tryHarmonics = [&] {
            if (channel.data.harmonics.on > 0.5f)
                return;
            if (peakHz > 250.0)
            {
                noteTool(channel.owner, ma::bus::toolHarmonics, ma::bus::ToolDecision::notNeeded,
                         pl("najmocniej gra ok. ") + hzText(peakHz) + pl(": harmoniczne robią alikwoty z treści poniżej 250 Hz, tu pomaga obecność w EQ"));
                return;
            }
            ma::HarmonicSettings settings;
            settings.on = 1.0f;
            settings.hz = static_cast<float>(juce::jlimit(60.0, 300.0, std::round(2.0 * peakHz / 5.0) * 5.0));
            auto test = trial;
            ma::TranslationResult after = now;
            // Overtones on a kick soften its punch, so less of them there.
            const float most = percussive ? kMaxPercussiveHarmonicsDb : kMaxHarmonicsDb;
            settings.amountDb = -60.0f; // none yet
            for (float amount = -24.0f; amount <= most; amount += 1.0f)
            {
                auto louder = settings;
                louder.amountDb = amount;
                auto attempt = trial;
                attempt[c].rawDb = ma::predictHarmonics(trial[c].rawDb, louder);
                // Only as much as the balance takes (phase 27).
                if (!tonalOk(attempt))
                    break;
                settings = louder;
                test = attempt;
                after = ma::analyseTranslation(test);
                if (deficit(after, c) <= 0.0)
                    break;
            }
            if (settings.amountDb <= -60.0f)
                noteTool(channel.owner, ma::bus::toolHarmonics, ma::bus::ToolDecision::notNeeded,
                         pl("ginie na telefonie, ale nawet najsłabsze harmoniczne wypchnęłyby dół poza korytarz celu tonalnego"));
            else if (!accept(after, test))
                noteTool(channel.owner, ma::bus::toolHarmonics, ma::bus::ToolDecision::notNeeded, rejected(after));
            if (settings.amountDb > -60.0f && accept(after, test))
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
                    const bool background = mix.channels[o].data.role == static_cast<int>(ma::Role::send);
                    was[o] = { trial[o].rawDb, mix.channels[o].data.priority, trial[o].valid, background };
                    will[o] = { test[o].rawDb, mix.channels[o].data.priority, test[o].valid, background };
                }
                const auto maskWas = ma::analyseMasking(was);
                const auto maskWill = ma::analyseMasking(will);
                bool collides = false;
                for (size_t o = 0; o < trial.size(); ++o)
                    if (o != c && mix.channels[o].data.priority <= channel.data.priority)
                        collides = collides || maskWill.amount[c][o] - maskWas.amount[c][o] > 0.05;
                if (!collides && accept(after, test))
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

        // Phase 31: the steps that were not tried, and why.
        const auto noted = [&](int bit) {
            const auto found = toolWhy.find(channel.owner);
            int index = 0;
            while (index < ma::bus::kToolCount && (1 << index) != bit)
                ++index;
            return found != toolWhy.end() && found->second[static_cast<size_t>(index)].decision != ma::bus::ToolDecision::none;
        };
        const auto inProposal = [&](Proposal::Item::Action action) {
            return std::any_of(currentProposal.items.begin(), currentProposal.items.end(),
                               [&](const auto& i) { return i.owner == channel.owner && i.action == action; });
        };
        if (!noted(ma::bus::toolHarmonics) && !inProposal(Proposal::Item::Action::harmonics))
            noteTool(channel.owner, ma::bus::toolHarmonics, ma::bus::ToolDecision::notNeeded,
                     peakHz > 250.0 ? pl("najmocniej gra ok. ") + hzText(peakHz)
                                          + pl(": harmoniczne robią alikwoty z treści poniżej 250 Hz, tu pomaga obecność w EQ")
                                    : pl("ginął na telefonie, ale wystarczył wcześniejszy krok (") + (percussive ? pl("atak") : pl("miejsce albo obecność")) + ")");
        if (!noted(ma::bus::toolTransients) && !inProposal(Proposal::Item::Action::transients))
            noteTool(channel.owner, ma::bus::toolTransients, ma::bus::ToolDecision::notNeeded,
                     percussive ? pl("ginął na telefonie, ale wystarczył wcześniejszy krok") : pl("podkreślenie ataku jest dla stopy i perkusji"));

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

    // Phase 27: what these steps add to each channel, for the second pass of propose().
    translationDelta.clear();
    for (size_t c = 0; c < mix.channels.size(); ++c)
    {
        if (!trial[c].valid)
            continue;
        ma::SpectrumAnalyzer::Bands delta {};
        bool any = false;
        for (int b = 0; b < ma::bus::kBands; ++b)
        {
            delta[static_cast<size_t>(b)] = trial[c].rawDb[static_cast<size_t>(b)] - base[c].rawDb[static_cast<size_t>(b)];
            any = any || std::abs(delta[static_cast<size_t>(b)]) > 0.05;
        }
        if (any)
            translationDelta[mix.channels[c].owner] = delta;
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
        // Phase 30: without a cable the kick's Agent gives the key over the bus.
        const bool wireless = !connected && masked.data.keyId != 0 && masker.data.keyId != 0;
        const bool wasOn = masker.data.duck.on > 0.5f && (connected || masker.data.duckKeyFound != 0);
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
        {
            noteTool(masker.owner, ma::bus::toolDuck, ma::bus::ToolDecision::kept,
                     pl("już ustępuje ") + quoted(masked.name) + pl(" w tym paśmie na każde uderzenie"));
            continue;
        }

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
        item.duckKey = masked.data.keyId;
        if (connected)
            item.text += pl(". Sidechain podłączony");
        else if (wireless)
            item.text += pl(". Bez kabla: Agent ") + quoted(masked.name) + pl(" podaje swoją obwiednię przez wspólną pamięć");
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

// Phase 30: the target used last, in any project, so a new project starts with it.
juce::File BrainProcessor::lastTargetFile()
{
    return targetFolder().getChildFile("last-target.txt");
}

void BrainProcessor::rememberLastTarget(const juce::String& name)
{
    targetFolder().createDirectory();
    lastTargetFile().replaceWithText(name);
}

bool BrainProcessor::selectLastTarget()
{
    const auto name = lastTargetFile().loadFileAsString().trim();
    if (name.isEmpty() || !selectTarget(name))
        return false;
    autoSelectedTarget = true;
    return true;
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
            autoSelectedTarget = false;
            rememberLastTarget(name);
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
    autoSelectedTarget = false;
    rememberLastTarget(trimmed);
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
                                          : pl("Breaku nie znalazłem (potrzebny stem perkusji i co najmniej 8 s bez niej). "))
               + (profile.intro.valid ? pl("Intro ") + juce::String(juce::roundToInt(profile.intro.seconds)) + pl(" s, ")
                                            + juce::String(profile.intro.belowDropDb, 1) + pl(" dB ciszej. ")
                                      : pl("Intro brak (stemy zaczynają się od dropu). "))
               + (profile.build.valid ? pl("Build ") + juce::String(juce::roundToInt(profile.build.seconds)) + pl(" s, ")
                                            + juce::String(profile.build.belowDropDb, 1) + pl(" dB ciszej. ")
                                      : juce::String());
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

int BrainProcessor::applySelected(bool refining)
{
    if (!currentProposal.made)
        return 0;
    if (comparing)
        setComparing(false);

    double globalGain = 0.0;
    for (const auto& item : currentProposal.items)
        if (item.selected && item.action == Proposal::Item::Action::solver && item.change.kind == ma::SolverChange::Kind::globalGain)
            globalGain += item.change.gainDb;
    refine.globalGainDb += globalGain;

    // Phase 9: a stem group's level moves every channel of the group, its thresholds with it.
    std::array<double, ma::kStemGroups> groupGain {};
    for (const auto& item : currentProposal.items)
        if (item.selected && item.action == Proposal::Item::Action::level && item.group >= 0 && item.group < ma::kStemGroups)
            groupGain[static_cast<size_t>(item.group)] += item.gainDb;

    int sent = 0;
    for (const auto& channel : mix.channels)
    {
        const int group = groupOfChannel(channel);
        double gain = globalGain + (group >= 0 ? groupGain[static_cast<size_t>(group)] : 0.0);
        // A bus is fed by channels that take the gain themselves: its trim stays, and what reaches
        // its compressor and clipper moves with its sources (the group it was recognised as).
        double levelGain = gain;
        if (!inMixSums(channel))
        {
            const int sources = ma::stemGroupForRole(static_cast<ma::Role>(juce::jlimit(0, static_cast<int>(ma::Role::send), static_cast<int>(channel.data.detectedRole))));
            levelGain = globalGain + (sources >= 0 ? groupGain[static_cast<size_t>(sources)] : 0.0);
            gain = 0.0;
        }
        // Phase 23: one channel quieter for a collision EQ cannot take further.
        for (const auto& item : currentProposal.items)
            if (item.selected && (item.action == Proposal::Item::Action::channelLevel || item.action == Proposal::Item::Action::balance)
                && item.owner == channel.owner && inMixSums(channel))
            {
                gain += item.gainDb;
                levelGain += item.gainDb;
            }
        // A duck listens to the kick: its threshold follows the drums.
        const double kickGain = globalGain + groupGain[static_cast<size_t>(ma::StemGroup::drums)];
        bool eqChanges = false;
        auto eq = proposedEqFor(channel, eqChanges);
        auto duck = channel.data.duck;
        if (duck.on > 0.5f)
            duck.thresholdDb = static_cast<float>(juce::jlimit(-60.0, 0.0, duck.thresholdDb + kickGain - globalGain));
        auto duckSync = channel.data.duckSync;
        auto duckKey = channel.data.duckKey;
        auto polarity = channel.data.polarityInverted;
        auto harmonics = channel.data.harmonics;
        auto transients = channel.data.transients;
        auto cleanup = channel.data.cleanup;
        auto compressor = channel.data.compressor;
        auto clipper = channel.data.clipper;
        auto evener = channel.data.evener;
        auto dynamicEq = channel.data.dynamicEq;
        auto stereoEq = channel.data.stereo;
        auto saturation = channel.data.saturation;
        auto pan = channel.data.pan;
        auto depth = channel.data.depth;
        auto align = channel.data.align;
        // Phase 30: the dips go along as the Agent has them, unless the proposal sets one.
        std::array<ma::UnmaskSettings, ma::kMaxUnmask> unmask {};
        std::array<std::uint64_t, ma::kMaxUnmask> unmaskKey {};
        for (int d = 0; d < ma::kMaxUnmask; ++d)
        {
            unmask[static_cast<size_t>(d)] = channel.data.unmask[d];
            unmaskKey[static_cast<size_t>(d)] = channel.data.unmaskKey[d];
        }
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
                duckKey = item.duckKey;
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
                    case Proposal::Item::ChainTool::saturation: saturation = item.saturation; break;
                    case Proposal::Item::ChainTool::dynamicEq: dynamicEq = item.dynamicEq; break;
                    case Proposal::Item::ChainTool::pan: pan = item.pan; break;
                    case Proposal::Item::ChainTool::depth: depth = item.depth; break;
                    case Proposal::Item::ChainTool::align: align = item.align; break;
                    case Proposal::Item::ChainTool::none: break;
                }
                dynamicChanges = true;
            }
            else if (item.action == Proposal::Item::Action::polarity)
            {
                polarity = polarity != 0 ? 0 : 1;
                dynamicChanges = true;
            }
            else if (item.action == Proposal::Item::Action::unmask && item.unmaskIndex >= 0 && item.unmaskIndex < ma::kMaxUnmask)
            {
                unmask[static_cast<size_t>(item.unmaskIndex)] = item.unmask;
                unmaskKey[static_cast<size_t>(item.unmaskIndex)] = item.unmaskKey;
                dynamicChanges = true;
            }
        }

        if (!eqChanges && !dynamicChanges && std::abs(gain) < 0.05 && std::abs(levelGain) < 0.05 && std::abs(kickGain - globalGain) < 0.05)
            continue;
        if (!eqChanges)
            for (int b = 0; b < ma::kEqBands; ++b)
                eq[static_cast<size_t>(b)] = channel.data.eq[b]; // keep what is there

        ma::bus::Command command {};
        command.target = channel.owner;
        command.serial = ++commandSerial;
        command.kind = refining ? ma::bus::CommandKind::refine : ma::bus::CommandKind::apply;
        command.trimDb = static_cast<float>(juce::jlimit(-24.0, 24.0, channel.data.trimDb + gain));
        for (int b = 0; b < ma::kEqBands; ++b)
            command.eq[b] = eq[static_cast<size_t>(b)];
        command.duck = duck;
        command.duckSync = duckSync;
        command.duckKey = duckKey;
        command.polarityInverted = polarity;
        command.harmonics = harmonics;
        command.transients = transients;
        // The trim moves with the gain, and the compressor and clipper levels with it.
        compressor.thresholdDb = static_cast<float>(juce::jlimit(-60.0, 0.0, compressor.thresholdDb + levelGain));
        clipper.ceilingDb = static_cast<float>(juce::jlimit(-30.0, 0.0, clipper.ceilingDb + levelGain));
        command.cleanup = cleanup;
        command.compressor = compressor;
        command.clipper = clipper;
        command.evener = evener;
        command.stereo = stereoEq;
        command.saturation = saturation;
        // The rider's target is a level at its input: it moves with the channel's gain, its bands are relative.
        if (ma::riderOn(dynamicEq.rider))
            dynamicEq.rider.targetDb = static_cast<float>(juce::jlimit(-60.0, 0.0, dynamicEq.rider.targetDb + levelGain));
        command.dynamicEq = dynamicEq;
        command.pan = pan;
        command.depth = depth;
        command.align = align;
        command.inputShiftDb = inMixSums(channel) ? 0.0f : static_cast<float>(levelGain);
        for (int d = 0; d < ma::kMaxUnmask; ++d)
        {
            command.unmask[d] = unmask[static_cast<size_t>(d)];
            command.unmaskKey[d] = unmaskKey[static_cast<size_t>(d)];
        }
        sendCommand(channel.owner, command);
        ++sent;
    }

    if (sent > 0)
    {
        // Phase 28: which level steps went in, so a collision they did not quite clear reads as done.
        for (auto& step : collisionSteps)
            step.applied = std::any_of(currentProposal.items.begin(), currentProposal.items.end(), [&](const auto& item) {
                return item.selected && item.action == Proposal::Item::Action::channelLevel && item.owner == step.masker;
            });
        currentProposal = {};
        loudnessBefore = loudnessAfter = -INFINITY;
        resetAtMs = ma::SharedSession::nowMs() + kAgentSwitchMs; // measure the mix again from there
        lastApplyMs = juce::Time::currentTimeMillis();
    }
    return sent;
}

// Phase 30: refining after the user's "Zastosuj".
bool BrainProcessor::autoRefineOn() const
{
    return autoRefine->load() > 0.5f;
}

std::array<std::int64_t, 5> BrainProcessor::itemSignature(const Proposal::Item& item)
{
    const auto extra = item.action == Proposal::Item::Action::solver ? static_cast<std::int64_t>(item.change.kind)
                       : item.action == Proposal::Item::Action::unmask ? static_cast<std::int64_t>(item.maskedOwner)
                                                                        : 0;
    return { static_cast<std::int64_t>(item.owner), static_cast<std::int64_t>(item.action), static_cast<std::int64_t>(item.tool),
             static_cast<std::int64_t>(item.group), extra };
}

int BrainProcessor::applyByUser()
{
    stopRefining();
    // What the user unticked stays unticked in the rounds that follow.
    std::vector<std::array<std::int64_t, 5>> unticked;
    for (const auto& item : currentProposal.items)
        if (!item.advisory && !item.selected && !item.inPlace)
            unticked.push_back(itemSignature(item));
    const int scope = analysisScope;
    refine.globalGainDb = 0.0;
    refine.peakSeconds = std::min(mix.dropSeconds, kMaxRestoreWaitSeconds);
    const int sent = applySelected(false);
    if (sent > 0 && autoRefineOn())
    {
        refine.active = true;
        refine.roundsLeft = kRefineRounds;
        refine.round = 0;
        refine.scope = scope;
        refine.sawReset = false;
        refine.readyMs = 0;
        refine.unticked = std::move(unticked);
        refine.lastApplied.clear();
        refine.status = pl("Zastosowano na ") + juce::String(sent) + pl(" kanałach. Puść drop: zmierzę efekt i dopracuję resztę sam.");
    }
    return sent;
}

void BrainProcessor::stopRefining()
{
    refine.active = false;
    refine.status = {};
}

void BrainProcessor::updateRefine(std::uint64_t now)
{
    if (!refine.active)
        return;
    if (!autoRefineOn())
    {
        stopRefining();
        return;
    }
    if (resetAtMs != 0)
        return; // the Agents are still switching
    if (mix.dropSeconds < 1.0)
        refine.sawReset = true;
    if (!refine.sawReset)
        return;
    const auto roundText = refine.round == 0 ? pl("Mierzę efekt: ")
                           : pl("Dopracowuję (po rundzie ") + juce::String(refine.round) + pl(" z ") + juce::String(kRefineRounds) + pl("): ");
    if (mix.dropSeconds < kMinimumDropSeconds)
    {
        refine.readyMs = 0;
        refine.status = roundText + pl("puść drop, mierzę efekt ") + juce::String(juce::roundToInt(mix.dropSeconds)) + " / "
                        + juce::String(juce::roundToInt(kMinimumDropSeconds)) + " s.";
        return;
    }
    // Gain comes back only from a peak measured over as much of the drop as the cut was.
    if (refine.globalGainDb < 0.0 && mix.dropSeconds < refine.peakSeconds)
    {
        refine.readyMs = 0;
        refine.status = roundText + pl("mierzę szczyty całego dropu ") + juce::String(juce::roundToInt(mix.dropSeconds)) + " / "
                        + juce::String(juce::roundToInt(refine.peakSeconds)) + " s.";
        return;
    }
    // The loud-master preview needs a little longer; it gets kRefinePreviewWaitMs more at most.
    if (refine.readyMs == 0)
        refine.readyMs = now;
    if (previewStats().seconds < kMinimumPreviewSeconds && now - refine.readyMs < kRefinePreviewWaitMs)
    {
        refine.status = roundText + pl("sprawdzam jeszcze głośny master.");
        return;
    }

    analyse(refine.scope);
    for (auto& item : currentProposal.items)
        if (!item.advisory && std::find(refine.unticked.begin(), refine.unticked.end(), itemSignature(item)) != refine.unticked.end())
            item.selected = false;
    const auto left = std::count_if(currentProposal.items.begin(), currentProposal.items.end(),
                                    [](const auto& i) { return !i.advisory && i.selected != i.inPlace; });
    // The same changes as the round before: applying them again would not move anything.
    juce::StringArray changes;
    for (const auto& item : currentProposal.items)
        if (!item.advisory && item.selected != item.inPlace)
            changes.add(item.text);
    changes.sort(false);
    const bool repeating = refine.round > 0 && changes == refine.lastApplied;
    if (left == 0 || refine.roundsLeft <= 0 || repeating)
    {
        refine.active = false;
        // mixcheck: the last round's other changes (levels, EQ, a vocal rider) can lift the peak past what
        // its give-back left as margin. The rounds end inside the ceiling: only the analysis's own ceiling
        // cut goes out then, everything else stays as it is.
        const double peak = truePeakDb();
        const auto cut = std::find_if(currentProposal.items.begin(), currentProposal.items.end(), [](const auto& i) {
            return !i.advisory && i.action == Proposal::Item::Action::solver && i.change.kind == ma::SolverChange::Kind::globalGain
                   && i.change.gainDb < 0.0;
        });
        if (std::isfinite(peak) && peak > peakCeilingDb() && cut != currentProposal.items.end())
        {
            for (auto& item : currentProposal.items)
                item.selected = &item == &*cut ? true : item.inPlace;
            const double cutDb = cut->change.gainDb;
            applySelected(true);
            refine.status = pl("Dopracowane (") + juce::String(refine.round) + (refine.round == 1 ? pl(" runda") : pl(" rundy"))
                            + pl("). Szczyt wyszedł ") + juce::String(peak, 1) + pl(" dBTP, ponad limit pre-master: ściszyłem wszystko o ")
                            + juce::String(-cutDb, 1) + pl(" dB.");
            return;
        }
        if (left == 0)
            refine.status = refine.round == 0 ? pl("Efekt zmierzony: nic więcej do zmiany.")
                                              : pl("Dopracowane (") + juce::String(refine.round)
                                                    + (refine.round == 1 ? pl(" runda") : pl(" rundy")) + pl("): nic więcej do zmiany.");
        else
            refine.status = pl("Dopracowane (") + juce::String(refine.round) + (refine.round == 1 ? pl(" runda") : pl(" rundy"))
                            + pl("). W propozycji zostało ")
                            + juce::String(static_cast<int>(left))
                            + (left == 1 ? pl(" zmiana") : left < 5 ? pl(" zmiany") : pl(" zmian")) + pl(": przejrzyj i zastosuj sam, jeśli chcesz.");
        return;
    }
    refine.lastApplied = changes;
    const int sent = applySelected(true);
    ++refine.round;
    --refine.roundsLeft;
    refine.sawReset = false;
    refine.readyMs = 0;
    refine.status = pl("Runda ") + juce::String(refine.round) + pl(": dopracowano ") + juce::String(sent)
                    + pl(" kanałów. Puść drop jeszcze raz, zmierzę efekt.");
}

bool BrainProcessor::canUndo() const
{
    return std::any_of(mix.channels.begin(), mix.channels.end(), [](const auto& c) { return c.data.hasBackup != 0; });
}

int BrainProcessor::undoLast()
{
    stopRefining();
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
        lastApplyMs = juce::Time::currentTimeMillis();
    }
    return sent;
}

void BrainProcessor::setComparing(bool shouldCompare)
{
    if (shouldCompare)
        stopRefining();
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
    parameters.state.setProperty("sketchMode", sketchOn, nullptr);
    parameters.state.setProperty("tonalAfterLimiter", afterLimiterOn, nullptr);
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
            // Phase 30: a project without a target (or whose target file is gone) takes the last one used.
            if (name.isEmpty() || !selectTarget(name))
                selectLastTarget();
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
            sketchOn = static_cast<bool>(parameters.state.getProperty("sketchMode", true));
            afterLimiterOn = static_cast<bool>(parameters.state.getProperty("tonalAfterLimiter", true));
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
    for (int s = 0; s < ma::kStemSections; ++s)
        keyedSectionStems[static_cast<size_t>(s)] = ma::stemSectionTarget(keyedStems, static_cast<ma::StemSection>(s));
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
    // Phase 28: only once audio has run since the last reset, or an Agent's block from before
    // "Zastosuj" (transport stopped, or the commands just landing) would stay in the hold.
    const bool fresh = !previewResetRequested.load() && previewStats().seconds >= kPeakSettleSeconds;
    for (const auto& channel : mix.channels)
        if (fresh && std::isfinite(channel.data.outputPeakDb))
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
    const auto lower = pl(" albo niższy cel, np. ") + juce::String(target - 2.0, 1) + " LUFS.";
    auto advice = (peaks.isNotEmpty() ? pl(" Najwyższe szczyty daje ") + quoted(peaks) + pl(": clipper na nim („Analizuj”, cały łańcuch)")
                                      : pl(" Clipper na kanałach z najwyższymi szczytami („Analizuj”, cały łańcuch)"))
                  + lower;
    // Phase 28: after a whole-mix analysis that saw the preview, say what it did about it.
    const bool clipPending = currentProposal.made && std::any_of(currentProposal.items.begin(), currentProposal.items.end(), [&](const auto& item) {
        return item.selected && item.action == Proposal::Item::Action::chain && item.tool == Proposal::Item::ChainTool::clipper
               && item.clipper.on > 0.5f
               && std::find(loudMasterPlan.owners.begin(), loudMasterPlan.owners.end(), item.owner) != loudMasterPlan.owners.end();
    });
    if (clipPending)
        advice = pl(" W propozycji jest mocniejszy clipper na ") + loudMasterPlan.carriers.joinIntoString(", ")
                 + pl(" pod głośny master: kliknij „Zastosuj”, potem posłuchaj drop jeszcze raz.");
    else if (loudMasterPlan.made && loudMasterPlan.capped)
        advice = pl(" Clipper na ") + loudMasterPlan.carriers.joinIntoString(", ")
                 + pl(" ścina już tyle, ile się da bez spłaszczenia uderzenia. Reszta to gęstość dropu (bas, pady): ścisz je trochę")
                 + lower;
    else if (loudMasterPlan.made && !loudMasterPlan.carriers.isEmpty())
        // Measured again with the new clipping and still too busy: the clippers have room left.
        advice = pl(" Clipper na ") + loudMasterPlan.carriers.joinIntoString(", ")
                 + pl(" już pomaga, a ma jeszcze zapas: kliknij „Analizuj”, dobierze go do tego, co limiter ścina teraz") + lower;
    else if (loudMasterPlan.made && loudMasterPlan.loudest.isNotEmpty())
        advice = pl(" Najwyższe szczyty daje ") + quoted(loudMasterPlan.loudest)
                 + pl(", a to nie perkusja, więc Mózg nie clipuje go sam: clipper albo kompresor na nim ustaw ręcznie") + lower;
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

juce::String BrainProcessor::songTime(double seconds) const
{
    const int total = juce::roundToInt(std::max(0.0, seconds));
    const auto clock = juce::String(total / 60) + ":" + juce::String(total % 60).paddedLeft('0', 2);
    const double bpm = tempoBpm();
    return bpm > 0.0 ? pl("takt ") + juce::String(static_cast<int>(seconds * bpm / 240.0 + 1e-6) + 1) + " (" + clock + ")" : clock;
}

MixSnapshot::ReportItem BrainProcessor::energyItem() const
{
    using Status = MixSnapshot::Status;
    MixSnapshot::ReportItem item;
    item.title = pl("Przebieg utworu (mapa energii)");
    item.group = MixSnapshot::ReportGroup::arrangement;
    const auto& e = mix.energy;
    if (!e.comparison.valid)
    {
        item.detail = pl("Puść utwór od początku przez pierwszy drop (FL w trybie utworu, żeby Mózg znał pozycję).");
        return item;
    }
    // Phase 22: a loop of the drop (pattern mode, or only the drop played) has no course to judge:
    // every heard second is at the drop's level, so "first drop at bar 1" would mean nothing.
    {
        int heard = 0, under = 0;
        for (const double level : e.ours.levelDb)
            if (std::isfinite(level))
            {
                ++heard;
                under += level < -6.0 ? 1 : 0;
            }
        if (under < 4)
        {
            item.detail = pl("Słyszę tylko drop (") + juce::String(heard)
                          + pl(" s, nic wyraźnie ciszej), więc przebiegu utworu nie ma z czego ocenić. Puść całość od początku "
                               "w trybie utworu (Song), przez intro, build i pierwszy drop.");
            return item;
        }
    }
    const auto& c = e.comparison;
    const auto& r = e.references;
    auto range = [](const ma::StemCorridor& corridor, int decimals) {
        return juce::String(corridor.low, decimals) + pl("–") + juce::String(corridor.high, decimals);
    };
    juce::StringArray parts, advice;
    {
        juce::String text = pl("pierwszy drop: ") + songTime(c.firstDropSeconds);
        if (r.valid)
        {
            const int low = juce::roundToInt(r.firstDrop.low), high = juce::roundToInt(r.firstDrop.high);
            text += pl(", w referencjach ") + juce::String(low / 60) + ":" + juce::String(low % 60).paddedLeft('0', 2) + pl("–")
                    + juce::String(high / 60) + ":" + juce::String(high % 60).paddedLeft('0', 2);
            if (c.firstDropOutside > 0.0)
                advice.add(pl("intro z buildem trwa dłużej niż w referencjach"));
            else if (c.firstDropOutside < 0.0)
                advice.add(pl("drop wchodzi wcześniej niż w referencjach"));
        }
        parts.add(text);
    }
    {
        juce::String text = pl("dropów: ") + juce::String(c.drops);
        if (r.valid)
            text += pl(" (w referencjach ") + range(r.drops, 0) + ")";
        parts.add(text);
    }
    if (std::isfinite(c.deepestDb))
    {
        juce::String text = pl("najgłębiej między dropami ") + juce::String(-c.deepestDb, 1) + pl(" dB pod dropem");
        if (r.valid && r.deepest.valid)
        {
            text += pl(" (w referencjach ") + juce::String(-r.deepest.high, 1) + pl("–") + juce::String(-r.deepest.low, 1) + " dB)";
            // A dB of slack: the corridor's edge is not a problem yet.
            if (c.deepestOutside >= 1.0)
                advice.add(pl("między dropami miks siada za mało, drop nie ma z czego wyrosnąć"));
            else if (c.deepestOutside <= -1.0)
                advice.add(pl("między dropami miks siada głębiej niż w referencjach"));
        }
        parts.add(text);
    }
    for (const auto& span : c.spans)
        advice.add(songTime(span.fromSeconds) + pl("–") + songTime(span.toSeconds) + pl(" o ") + juce::String(std::abs(span.deviationDb), 1)
                   + (span.deviationDb > 0.0 ? pl(" dB głośniej") : pl(" dB ciszej")) + pl(" niż referencje w tym miejscu"));
    if (!r.valid)
    {
        item.status = Status::good;
        item.detail = parts.joinIntoString(", ") + "."
                      + (keyedStems.valid() ? pl(" Zestaw referencji nie ma jeszcze przebiegu energii: wczytaj ich stemy jeszcze raz.")
                                            : pl(" Wczytaj referencje ze stemami, żeby porównać przebieg."));
        return item;
    }
    item.status = advice.isEmpty() ? Status::good : Status::warning;
    item.detail = parts.joinIntoString(", ") + "."
                  + (advice.isEmpty() ? pl(" Przebieg jak w referencjach.")
                                      : " " + advice.joinIntoString("; ").substring(0, 1).toUpperCase() + advice.joinIntoString("; ").substring(1)
                                            + pl(". To sprawa aranżu, widać to na zakładce „Energia”."));
    return item;
}

juce::String BrainProcessor::automationStep(const ma::SectionAutomation& automation, int group)
{
    const auto at = static_cast<size_t>(group);
    if (!automation.valid || group < 0 || group >= ma::kStemGroups)
        return {};
    if (automation.mute[at])
        return pl("zgaś");
    return std::abs(automation.gainDb[at]) > 0.05 ? signedDb(automation.gainDb[at]) : juce::String();
}

juce::String BrainProcessor::automationText(ma::StemSection section) const
{
    const auto& ours = mix.section(section);
    const auto& automation = ours.automation;
    if (!ours.stems.valid || !automation.valid || !automation.any)
        return {};
    juce::StringArray parts;
    for (int g = 0; g < ma::kStemGroups; ++g)
    {
        const auto step = automationStep(automation, g);
        if (step.isEmpty())
            continue;
        juce::StringArray names;
        for (const auto& channel : mix.channels)
            if (groupOfChannel(channel) == g && channel.name.isNotEmpty())
                names.add(channel.name);
        parts.add(stemGroupName(g) + " " + step + (names.isEmpty() ? juce::String() : " (" + names.joinIntoString(", ") + ")"));
    }
    juce::String out = pl("Automatyka głośności względem dropu (głośność kanału w Channel Rack, przed Agentem, żeby Mózg ją słyszał): ")
                       + parts.joinIntoString(", ");
    if (section == ma::StemSection::build)
        out += pl(" (średnio; build zwykle narasta do dropu)");
    if (std::isfinite(automation.belowDropAfter) && std::isfinite(automation.belowDropBefore)
        && std::abs(automation.belowDropAfter - automation.belowDropBefore) >= 0.5)
        out += pl(", wtedy ") + sectionText(section).name + pl(" będzie ") + juce::String(automation.belowDropAfter, 1)
               + pl(" dB ciszej niż drop");
    // Where: bars (4/4 at the host's tempo) and minutes.
    juce::StringArray where;
    const double bpm = tempoBpm();
    auto clock = [](double seconds) {
        const int total = juce::roundToInt(seconds);
        return juce::String(total / 60) + ":" + juce::String(total % 60).paddedLeft('0', 2);
    };
    for (size_t r = 0; r < ours.ranges.size() && r < 3; ++r)
    {
        const auto [from, to] = ours.ranges[r];
        juce::String text;
        if (bpm > 0.0)
            text = pl("takty ") + juce::String(static_cast<int>(from * bpm / 240.0) + 1) + pl("–")
                   + juce::String(static_cast<int>(std::ceil(to * bpm / 240.0 - 1e-6))) + " (" + clock(from) + pl("–") + clock(to) + ")";
        else
            text = clock(from) + pl("–") + clock(to);
        where.add(text);
    }
    if (!where.isEmpty())
        out += pl(". Gdzie: ") + where.joinIntoString(", ");
    return out + ".";
}

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
        if (!item.gate && item.status == MixSnapshot::Status::problem)
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
    // In a sketch, only what stays with the missing parts in belongs to the master's EQ.
    const auto& masterTonal = mix.sketch.active() && mix.sketch.tonal.valid ? mix.sketch.tonal : mix.tonal;
    if (masterTonal.valid && !masterTonal.regions.empty())
    {
        out << "<li>" << pl("EQ na sumie (szeroko, względem celu ") << html(quoted(juce::String::fromUTF8(activeTarget().name.c_str()))) << "): ";
        juce::StringArray parts;
        for (const auto& region : masterTonal.regions)
            parts.add((region.hole() ? "+" : pl("−")) + juce::String(std::abs(region.deviationDb), 1) + " dB " + hzText(region.lowHz) + pl("–")
                      + hzText(region.highHz));
        out << html(parts.joinIntoString(", ")) << pl(". Lepiej poprawić to w miksie, jeśli to jeden kanał.") << "</li>";
    }
    else if (masterTonal.valid)
    {
        out << "<li>" << pl("EQ na sumie: balans mieści się w korytarzu celu ") << html(quoted(juce::String::fromUTF8(activeTarget().name.c_str())))
            << pl(", wystarczą drobne korekty do gustu.") << "</li>";
    }
    // Phase 24: what is left of each TBC range's distance from the centre (after "Zastosuj" it is
    // what the channels could not take): a broad move in Ozone's EQ or a Dynamics band's gain.
    if (masterTonal.valid)
    {
        const auto moves = ozoneCurve(masterTonal);
        if (!moves.isEmpty())
            out << "<li>" << pl("Krzywa EQ do Ozone na resztę (Bell, kolejno): ")
                << html(moves.joinIntoString("; "))
                << pl(". Najpierw „Analizuj” i „Zastosuj” w Mózgu: zrobi to na kanałach, tu zostaje tylko reszta.")
                << (afterLimiterOn && limiterTiltKnown() ? pl(" Liczone jak po limiterze do celu.") : juce::String()) << "</li>";
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
    if (mix.sketch.active())
    {
        out << "<h2>" << pl("Aranż: to jeszcze szkic") << "</h2><section><p>"
            << pl("Drop nie ma jeszcze wszystkich partii. Balans i proporcje liczone z partiami-duchami; zanim pójdzie do Ozone, dograj:")
            << "</p><ul>";
        for (const auto& ghost : mix.sketch.analysis.ghosts)
            out << "<li>" << html(ghostBrief(ghost)) << "</li>";
        out << "</ul></section>\n";
    }
    if (keyedStems.valid() && mix.stems.valid)
    {
        out << "<h2>" << pl("Proporcje jak w referencjach (drop do dropu)") << "</h2><section>";
        const auto key = trackKey();
        if (key.valid())
            out << "<p>" << pl("Tonacja utworu: ") << html(keyText(key)) << pl(". Referencje przesunięte do niej.") << "</p>";
        out << groupTable(mix.stems) << "</section>\n";
    }
    for (const auto section : { ma::StemSection::intro, ma::StemSection::build, ma::StemSection::breakdown })
    {
        const auto& target = keyedSectionStems[static_cast<size_t>(section)];
        const auto& ours = mix.section(section);
        if (!target.valid() || !ours.stems.valid)
            continue;
        const auto text = sectionText(section);
        out << "<h2>" << html(text.title) << "</h2><section><p>" << html(text.name.substring(0, 1).toUpperCase() + text.name.substring(1))
            << pl(" miksu jest ") << juce::String(ours.belowDropDb, 1) << pl(" dB ciszej niż drop");
        if (target.contrast(section).valid)
            out << pl(", w referencjach ") << juce::String(target.contrast(section).low, 1) << pl("–")
                << juce::String(target.contrast(section).high, 1) << " dB";
        out << pl(". Różnice w sekcjach poprawia aranż i automatyka głośności, nie EQ.") << "</p>" << groupTable(ours.stems);
        if (const auto automation = automationText(section); automation.isNotEmpty())
            out << "<p>" << html(automation) << "</p>";
        out << "</section>\n";
    }
    if (mix.energy.comparison.valid)
        out << "<h2>" << pl("Przebieg utworu") << "</h2><section><p>" << html(energyItem().detail) << "</p></section>\n";

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
