#include "ma/StemReference.h"

#include "ma/EqualLoudness.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace ma
{
namespace
{
constexpr double kAbsentDb = -200.0;
constexpr double kLevelLowHz = 30.0, kLevelHighHz = 16000.0;
constexpr double kWidthLowHz = 300.0, kWidthHighHz = 10000.0;
constexpr double kPlaysInRegionDb = -30.0; // a group under this share of a region does not play there
constexpr double kOwnRangeDb = 35.0;
constexpr double kDropRangeDb = 3.0;
constexpr double kMinimumDropSeconds = 8.0;
constexpr double kLevelToleranceDb = 1.5;
constexpr double kShapeToleranceDb = 2.0;
constexpr double kWidthTolerance = 0.06;
constexpr double kMaxLevelStepDb = 3.0;
constexpr double kBreakDrumsBelowDb = 15.0; // drums this far under their drop level: a break
constexpr double kBreakRangeDb = 20.0;      // the rest still plays: within this of the drop
constexpr int kDrumsSmoothBlocks = 10;      // +-1 s, so the gaps between kicks do not count
constexpr double kExtraGroupDb = -25.0; // a group the references lack must be at least this loud to be named
constexpr double kContrastToleranceDb = 1.5;
// Phase 16: sections around the drop. A drop comes in after at least 4 s without one; the
// short-term loudness (3 s) finds it about 1.5 s late, so the entry is moved back by that.
constexpr int kEntryGapBlocks = 40;
constexpr int kLoudnessLagBlocks = 15;
constexpr int kBuildBlocks = 80;            // the build: the last 8 s before a drop
constexpr double kMinimumBuildSeconds = 4.0;
constexpr double kSectionRangeDb = 30.0;    // playing at all: within this of the drop

// File format: the section's line and the first letter of its group / spectrum lines.
const char* sectionKey(StemSection s)
{
    return s == StemSection::intro ? "intro" : s == StemSection::build ? "build" : "break";
}
char sectionPrefix(StemSection s)
{
    return s == StemSection::intro ? 'i' : s == StemSection::build ? 'u' : 'b';
}
// The section a line kind names: its key ("break") with what = 0, or one of its group /
// spectrum lines ("bgroup", "bmid", "bside") with what = 1; -1 = none.
int sectionOfKind(const std::string& kind, int what)
{
    for (int s = 0; s < kStemSections; ++s)
    {
        const auto section = static_cast<StemSection>(s);
        if (what == 0 && kind == sectionKey(section))
            return s;
        if (what == 1 && kind.size() > 1 && kind[0] == sectionPrefix(section)
            && (kind.compare(1, std::string::npos, "group") == 0 || kind.compare(1, std::string::npos, "mid") == 0
                || kind.compare(1, std::string::npos, "side") == 0))
            return s;
    }
    return -1;
}

constexpr std::array<double, kStemRegions + 1> kRegionEdges { 30.0, 90.0, 250.0, 700.0, 2000.0, 5500.0, 16000.0 };

int numBands()
{
    return SpectrumAnalyzer::numBandsFor(BandScale::erb);
}

double hzOf(int band)
{
    return SpectrumAnalyzer::bandCentreHz(BandScale::erb, band);
}

double toPower(double db)
{
    return db > kAbsentDb + 1.0 ? std::pow(10.0, db / 10.0) : 0.0;
}

double toDb(double power)
{
    return power > 1e-20 ? 10.0 * std::log10(power) : kAbsentDb;
}

int regionOf(double hz)
{
    for (int r = 0; r < kStemRegions; ++r)
        if (hz >= kRegionEdges[static_cast<size_t>(r)] && hz < kRegionEdges[static_cast<size_t>(r + 1)])
            return r;
    return -1;
}

double earWeight(double hz)
{
    return std::pow(10.0, -equalLoudnessWeightDb(hz) / 10.0);
}

std::string lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool contains(const std::string& text, const char* word)
{
    return text.find(word) != std::string::npos;
}

int groupFromWords(const std::string& text)
{
    // Vocals first ("vocal bass" is a vocal), the full mix last ("drum mix" is drums).
    for (const char* w : { "vocal", "vox", "voice", "wokal", "acapella", "a cappella", "spiew" })
        if (contains(text, w))
            return static_cast<int>(StemGroup::vocals);
    for (const char* w : { "drum", "perc", "kick", "snare", "hat", "perkus", "beben", "beat", "stopa" })
        if (contains(text, w))
            return static_cast<int>(StemGroup::drums);
    for (const char* w : { "bass", "bas", "sub", "808" })
        if (contains(text, w))
            return static_cast<int>(StemGroup::bass);
    for (const char* w : { "other", "melod", "music", "instrument", "inst", "synth", "piano", "guitar", "gitar", "keys",
                           "lead", "pad", "chord", "akord", "pozosta", "reszta", "inne" })
        if (contains(text, w))
            return static_cast<int>(StemGroup::music);
    for (const char* w : { "mix", "full", "master", "orig", "calosc" })
        if (contains(text, w))
            return kStemFullMix;
    return kStemUnknown;
}

double quantile(std::vector<double> values, double q)
{
    std::sort(values.begin(), values.end());
    const double position = q * static_cast<double>(values.size() - 1);
    const auto below = static_cast<size_t>(std::floor(position));
    const auto above = std::min(below + 1, values.size() - 1);
    const double t = position - static_cast<double>(below);
    return values[below] * (1.0 - t) + values[above] * t;
}

StemCorridor corridorOf(const std::vector<double>& values, double tolerance)
{
    StemCorridor c;
    if (values.empty())
        return c;
    c.valid = true;
    c.centre = quantile(values, 0.5);
    c.low = std::min(c.centre - tolerance, quantile(values, 0.2));
    c.high = std::max(c.centre + tolerance, quantile(values, 0.8));
    return c;
}

double shapeTolerance(int region)
{
    // References disagree more at the extremes.
    return region == 0 || region == kStemRegions - 1 ? kShapeToleranceDb + 1.0 : kShapeToleranceDb;
}

// Ear-weighted power of every group and of their sum.
std::array<double, kStemGroups> weightedPowers(const std::array<SpectrumAnalyzer::Bands, kStemGroups>& groupDb,
                                               const std::array<double, kStemGroups>& gainDb)
{
    std::array<double, kStemGroups> out {};
    for (int b = 0; b < numBands(); ++b)
    {
        const double hz = hzOf(b);
        if (hz < kLevelLowHz || hz > kLevelHighHz)
            continue;
        const double w = earWeight(hz);
        for (int g = 0; g < kStemGroups; ++g)
            out[static_cast<size_t>(g)] += toPower(groupDb[static_cast<size_t>(g)][static_cast<size_t>(b)] + gainDb[static_cast<size_t>(g)]) * w;
    }
    return out;
}

std::string number(double value, int decimals = 4)
{
    std::ostringstream out;
    out.precision(decimals);
    out << std::fixed << value;
    return out.str();
}

bool bandsPresent(const SpectrumAnalyzer::Bands& db)
{
    return *std::max_element(db.begin(), db.begin() + numBands()) > -120.0;
}

StemMeasure measureBands(const std::array<SpectrumAnalyzer::Bands, kStemGroups>& groupDb,
                         const std::array<SpectrumAnalyzer::Bands, kStemGroups>& sideDb)
{
    std::array<bool, kStemGroups> present {};
    for (int g = 0; g < kStemGroups; ++g)
        present[static_cast<size_t>(g)] = bandsPresent(groupDb[static_cast<size_t>(g)]);
    return measureStemGroups(groupDb, sideDb, present);
}

// Each group's mid and side as the analysers averaged them (-200 for a group not heard).
void readAnalysers(const std::array<StereoAnalyzer, kStemGroups>& analysers, const std::array<bool, kStemGroups>& seen,
                   std::array<SpectrumAnalyzer::Bands, kStemGroups>& groupDb, std::array<SpectrumAnalyzer::Bands, kStemGroups>& sideDb)
{
    for (int g = 0; g < kStemGroups; ++g)
    {
        const auto& analyser = analysers[static_cast<size_t>(g)];
        groupDb[static_cast<size_t>(g)].fill(kAbsentDb);
        sideDb[static_cast<size_t>(g)].fill(kAbsentDb);
        if (!seen[static_cast<size_t>(g)] || analyser.framesAnalysed() == 0)
            continue;
        const auto midDb = analyser.averageMidDb();
        const auto sDb = analyser.averageSideDb();
        for (int b = 0; b < numBands(); ++b)
        {
            groupDb[static_cast<size_t>(g)][static_cast<size_t>(b)] = std::max(kAbsentDb, midDb[static_cast<size_t>(b)]);
            sideDb[static_cast<size_t>(g)][static_cast<size_t>(b)] = std::max(kAbsentDb, sDb[static_cast<size_t>(b)]);
        }
    }
}

void writeGroupLine(std::ostream& out, const char* kind, int g, const StemGroupMeasure& m);
bool readGroupLine(std::istringstream& words, StemMeasure& measure);
void writeGroupLine(std::ostream& out, const char* kind, int g, const StemGroupMeasure& m)
{
    out << kind << " " << stemGroupKey(static_cast<StemGroup>(g)) << " " << (m.present ? 1 : 0) << " " << number(m.levelDb) << " "
        << (m.widthValid ? number(m.width) : std::string("-"));
    for (int r = 0; r < kStemRegions; ++r)
        out << " " << (m.shapeValid[static_cast<size_t>(r)] ? number(m.shapeDb[static_cast<size_t>(r)]) : std::string("-"));
    out << "\n";
}

bool readGroupLine(std::istringstream& words, StemMeasure& measure)
{
    std::string key, widthText;
    int present = 0;
    double levelDb = 0.0;
    words >> key >> present >> levelDb >> widthText;
    if (words.fail())
        return false;
    int g = -1;
    for (int k = 0; k < kStemGroups; ++k)
        if (key == stemGroupKey(static_cast<StemGroup>(k)))
            g = k;
    if (g < 0)
        return false;
    auto& m = measure.groups[static_cast<size_t>(g)];
    m.present = present != 0;
    m.levelDb = levelDb;
    m.widthValid = widthText != "-";
    m.width = m.widthValid ? std::strtod(widthText.c_str(), nullptr) : 0.0;
    for (int r = 0; r < kStemRegions; ++r)
    {
        std::string value;
        words >> value;
        if (words.fail())
            return false;
        m.shapeValid[static_cast<size_t>(r)] = value != "-";
        m.shapeDb[static_cast<size_t>(r)] = m.shapeValid[static_cast<size_t>(r)] ? std::strtod(value.c_str(), nullptr) : 0.0;
    }
    return true;
}
} // namespace

const char* stemGroupKey(StemGroup group)
{
    switch (group)
    {
        case StemGroup::drums: return "drums";
        case StemGroup::bass: return "bass";
        case StemGroup::vocals: return "vocals";
        case StemGroup::music: return "music";
    }
    return "music";
}

std::vector<int> stemGroupsFromNames(const std::vector<std::string>& fileNames)
{
    std::vector<std::string> names;
    for (const auto& file : fileNames)
    {
        auto name = lower(file);
        const auto slash = name.find_last_of("/\\");
        if (slash != std::string::npos)
            name = name.substr(slash + 1);
        const auto dot = name.find_last_of('.');
        if (dot != std::string::npos && dot > 0)
            name = name.substr(0, dot);
        names.push_back(name);
    }

    // Drop what every name shares at the start and the end: the track's title.
    size_t prefix = 0, suffix = 0;
    if (names.size() > 1)
    {
        size_t shortest = names.front().size();
        for (const auto& n : names)
            shortest = std::min(shortest, n.size());
        while (prefix < shortest
               && std::all_of(names.begin(), names.end(), [&](const auto& n) { return n[prefix] == names.front()[prefix]; }))
            ++prefix;
        while (suffix < shortest - prefix
               && std::all_of(names.begin(), names.end(), [&](const auto& n) {
                      return n[n.size() - 1 - suffix] == names.front()[names.front().size() - 1 - suffix];
                  }))
            ++suffix;
    }

    std::vector<int> groups;
    for (const auto& n : names)
    {
        const auto own = n.substr(prefix, n.size() - prefix - suffix);
        int group = groupFromWords(own);
        if (group == kStemUnknown && (prefix > 0 || suffix > 0))
            group = groupFromWords(n); // the common part cut into the word itself ("drums" / "dry")
        groups.push_back(group);
    }
    return groups;
}

bool stemGroupHasKey(int group)
{
    return group == static_cast<int>(StemGroup::bass) || group == static_cast<int>(StemGroup::vocals)
           || group == static_cast<int>(StemGroup::music);
}

int stemGroupForRole(Role role)
{
    switch (role)
    {
        case Role::kick:
        case Role::percussion: return static_cast<int>(StemGroup::drums);
        case Role::bass: return static_cast<int>(StemGroup::bass);
        case Role::vocal: return static_cast<int>(StemGroup::vocals);
        case Role::lead:
        case Role::pad:
        case Role::effects:
        case Role::other: return static_cast<int>(StemGroup::music);
        case Role::bus:
        case Role::send: return -1;
    }
    return -1;
}

double stemRegionEdgeHz(int edge)
{
    return kRegionEdges[static_cast<size_t>(std::clamp(edge, 0, kStemRegions))];
}

StemMeasure measureStemGroups(const std::array<SpectrumAnalyzer::Bands, kStemGroups>& groupDb,
                              const std::array<SpectrumAnalyzer::Bands, kStemGroups>& sideDb,
                              const std::array<bool, kStemGroups>& present)
{
    StemMeasure out;
    std::array<SpectrumAnalyzer::Bands, kStemGroups> used {};
    for (int g = 0; g < kStemGroups; ++g)
    {
        used[static_cast<size_t>(g)].fill(kAbsentDb);
        if (present[static_cast<size_t>(g)])
            used[static_cast<size_t>(g)] = groupDb[static_cast<size_t>(g)];
    }

    const auto weighted = weightedPowers(used, {});
    double total = 0.0;
    for (double p : weighted)
        total += p;
    if (total <= 0.0)
        return out;
    out.valid = true;

    std::array<std::array<double, kStemRegions>, kStemGroups> regionPower {};
    std::array<double, kStemRegions> regionTotal {};
    std::array<double, kStemGroups> mid {}, side {};
    for (int b = 0; b < numBands(); ++b)
    {
        const double hz = hzOf(b);
        const int r = regionOf(hz);
        for (int g = 0; g < kStemGroups; ++g)
        {
            const double p = toPower(used[static_cast<size_t>(g)][static_cast<size_t>(b)]);
            if (r >= 0)
            {
                regionPower[static_cast<size_t>(g)][static_cast<size_t>(r)] += p;
                regionTotal[static_cast<size_t>(r)] += p;
            }
            if (hz >= kWidthLowHz && hz <= kWidthHighHz && present[static_cast<size_t>(g)])
            {
                mid[static_cast<size_t>(g)] += p;
                side[static_cast<size_t>(g)] += toPower(sideDb[static_cast<size_t>(g)][static_cast<size_t>(b)]);
            }
        }
    }
    const double loudestRegion = *std::max_element(regionTotal.begin(), regionTotal.end());

    for (int g = 0; g < kStemGroups; ++g)
    {
        auto& m = out.groups[static_cast<size_t>(g)];
        const double own = weighted[static_cast<size_t>(g)];
        // A group 40 dB under the rest is not really there.
        m.present = present[static_cast<size_t>(g)] && own > total * 1e-4;
        if (!m.present)
            continue;
        m.levelDb = toDb(own) - toDb(total);
        // The group's own tonal balance: its level in each region against its average over the
        // regions it plays in (like a tonal balance meter on the stem alone), apart from its level
        // in the mix, so a thin vocal shows on the vocal and not as too much of everything else.
        std::array<double, kStemRegions> regionDb {};
        double loudestOwn = kAbsentDb;
        for (int r = 0; r < kStemRegions; ++r)
        {
            regionDb[static_cast<size_t>(r)] = toDb(regionPower[static_cast<size_t>(g)][static_cast<size_t>(r)]);
            loudestOwn = std::max(loudestOwn, regionDb[static_cast<size_t>(r)]);
        }
        double mean = 0.0;
        int count = 0;
        for (int r = 0; r < kStemRegions; ++r)
        {
            const double t = regionTotal[static_cast<size_t>(r)];
            // It plays there (within 35 dB of its loudest region, a real part of the region),
            // and the mix itself has something there (within 50 dB of its loudest region).
            const double share = t > 0.0 ? regionDb[static_cast<size_t>(r)] - toDb(t) : kAbsentDb;
            m.shapeValid[static_cast<size_t>(r)] = regionDb[static_cast<size_t>(r)] >= loudestOwn - kOwnRangeDb
                                                   && share >= kPlaysInRegionDb && t > loudestRegion * 1e-5;
            if (m.shapeValid[static_cast<size_t>(r)])
            {
                mean += regionDb[static_cast<size_t>(r)];
                ++count;
            }
        }
        mean = count > 0 ? mean / count : 0.0;
        for (int r = 0; r < kStemRegions; ++r)
            m.shapeDb[static_cast<size_t>(r)] = m.shapeValid[static_cast<size_t>(r)] ? regionDb[static_cast<size_t>(r)] - mean : 0.0;
        const double both = mid[static_cast<size_t>(g)] + side[static_cast<size_t>(g)];
        m.widthValid = both > 0.0 && side[static_cast<size_t>(g)] > 0.0;
        m.width = m.widthValid ? side[static_cast<size_t>(g)] / both : 0.0;
    }
    return out;
}

void StemProfiler::prepare(double rate, bool haveFullMix)
{
    sampleRate = rate;
    block = std::max(1, static_cast<int>(std::lround(rate * 0.1)));
    fullMix = haveFullMix;
    meter.prepare(rate, 2);
    meter.reset();
    keys.prepare(rate);
    loudness.clear();
    drums.clear();
    drop.clear();
    for (auto& g : groups)
    {
        g.prepare(rate);
        g.reset();
    }
    for (auto& set : sectionGroups)
        for (auto& g : set)
        {
            g.prepare(rate);
            g.reset();
        }
    for (auto& s : sections)
        s.clear();
    seen = {};
    sectionSeen = {};
    sectionBlocks = {};
    sum.prepare(rate, BandScale::erb);
    sum.reset();
    mix.prepare(rate, BandScale::erb);
    mix.reset();
    sumLeft.assign(static_cast<size_t>(block), 0.0f);
    sumRight.assign(static_cast<size_t>(block), 0.0f);
    dropBlocks = 0;
}

void StemProfiler::measureBlock(const float* left, const float* right, int numSamples, const float* keyLeft,
                                const float* keyRight)
{
    const float* channels[] { left, right };
    meter.process(channels, 2, numSamples);
    loudness.push_back(meter.shortTermLufs());
    // The key over the whole track, not only the drop: more notes, a surer guess.
    const float* keyChannels[] { keyLeft != nullptr ? keyLeft : left, keyRight != nullptr ? keyRight : (keyLeft != nullptr ? keyLeft : right) };
    keys.process(keyChannels, 2, numSamples);
}

void StemProfiler::chooseDrop()
{
    double loudest = -INFINITY;
    for (double l : loudness)
        if (std::isfinite(l))
            loudest = std::max(loudest, l);
    drop.assign(loudness.size(), 0);
    dropBlocks = 0;
    if (!std::isfinite(loudest))
        return;
    const double blockSeconds = static_cast<double>(block) / sampleRate;
    // The loudest part; a track without a clear one gets a wider window.
    for (double range : { kDropRangeDb, 2.0 * kDropRangeDb, 70.0 })
    {
        dropBlocks = 0;
        for (size_t i = 0; i < loudness.size(); ++i)
        {
            drop[i] = std::isfinite(loudness[i]) && loudness[i] >= loudest - range ? 1 : 0;
            dropBlocks += drop[i];
        }
        if (dropBlocks * blockSeconds >= kMinimumDropSeconds)
            break;
    }

    for (auto& s : sections)
        s.assign(loudness.size(), 0);
    sectionBlocks = {};
    if (dropBlocks == 0)
        return;
    double dropLoudness = 0.0;
    for (size_t i = 0; i < drop.size(); ++i)
        dropLoudness += drop[i] != 0 ? loudness[i] : 0.0;
    dropLoudness /= dropBlocks;
    auto playing = [&](size_t i) { return std::isfinite(loudness[i]) && loudness[i] >= dropLoudness - kSectionRangeDb; };
    auto keep = [&](StemSection section, double minimumSeconds) {
        auto& mask = sections[static_cast<size_t>(section)];
        int count = 0;
        for (char m : mask)
            count += m;
        if (count * blockSeconds < minimumSeconds)
        {
            std::fill(mask.begin(), mask.end(), 0);
            count = 0;
        }
        sectionBlocks[static_cast<size_t>(section)] = count;
    };

    // Phase 16: where the drops come in, the build before each, the intro before the first.
    std::vector<size_t> entries;
    long lastDrop = -1000000;
    for (size_t i = 0; i < drop.size(); ++i)
        if (drop[i] != 0)
        {
            if (static_cast<long>(i) - lastDrop > kEntryGapBlocks)
                entries.push_back(i > static_cast<size_t>(kLoudnessLagBlocks) ? i - kLoudnessLagBlocks : 0);
            lastDrop = static_cast<long>(i);
        }
    auto& build = sections[static_cast<size_t>(StemSection::build)];
    for (size_t entry : entries)
        for (size_t i = entry > static_cast<size_t>(kBuildBlocks) ? entry - kBuildBlocks : 0; i < entry; ++i)
            build[i] = drop[i] == 0 && playing(i) ? 1 : 0;
    keep(StemSection::build, kMinimumBuildSeconds);
    const size_t firstEntry = entries.empty() ? 0 : entries.front();
    auto& intro = sections[static_cast<size_t>(StemSection::intro)];
    for (size_t i = 0; i < firstEntry; ++i)
        intro[i] = drop[i] == 0 && build[i] == 0 && playing(i) ? 1 : 0;
    keep(StemSection::intro, kMinimumDropSeconds);

    // The break: after the first drop, the drums gone (smoothed over two seconds, so the gaps
    // between kicks do not count), the rest still playing. Defined by what plays, so it may
    // hold a build too (the end of a breakdown rising into the drop).
    if (drums.empty())
        return;
    drums.resize(loudness.size(), 0.0);
    std::vector<double> smooth(drums.size(), 0.0);
    for (size_t i = 0; i < drums.size(); ++i)
    {
        const size_t from = i >= static_cast<size_t>(kDrumsSmoothBlocks) ? i - kDrumsSmoothBlocks : 0;
        const size_t to = std::min(drums.size() - 1, i + kDrumsSmoothBlocks);
        double sum = 0.0;
        for (size_t j = from; j <= to; ++j)
            sum += drums[j];
        smooth[i] = sum / static_cast<double>(to - from + 1);
    }
    double dropDrums = 0.0;
    for (size_t i = 0; i < drop.size(); ++i)
        if (drop[i] != 0)
            dropDrums += smooth[i];
    dropDrums /= dropBlocks;
    if (dropDrums <= 0.0)
        return;
    const double quietDrums = dropDrums * std::pow(10.0, -kBreakDrumsBelowDb / 10.0);
    auto& breakdown = sections[static_cast<size_t>(StemSection::breakdown)];
    for (size_t i = firstEntry; i < loudness.size(); ++i)
        breakdown[i] = drop[i] == 0 && std::isfinite(loudness[i]) && loudness[i] >= dropLoudness - kBreakRangeDb
                               && smooth[i] <= quietDrums
                           ? 1
                           : 0;
    keep(StemSection::breakdown, kMinimumDropSeconds);
}

void StemProfiler::measureDrums(const float* left, const float* right, int numSamples)
{
    double power = 0.0;
    for (int i = 0; i < numSamples; ++i)
        power += 0.5 * (static_cast<double>(left[i]) * left[i] + static_cast<double>(right[i]) * right[i]);
    // One value per block, in step with measureBlock.
    drums.resize(loudness.size(), 0.0);
    if (!drums.empty())
        drums.back() = numSamples > 0 ? power / numSamples : 0.0;
}

bool StemProfiler::inSection(StemSection section, int blockIndex) const
{
    const auto& mask = sections[static_cast<size_t>(section)];
    return blockIndex >= 0 && blockIndex < static_cast<int>(mask.size()) && mask[static_cast<size_t>(blockIndex)] != 0;
}

bool StemProfiler::needed(int blockIndex) const
{
    if (inDrop(blockIndex))
        return true;
    for (int s = 0; s < kStemSections; ++s)
        if (inSection(static_cast<StemSection>(s), blockIndex))
            return true;
    return false;
}

bool StemProfiler::inDrop(int blockIndex) const
{
    return blockIndex >= 0 && blockIndex < static_cast<int>(drop.size()) && drop[static_cast<size_t>(blockIndex)] != 0;
}

void StemProfiler::analyseBlock(int blockIndex, const std::array<const float*, kStemGroups>& left,
                                const std::array<const float*, kStemGroups>& right, const float* mixLeft,
                                const float* mixRight, int numSamples)
{
    for (int s = 0; s < kStemSections; ++s)
    {
        if (!inSection(static_cast<StemSection>(s), blockIndex))
            continue;
        numSamples = std::min(numSamples, block);
        for (int g = 0; g < kStemGroups; ++g)
        {
            const float* l = left[static_cast<size_t>(g)];
            if (l == nullptr)
                continue;
            const float* r = right[static_cast<size_t>(g)] != nullptr ? right[static_cast<size_t>(g)] : l;
            sectionGroups[static_cast<size_t>(s)][static_cast<size_t>(g)].process(l, r, numSamples);
            sectionSeen[static_cast<size_t>(s)][static_cast<size_t>(g)] = true;
        }
        return;
    }
    if (!inDrop(blockIndex))
        return;
    numSamples = std::min(numSamples, block);
    std::fill(sumLeft.begin(), sumLeft.end(), 0.0f);
    std::fill(sumRight.begin(), sumRight.end(), 0.0f);
    for (int g = 0; g < kStemGroups; ++g)
    {
        const float* l = left[static_cast<size_t>(g)];
        const float* r = right[static_cast<size_t>(g)] != nullptr ? right[static_cast<size_t>(g)] : l;
        if (l == nullptr)
            continue;
        groups[static_cast<size_t>(g)].process(l, r, numSamples);
        seen[static_cast<size_t>(g)] = true;
        for (int i = 0; i < numSamples; ++i)
        {
            sumLeft[static_cast<size_t>(i)] += l[i];
            sumRight[static_cast<size_t>(i)] += r[i];
        }
    }
    const float* sumChannels[] { sumLeft.data(), sumRight.data() };
    sum.process(sumChannels, 2, numSamples);
    if (fullMix && mixLeft != nullptr)
    {
        const float* mixChannels[] { mixLeft, mixRight != nullptr ? mixRight : mixLeft };
        mix.process(mixChannels, 2, numSamples);
    }
}

StemProfile StemProfiler::finish(const std::string& name) const
{
    StemProfile profile;
    profile.name = name;
    profile.dropSeconds = dropBlocks * static_cast<double>(block) / sampleRate;

    readAnalysers(groups, seen, profile.groupDb, profile.sideDb);
    profile.measure = measureBands(profile.groupDb, profile.sideDb);
    profile.hasBands = true;

    for (int s = 0; s < kStemSections; ++s)
    {
        const auto at = static_cast<size_t>(s);
        if (sectionBlocks[at] == 0)
            continue;
        auto& b = profile.section(static_cast<StemSection>(s));
        readAnalysers(sectionGroups[at], sectionSeen[at], b.groupDb, b.sideDb);
        b.measure = measureBands(b.groupDb, b.sideDb);
        b.valid = b.measure.valid;
        b.seconds = sectionBlocks[at] * static_cast<double>(block) / sampleRate;
        double dropLoudness = 0.0, sectionLoudness = 0.0;
        for (size_t i = 0; i < loudness.size(); ++i)
        {
            dropLoudness += drop[i] != 0 ? loudness[i] : 0.0;
            sectionLoudness += sections[at][i] != 0 ? loudness[i] : 0.0;
        }
        b.belowDropDb = dropLoudness / std::max(1, dropBlocks) - sectionLoudness / sectionBlocks[at];
    }
    const auto key = keys.estimate();
    if (key.confident)
    {
        profile.key = key.key;
        profile.keyCorrelation = key.correlation;
    }

    // Do the stems add up to the track? Missing stems or a poor separation show here.
    if (fullMix && mix.framesAnalysed() > 0 && sum.framesAnalysed() > 0)
    {
        const auto mixDb = mix.averageDb();
        const auto sumDb = sum.averageDb();
        double loudest = -INFINITY;
        for (int b = 0; b < numBands(); ++b)
            loudest = std::max(loudest, mixDb[static_cast<size_t>(b)]);
        double distance = 0.0;
        int count = 0;
        for (int b = 0; b < numBands(); ++b)
        {
            const double hz = hzOf(b);
            if (hz < 40.0 || hz > 16000.0 || mixDb[static_cast<size_t>(b)] < loudest - 40.0)
                continue;
            distance += std::abs(std::max(kAbsentDb, sumDb[static_cast<size_t>(b)]) - mixDb[static_cast<size_t>(b)]);
            ++count;
        }
        profile.separationErrorDb = count > 0 ? distance / count : -1.0;
    }
    return profile;
}

void StemTarget::rebuild()
{
    level = {};
    shape = {};
    width = {};
    for (int g = 0; g < kStemGroups; ++g)
    {
        std::vector<double> levels, widths;
        std::array<std::vector<double>, kStemRegions> shapes;
        for (const auto& p : profiles)
        {
            const auto& m = p.measure.groups[static_cast<size_t>(g)];
            if (!p.measure.valid || !m.present)
                continue;
            levels.push_back(m.levelDb);
            if (m.widthValid)
                widths.push_back(m.width);
            for (int r = 0; r < kStemRegions; ++r)
                if (m.shapeValid[static_cast<size_t>(r)])
                    shapes[static_cast<size_t>(r)].push_back(m.shapeDb[static_cast<size_t>(r)]);
        }
        level[static_cast<size_t>(g)] = corridorOf(levels, kLevelToleranceDb);
        width[static_cast<size_t>(g)] = corridorOf(widths, kWidthTolerance);
        for (int r = 0; r < kStemRegions; ++r)
            shape[static_cast<size_t>(g)][static_cast<size_t>(r)] = corridorOf(shapes[static_cast<size_t>(r)], shapeTolerance(r));
    }
    for (int s = 0; s < kStemSections; ++s)
    {
        std::vector<double> contrasts;
        for (const auto& p : profiles)
            if (p.section(static_cast<StemSection>(s)).valid)
                contrasts.push_back(p.section(static_cast<StemSection>(s)).belowDropDb);
        const auto corridor = corridorOf(contrasts, kContrastToleranceDb);
        (s == 0 ? introContrast : s == 1 ? buildContrast : breakContrast) = corridor;
    }
}

std::string serialiseStemTarget(const StemTarget& target)
{
    std::ostringstream out;
    out << target.name << "\n";
    for (const auto& p : target.profiles)
    {
        out << "profile " << number(p.dropSeconds) << " " << number(p.separationErrorDb) << " " << p.name << "\n";
        if (p.key.valid())
            out << "key " << p.key.tonic << " " << (p.key.minor ? 1 : 0) << " " << number(p.keyCorrelation) << "\n";
        if (p.hasBands)
            for (int g = 0; g < kStemGroups; ++g)
                for (int which = 0; which < 2; ++which)
                {
                    const auto& db = (which == 0 ? p.groupDb : p.sideDb)[static_cast<size_t>(g)];
                    out << (which == 0 ? "mid " : "side ") << stemGroupKey(static_cast<StemGroup>(g));
                    for (int b = 0; b < numBands(); ++b)
                        out << " " << number(std::max(kAbsentDb, db[static_cast<size_t>(b)]), 2);
                    out << "\n";
                }
        for (int g = 0; g < kStemGroups; ++g)
            writeGroupLine(out, "group", g, p.measure.groups[static_cast<size_t>(g)]);
        for (int s = 0; s < kStemSections; ++s)
        {
            const auto section = static_cast<StemSection>(s);
            const auto& b = p.section(section);
            if (!b.valid)
                continue;
            const std::string prefix(1, sectionPrefix(section));
            out << sectionKey(section) << " " << number(b.seconds) << " " << number(b.belowDropDb) << "\n";
            for (int g = 0; g < kStemGroups; ++g)
                writeGroupLine(out, (prefix + "group").c_str(), g, b.measure.groups[static_cast<size_t>(g)]);
            for (int g = 0; g < kStemGroups; ++g)
                for (int which = 0; which < 2; ++which)
                {
                    const auto& db = (which == 0 ? b.groupDb : b.sideDb)[static_cast<size_t>(g)];
                    out << prefix << (which == 0 ? "mid " : "side ") << stemGroupKey(static_cast<StemGroup>(g));
                    for (int band = 0; band < numBands(); ++band)
                        out << " " << number(std::max(kAbsentDb, db[static_cast<size_t>(band)]), 2);
                    out << "\n";
                }
        }
    }
    return out.str();
}

bool parseStemTarget(const std::string& text, StemTarget& target)
{
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line))
        return false;
    StemTarget parsed;
    parsed.name = line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::istringstream words(line);
        std::string kind;
        words >> kind;
        if (kind == "profile")
        {
            StemProfile p;
            words >> p.dropSeconds >> p.separationErrorDb;
            if (words.fail())
                return false;
            std::getline(words >> std::ws, p.name);
            p.measure.valid = true;
            for (auto* set : { &p.groupDb, &p.sideDb })
                for (auto& db : *set)
                    db.fill(kAbsentDb);
            parsed.profiles.push_back(p);
        }
        else if (kind == "key")
        {
            if (parsed.profiles.empty())
                return false;
            int tonic = -1, minor = 0;
            double correlation = 0.0;
            words >> tonic >> minor >> correlation;
            if (words.fail() || tonic < 0 || tonic >= 12)
                return false;
            parsed.profiles.back().key = { tonic, minor != 0 };
            parsed.profiles.back().keyCorrelation = correlation;
        }
        else if (kind == "mid" || kind == "side" || kind == "group" || sectionOfKind(kind, 1) >= 0)
        {
            if (parsed.profiles.empty())
                return false;
            auto& p = parsed.profiles.back();
            // "mid", "side", "group" for the drop; with a section's letter in front for that section.
            const int s = kind == "mid" || kind == "side" || kind == "group" ? -1 : sectionOfKind(kind, 1);
            const std::string rest = s < 0 ? kind : kind.substr(1);
            StemProfile::Section* section = s < 0 ? nullptr : &p.section(static_cast<StemSection>(s));
            if (section != nullptr && !section->valid)
                return false;
            if (rest == "group")
            {
                if (!readGroupLine(words, section != nullptr ? section->measure : p.measure))
                    return false;
                continue;
            }
            std::string key;
            words >> key;
            int g = -1;
            for (int k = 0; k < kStemGroups; ++k)
                if (key == stemGroupKey(static_cast<StemGroup>(k)))
                    g = k;
            if (g < 0)
                return false;
            const bool mid = rest == "mid";
            auto& set = section != nullptr ? (mid ? section->groupDb : section->sideDb) : (mid ? p.groupDb : p.sideDb);
            auto& db = set[static_cast<size_t>(g)];
            for (int b = 0; b < numBands(); ++b)
            {
                words >> db[static_cast<size_t>(b)];
                if (words.fail())
                    return false;
            }
            if (section == nullptr)
                p.hasBands = true;
        }
        else if (sectionOfKind(kind, 0) >= 0)
        {
            if (parsed.profiles.empty())
                return false;
            auto& b = parsed.profiles.back().section(static_cast<StemSection>(sectionOfKind(kind, 0)));
            words >> b.seconds >> b.belowDropDb;
            if (words.fail())
                return false;
            b.valid = true;
            b.measure.valid = true;
            for (auto* set : { &b.groupDb, &b.sideDb })
                for (auto& db : *set)
                    db.fill(kAbsentDb);
        }
        else if (!kind.empty())
        {
            return false;
        }
    }
    parsed.rebuild();
    target = std::move(parsed);
    return true;
}

