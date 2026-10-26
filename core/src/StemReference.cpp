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

StemMeasure measureProfile(const StemProfile& profile)
{
    std::array<bool, kStemGroups> present {};
    for (int g = 0; g < kStemGroups; ++g)
        present[static_cast<size_t>(g)] = bandsPresent(profile.groupDb[static_cast<size_t>(g)]);
    return measureStemGroups(profile.groupDb, profile.sideDb, present);
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
    drop.clear();
    for (auto& g : groups)
    {
        g.prepare(rate);
        g.reset();
    }
    seen = {};
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
}

bool StemProfiler::inDrop(int blockIndex) const
{
    return blockIndex >= 0 && blockIndex < static_cast<int>(drop.size()) && drop[static_cast<size_t>(blockIndex)] != 0;
}

void StemProfiler::analyseBlock(int blockIndex, const std::array<const float*, kStemGroups>& left,
                                const std::array<const float*, kStemGroups>& right, const float* mixLeft,
                                const float* mixRight, int numSamples)
{
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

    std::array<SpectrumAnalyzer::Bands, kStemGroups> groupDb {}, sideDb {};
    std::array<bool, kStemGroups> present {};
    for (int g = 0; g < kStemGroups; ++g)
    {
        const auto& analyser = groups[static_cast<size_t>(g)];
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
        present[static_cast<size_t>(g)] = *std::max_element(groupDb[static_cast<size_t>(g)].begin(),
                                                            groupDb[static_cast<size_t>(g)].begin() + numBands()) > -120.0;
    }
    profile.measure = measureStemGroups(groupDb, sideDb, present);
    profile.groupDb = groupDb;
    profile.sideDb = sideDb;
    profile.hasBands = true;
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
        {
            const auto& m = p.measure.groups[static_cast<size_t>(g)];
            out << "group " << stemGroupKey(static_cast<StemGroup>(g)) << " " << (m.present ? 1 : 0) << " " << number(m.levelDb)
                << " " << (m.widthValid ? number(m.width) : std::string("-"));
            for (int r = 0; r < kStemRegions; ++r)
                out << " " << (m.shapeValid[static_cast<size_t>(r)] ? number(m.shapeDb[static_cast<size_t>(r)]) : std::string("-"));
            out << "\n";
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
        else if (kind == "mid" || kind == "side")
        {
            if (parsed.profiles.empty())
                return false;
            std::string key;
            words >> key;
            int g = -1;
            for (int k = 0; k < kStemGroups; ++k)
                if (key == stemGroupKey(static_cast<StemGroup>(k)))
                    g = k;
            if (g < 0)
                return false;
            auto& p = parsed.profiles.back();
            auto& db = (kind == "mid" ? p.groupDb : p.sideDb)[static_cast<size_t>(g)];
            for (int b = 0; b < numBands(); ++b)
            {
                words >> db[static_cast<size_t>(b)];
                if (words.fail())
                    return false;
            }
            p.hasBands = true;
        }
        else if (kind == "group")
        {
            if (parsed.profiles.empty())
                return false;
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
            auto& m = parsed.profiles.back().measure.groups[static_cast<size_t>(g)];
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
            {
                p.groupDb[static_cast<size_t>(g)] = shiftErbSpectrum(p.groupDb[static_cast<size_t>(g)], shift);
                p.sideDb[static_cast<size_t>(g)] = shiftErbSpectrum(p.sideDb[static_cast<size_t>(g)], shift);
            }
        p.measure = measureProfile(p);
        p.shiftSemitones = shift;
        moved = true;
    }
    if (moved)
        out.rebuild();
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
            continue;
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
                             const std::array<bool, kStemGroups>& present, const StemTarget& target)
{
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
        double t = 0.0;
        for (double p : w)
            t += p;
        std::array<double, kStemGroups> rel {};
        for (int g = 0; g < kStemGroups; ++g)
            rel[static_cast<size_t>(g)] = t > 0.0 && w[static_cast<size_t>(g)] > 0.0 ? toDb(w[static_cast<size_t>(g)]) - toDb(t) : kAbsentDb;
        if (total != nullptr)
            *total = t;
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
        active[static_cast<size_t>(g)] = c.valid && plan.before[static_cast<size_t>(g)] > -40.0;
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
        if (plan.before[static_cast<size_t>(g)] > kAbsentDb)
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