StemTarget keyedStemTarget(const StemTarget& target, MusicalKey trackKey)
{
    StemTarget out = target;
    bool moved = false;
    for (auto& p : out.profiles)
    {
        p.shiftSemitones = 0;
        if (!trackKey.valid() || !p.key.valid() || !p.hasBands)
            continue;
        const int shift = keyShiftSemitones(p.key, trackKey);
        if (shift == 0)
            continue;
        for (int g = 0; g < kStemGroups; ++g)
            if (stemGroupHasKey(g))
                for (auto* set : { &p.groupDb, &p.sideDb, &p.breakdown.groupDb, &p.breakdown.sideDb, &p.intro.groupDb,
                                   &p.intro.sideDb, &p.build.groupDb, &p.build.sideDb })
                    (*set)[static_cast<size_t>(g)] = shiftErbSpectrum((*set)[static_cast<size_t>(g)], shift);
        p.measure = measureBands(p.groupDb, p.sideDb);
        for (auto* section : { &p.breakdown, &p.intro, &p.build })
            if (section->valid)
                section->measure = measureBands(section->groupDb, section->sideDb);
        p.shiftSemitones = shift;
        moved = true;
    }
    if (moved)
        out.rebuild();
    return out;
}

StemTarget stemBreakTarget(const StemTarget& target)
{
    return stemSectionTarget(target, StemSection::breakdown);
}

StemTarget stemSectionTarget(const StemTarget& target, StemSection section)
{
    StemTarget out;
    out.name = target.name;
    for (const auto& p : target.profiles)
    {
        const auto& from = p.section(section);
        if (!from.valid)
            continue;
        StemProfile b = p;
        b.measure = from.measure;
        b.groupDb = from.groupDb;
        b.sideDb = from.sideDb;
        b.dropSeconds = from.seconds;
        out.profiles.push_back(b);
    }
    out.rebuild();
    // The contrasts stay those of the full set: each section against its own drop.
    out.introContrast = target.introContrast;
    out.buildContrast = target.buildContrast;
    out.breakContrast = target.breakContrast;
    return out;
}

StemComparison compareStems(const StemMeasure& mix, const StemTarget& target)
{
    StemComparison out;
    if (!mix.valid || !target.valid())
        return out;
    out.valid = true;
    for (int g = 0; g < kStemGroups; ++g)
    {
        const auto& ours = mix.groups[static_cast<size_t>(g)];
        auto& c = out.groups[static_cast<size_t>(g)];
        const auto& lc = target.level[static_cast<size_t>(g)];
        if (!lc.valid)
        {
            // Tails of what just stopped and quiet leftovers do not count.
            c.extra = ours.present && ours.levelDb > kExtraGroupDb;
            c.levelDb = c.extra ? ours.levelDb : 0.0;
            continue;
        }
        if (!ours.present)
        {
            c.missing = true;
            continue;
        }
        c.compared = true;
        c.levelDb = ours.levelDb;
        c.levelDeviationDb = lc.outside(ours.levelDb);
        for (int r = 0; r < kStemRegions; ++r)
        {
            const auto& sc = target.shape[static_cast<size_t>(g)][static_cast<size_t>(r)];
            c.shapeCompared[static_cast<size_t>(r)] = sc.valid && ours.shapeValid[static_cast<size_t>(r)];
            c.shapeDeviationDb[static_cast<size_t>(r)] = c.shapeCompared[static_cast<size_t>(r)] ? sc.outside(ours.shapeDb[static_cast<size_t>(r)]) : 0.0;
        }
        const auto& wc = target.width[static_cast<size_t>(g)];
        c.widthCompared = wc.valid && ours.widthValid;
        c.width = ours.width;
        c.widthDeviation = c.widthCompared ? wc.outside(ours.width) : 0.0;
    }
    return out;
}

StemLevelPlan planStemLevels(const std::array<SpectrumAnalyzer::Bands, kStemGroups>& groupDb,
                             const std::array<bool, kStemGroups>& present, const StemTarget& target,
                             const std::array<bool, kStemGroups>* fixed)
{
    auto isFixed = [fixed](int g) { return fixed != nullptr && (*fixed)[static_cast<size_t>(g)]; };
    StemLevelPlan plan;
    std::array<SpectrumAnalyzer::Bands, kStemGroups> used {};
    for (int g = 0; g < kStemGroups; ++g)
    {
        used[static_cast<size_t>(g)].fill(kAbsentDb);
        if (present[static_cast<size_t>(g)])
            used[static_cast<size_t>(g)] = groupDb[static_cast<size_t>(g)];
    }

    auto levels = [&](const std::array<double, kStemGroups>& gain, double* total) {
        const auto w = weightedPowers(used, gain);
        double t = 0.0, real = 0.0;
        for (int g = 0; g < kStemGroups; ++g)
        {
            t += w[static_cast<size_t>(g)];
            real += isFixed(g) ? 0.0 : w[static_cast<size_t>(g)];
        }
        std::array<double, kStemGroups> rel {};
        for (int g = 0; g < kStemGroups; ++g)
            rel[static_cast<size_t>(g)] = t > 0.0 && w[static_cast<size_t>(g)] > 0.0 ? toDb(w[static_cast<size_t>(g)]) - toDb(t) : kAbsentDb;
        if (total != nullptr)
            *total = real;
        return rel;
    };

    double totalBefore = 0.0;
    plan.before = levels({}, &totalBefore);
    if (totalBefore <= 0.0 || !target.valid())
        return plan;

    std::array<bool, kStemGroups> active {};
    bool anyOutside = false;
    for (int g = 0; g < kStemGroups; ++g)
    {
        const auto& c = target.level[static_cast<size_t>(g)];
        active[static_cast<size_t>(g)] = c.valid && plan.before[static_cast<size_t>(g)] > -40.0 && !isFixed(g);
        // Half a dB of slack outside, so a measurement on the edge does not start a move.
        anyOutside = anyOutside || (active[static_cast<size_t>(g)] && std::abs(c.outside(plan.before[static_cast<size_t>(g)])) >= 0.5);
    }
    if (!anyOutside)
    {
        plan.after = plan.before;
        return plan;
    }

    std::array<double, kStemGroups> gain {};
    for (int iteration = 0; iteration < 24; ++iteration)
    {
        const auto rel = levels(gain, nullptr);
        double moved = 0.0;
        for (int g = 0; g < kStemGroups; ++g)
        {
            if (!active[static_cast<size_t>(g)])
                continue;
            const auto& c = target.level[static_cast<size_t>(g)];
            // Half a dB inside the corridor, so the next pass finds it inside.
            const double low = std::min(c.low + 0.5, c.centre), high = std::max(c.high - 0.5, c.centre);
            const double aim = std::clamp(rel[static_cast<size_t>(g)], low, high);
            const double step = aim - rel[static_cast<size_t>(g)];
            // The group moves its own share less than its gain: the sum moves with it.
            const double share = std::pow(10.0, rel[static_cast<size_t>(g)] / 10.0);
            const double newGain = std::clamp(gain[static_cast<size_t>(g)] + step / std::max(0.2, 1.0 - share),
                                              -kMaxLevelStepDb, kMaxLevelStepDb);
            moved += std::abs(newGain - gain[static_cast<size_t>(g)]);
            gain[static_cast<size_t>(g)] = newGain;
        }
        if (moved < 0.01)
            break;
    }

    // Keep the overall loudness (and so the headroom) where it was: the same shift on every
    // group leaves their balance as planned.
    double totalAfter = 0.0;
    levels(gain, &totalAfter);
    const double shift = totalAfter > 0.0 ? toDb(totalAfter) - toDb(totalBefore) : 0.0;
    for (int g = 0; g < kStemGroups; ++g)
        if (plan.before[static_cast<size_t>(g)] > kAbsentDb && !isFixed(g))
            gain[static_cast<size_t>(g)] = std::clamp(gain[static_cast<size_t>(g)] - shift, -kMaxLevelStepDb, kMaxLevelStepDb);

    for (int g = 0; g < kStemGroups; ++g)
    {
        auto& value = gain[static_cast<size_t>(g)];
        value = std::abs(value) < 0.5 ? 0.0 : std::round(value * 10.0) / 10.0;
        plan.any = plan.any || value != 0.0;
    }
    plan.gainDb = gain;
    plan.after = levels(gain, nullptr);
    return plan;
}

} // namespace ma
