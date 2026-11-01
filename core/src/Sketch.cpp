#include "ma/Sketch.h"

#include "ma/EqualLoudness.h"

#include <algorithm>
#include <cmath>

namespace ma
{
namespace
{
constexpr double kAbsentDb = -200.0;
constexpr double kLevelLowHz = 30.0, kLevelHighHz = 16000.0; // as the stem levels
constexpr double kExpectedRangeDb = 20.0; // a group plays in a region within this of its loudest one
constexpr double kCarryRangeDb = 10.0;    // a channel carries a region within this of its own loudest one
constexpr double kHoleFromReferencesDb = 6.0;
constexpr double kHoleFromTypicalDb = 9.0; // the typical shapes are rough
constexpr double kAnchorLowHz = 100.0, kAnchorHighHz = 5000.0;

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

double earWeight(double hz)
{
    return std::pow(10.0, -equalLoudnessWeightDb(hz) / 10.0);
}

double weighted(const SpectrumAnalyzer::Bands& db)
{
    double sum = 0.0;
    for (int b = 0; b < numBands(); ++b)
    {
        const double hz = hzOf(b);
        if (hz >= kLevelLowHz && hz <= kLevelHighHz)
            sum += toPower(db[static_cast<size_t>(b)]) * earWeight(hz);
    }
    return sum;
}

int regionOf(double hz)
{
    for (int r = 0; r < kStemRegions; ++r)
        if (hz >= stemRegionEdgeHz(r) && hz < stemRegionEdgeHz(r + 1))
            return r;
    return -1;
}

std::array<double, kStemRegions> regionPowers(const SpectrumAnalyzer::Bands& db)
{
    std::array<double, kStemRegions> out {};
    for (int b = 0; b < numBands(); ++b)
    {
        const int r = regionOf(hzOf(b));
        if (r >= 0)
            out[static_cast<size_t>(r)] += toPower(db[static_cast<size_t>(b)]);
    }
    return out;
}

// Moves a spectrum so its ear-weighted level against the whole is `levelDb`.
void setLevel(SpectrumAnalyzer::Bands& db, double levelDb)
{
    const double now = toDb(weighted(db));
    if (now <= kAbsentDb)
        return;
    for (int b = 0; b < numBands(); ++b)
        if (db[static_cast<size_t>(b)] > kAbsentDb + 1.0)
            db[static_cast<size_t>(b)] += levelDb - now;
}

// Breakpoints (Hz, dB) joined on a log-frequency axis.
SpectrumAnalyzer::Bands shape(std::initializer_list<std::pair<double, double>> points)
{
    const std::vector<std::pair<double, double>> p(points);
    SpectrumAnalyzer::Bands out {};
    out.fill(kAbsentDb);
    for (int b = 0; b < numBands(); ++b)
    {
        const double hz = hzOf(b);
        if (hz <= p.front().first)
            out[static_cast<size_t>(b)] = p.front().second;
        else if (hz >= p.back().first)
            out[static_cast<size_t>(b)] = p.back().second;
        else
            for (size_t i = 1; i < p.size(); ++i)
                if (hz <= p[i].first)
                {
                    const double t = std::log(hz / p[i - 1].first) / std::log(p[i].first / p[i - 1].first);
                    out[static_cast<size_t>(b)] = p[i - 1].second + t * (p[i].second - p[i - 1].second);
                    break;
                }
    }
    return out;
}

std::array<SpectrumAnalyzer::Bands, kStemGroups> typicalShapes()
{
    return { {
        shape({ { 20.0, -30.0 }, { 45.0, -4.0 }, { 60.0, 0.0 }, { 100.0, -1.0 }, { 200.0, -10.0 }, { 400.0, -16.0 },
                { 1000.0, -17.0 }, { 2500.0, -14.0 }, { 6000.0, -10.0 }, { 10000.0, -11.0 }, { 16000.0, -20.0 } }),
        shape({ { 20.0, -20.0 }, { 40.0, -2.0 }, { 60.0, 0.0 }, { 110.0, 0.0 }, { 200.0, -6.0 }, { 400.0, -16.0 },
                { 1000.0, -30.0 }, { 3000.0, -45.0 }, { 10000.0, -60.0 } }),
        shape({ { 20.0, -60.0 }, { 100.0, -30.0 }, { 200.0, -10.0 }, { 400.0, -3.0 }, { 1000.0, -2.0 }, { 2500.0, -1.0 },
                { 4000.0, -4.0 }, { 8000.0, -10.0 }, { 14000.0, -18.0 } }),
        shape({ { 20.0, -45.0 }, { 80.0, -25.0 }, { 150.0, -12.0 }, { 300.0, -4.0 }, { 600.0, -2.0 }, { 1500.0, -3.0 },
                { 3000.0, -6.0 }, { 6000.0, -11.0 }, { 12000.0, -18.0 }, { 16000.0, -26.0 } }),
    } };
}

int ghostPriority(int group, int region)
{
    // As the role that would play it (defaultPriority): a whole drum group is led by the kick, a
    // missing low-mid part of the rest is a pad, its upper part a lead.
    switch (static_cast<StemGroup>(group))
    {
        case StemGroup::drums: return region < 0 || region <= 1 ? 1 : 4;
        case StemGroup::bass: return 2;
        case StemGroup::vocals: return 2;
        case StemGroup::music: return region < 0 ? 5 : region <= 2 ? 6 : 3;
    }
    return 5;
}

double median(std::vector<double> values)
{
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    return n % 2 == 1 ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
}
} // namespace

bool Arrangement::valid() const
{
    return std::any_of(groups.begin(), groups.end(), [](const auto& g) { return g.expected; });
}

Arrangement typicalArrangement()
{
    Arrangement out;
    const auto shapes = typicalShapes();
    constexpr std::array<double, kStemGroups> levels { -4.0, -5.5, -5.0, -5.0 };
    constexpr std::array<bool, kStemGroups> expected { true, true, false, true };
    for (int g = 0; g < kStemGroups; ++g)
    {
        auto& a = out.groups[static_cast<size_t>(g)];
        a.expected = expected[static_cast<size_t>(g)];
        a.relDb = shapes[static_cast<size_t>(g)];
        a.levelDb = levels[static_cast<size_t>(g)];
        setLevel(a.relDb, a.levelDb);
    }
    return out;
}

Arrangement arrangementFromStems(const StemTarget& target)
{
    Arrangement out;
    if (!target.valid())
        return out;
    const auto shapes = typicalShapes();
    std::array<SpectrumAnalyzer::Bands, kStemGroups> power {};
    std::array<int, kStemGroups> have {}, withBands {};
    std::array<std::array<int, kStemSections>, kStemGroups> inSection {};
    std::array<int, kStemSections> sections {};
    int profiles = 0, profilesWithBands = 0;
    for (const auto& p : target.profiles)
    {
        if (!p.measure.valid)
            continue;
        ++profiles;
        for (int s = 0; s < kStemSections; ++s)
        {
            const auto& section = p.section(static_cast<StemSection>(s));
            if (!section.valid)
                continue;
            ++sections[static_cast<size_t>(s)];
            for (int g = 0; g < kStemGroups; ++g)
                inSection[static_cast<size_t>(g)][static_cast<size_t>(s)] += section.measure.groups[static_cast<size_t>(g)].present ? 1 : 0;
        }
        double total = 0.0;
        if (p.hasBands)
            for (int g = 0; g < kStemGroups; ++g)
                if (p.measure.groups[static_cast<size_t>(g)].present)
                    total += weighted(p.groupDb[static_cast<size_t>(g)]);
        profilesWithBands += p.hasBands && total > 0.0 ? 1 : 0;
        for (int g = 0; g < kStemGroups; ++g)
        {
            if (!p.measure.groups[static_cast<size_t>(g)].present)
                continue;
            ++have[static_cast<size_t>(g)];
            if (!p.hasBands || total <= 0.0)
                continue;
            ++withBands[static_cast<size_t>(g)];
            for (int b = 0; b < numBands(); ++b)
                power[static_cast<size_t>(g)][static_cast<size_t>(b)] += toPower(p.groupDb[static_cast<size_t>(g)][static_cast<size_t>(b)]) / total;
        }
    }
    if (profiles == 0)
        return out;
    out.fromReferences = profilesWithBands > 0;
    for (int g = 0; g < kStemGroups; ++g)
    {
        const auto at = static_cast<size_t>(g);
        auto& a = out.groups[at];
        // Most of the references have it (a vocal in one track of five is not expected).
        a.expected = 2 * have[at] >= profiles + 1 && target.level[at].valid;
        a.levelDb = target.level[at].valid ? target.level[at].centre : 0.0;
        a.width = target.width[at].centre;
        a.widthValid = target.width[at].valid;
        for (int s = 0; s < kStemSections; ++s)
            a.inSection[static_cast<size_t>(s)] = sections[static_cast<size_t>(s)] > 0
                                                  && 2 * inSection[at][static_cast<size_t>(s)] >= sections[static_cast<size_t>(s)];
        if (withBands[at] > 0)
            for (int b = 0; b < numBands(); ++b)
                a.relDb[static_cast<size_t>(b)] = toDb(power[at][static_cast<size_t>(b)] / withBands[at]);
        else
            a.relDb = shapes[at];
        setLevel(a.relDb, a.levelDb);
    }
    return out;
}

SketchAnalysis analyseSketch(const SketchInput& input, const Arrangement& arrangement)
{
    SketchAnalysis out;
    out.fromReferences = arrangement.fromReferences;
    out.ghostDb.fill(kAbsentDb);
    out.oursDb.fill(kAbsentDb);

    std::array<double, kStemGroups> ours {};
    double oursTotal = 0.0;
    for (int g = 0; g < kStemGroups; ++g)
    {
        ours[static_cast<size_t>(g)] = input.present[static_cast<size_t>(g)] ? weighted(input.groupDb[static_cast<size_t>(g)]) : 0.0;
        oursTotal += ours[static_cast<size_t>(g)];
    }
    if (oursTotal <= 0.0 || !arrangement.valid())
        return out;
    out.valid = true;
    // A group 40 dB under the rest does not really play (as in the stem levels).
    std::array<bool, kStemGroups> plays {};
    for (int g = 0; g < kStemGroups; ++g)
        plays[static_cast<size_t>(g)] = ours[static_cast<size_t>(g)] > oursTotal * 1e-4;

    // The references' groups that play here are as loud together as ours: that sets the scale.
    double expectedPlaying = 0.0, oursExpected = 0.0;
    for (int g = 0; g < kStemGroups; ++g)
        if (plays[static_cast<size_t>(g)] && arrangement.groups[static_cast<size_t>(g)].expected)
        {
            expectedPlaying += weighted(arrangement.groups[static_cast<size_t>(g)].relDb);
            oursExpected += ours[static_cast<size_t>(g)];
        }
    const double scaleDb = expectedPlaying > 0.0 ? toDb(oursExpected) - toDb(expectedPlaying) : toDb(oursTotal);

    SpectrumAnalyzer::Bands ghostPower {};
    auto add = [&](SketchGhost ghost) {
        ghost.priority = ghostPriority(ghost.group, ghost.region);
        out.ghosts.push_back(ghost);
    };

    for (int g = 0; g < kStemGroups; ++g)
    {
        const auto& a = arrangement.groups[static_cast<size_t>(g)];
        if (!a.expected)
            continue;
        SpectrumAnalyzer::Bands expected {};
        for (int b = 0; b < numBands(); ++b)
            expected[static_cast<size_t>(b)] = a.relDb[static_cast<size_t>(b)] > kAbsentDb + 1.0 ? a.relDb[static_cast<size_t>(b)] + scaleDb : kAbsentDb;

        if (!plays[static_cast<size_t>(g)])
        {
            SketchGhost ghost;
            ghost.group = g;
            ghost.db = expected;
            out.missing[static_cast<size_t>(g)] = true;
            add(ghost);
            continue;
        }

        // A group that plays, but not where the references' does: the part that is missing
        // (a pad under a lead, hats over a kick), as long as none of its channels plays there.
        const auto expectedRegions = regionPowers(expected);
        const auto ourRegions = regionPowers(input.groupDb[static_cast<size_t>(g)]);
        const double loudestExpected = *std::max_element(expectedRegions.begin(), expectedRegions.end());
        std::array<bool, kStemRegions> wanted {}, carried {};
        for (int r = 0; r < kStemRegions; ++r)
            wanted[static_cast<size_t>(r)] = expectedRegions[static_cast<size_t>(r)] > 0.0
                                             && toDb(expectedRegions[static_cast<size_t>(r)]) >= toDb(loudestExpected) - kExpectedRangeDb;
        for (const auto& channel : input.channels)
        {
            if (channel.group != g)
                continue;
            const auto own = regionPowers(channel.db);
            const double loudest = *std::max_element(own.begin(), own.end());
            if (loudest <= 0.0)
                continue;
            for (int r = 0; r < kStemRegions; ++r)
                carried[static_cast<size_t>(r)] = carried[static_cast<size_t>(r)]
                                                  || toDb(own[static_cast<size_t>(r)]) >= toDb(loudest) - kCarryRangeDb;
        }
        // The group's own level from the regions it does carry, so a hole does not pull it down.
        std::vector<double> offsets;
        for (int r = 0; r < kStemRegions; ++r)
            if (wanted[static_cast<size_t>(r)] && carried[static_cast<size_t>(r)] && ourRegions[static_cast<size_t>(r)] > 0.0)
                offsets.push_back(toDb(ourRegions[static_cast<size_t>(r)]) - toDb(expectedRegions[static_cast<size_t>(r)]));
        if (offsets.empty())
            continue;
        const double offset = median(offsets);
        const double threshold = arrangement.fromReferences ? kHoleFromReferencesDb : kHoleFromTypicalDb;
        for (int r = 0; r < kStemRegions; ++r)
        {
            const auto at = static_cast<size_t>(r);
            if (!wanted[at] || carried[at] || input.cutOnPurpose[static_cast<size_t>(g)][at])
                continue;
            const double shortDb = offset - (toDb(ourRegions[at]) - toDb(expectedRegions[at]));
            if (shortDb < threshold)
                continue;
            SketchGhost ghost;
            ghost.group = g;
            ghost.region = r;
            ghost.shortDb = std::min(shortDb, 60.0);
            ghost.db.fill(kAbsentDb);
            for (int b = 0; b < numBands(); ++b)
            {
                if (regionOf(hzOf(b)) != r)
                    continue;
                const double want = toPower(expected[static_cast<size_t>(b)] + offset);
                const double have = toPower(input.groupDb[static_cast<size_t>(g)][static_cast<size_t>(b)]);
                ghost.db[static_cast<size_t>(b)] = toDb(std::max(0.0, want - have));
            }
            out.hole[static_cast<size_t>(g)][at] = true;
            add(ghost);
        }
    }

    for (const auto& ghost : out.ghosts)
        for (int b = 0; b < numBands(); ++b)
            ghostPower[static_cast<size_t>(b)] += toPower(ghost.db[static_cast<size_t>(b)]);
    double ghostsTotal = 0.0;
    for (const auto& ghost : out.ghosts)
        ghostsTotal += weighted(ghost.db);
    for (int b = 0; b < numBands(); ++b)
    {
        double sum = 0.0;
        for (int g = 0; g < kStemGroups; ++g)
            sum += input.present[static_cast<size_t>(g)] ? toPower(input.groupDb[static_cast<size_t>(g)][static_cast<size_t>(b)]) : 0.0;
        out.oursDb[static_cast<size_t>(b)] = toDb(sum);
        out.ghostDb[static_cast<size_t>(b)] = toDb(ghostPower[static_cast<size_t>(b)]);
    }
    const double whole = oursTotal + ghostsTotal;
    for (auto& ghost : out.ghosts)
    {
        const double own = weighted(ghost.db);
        ghost.levelDb = toDb(own) - toDb(whole);
        // The middle 80% of what the ear gets from it.
        double running = 0.0;
        bool low = false;
        for (int b = 0; b < numBands(); ++b)
        {
            const double hz = hzOf(b);
            if (hz < kLevelLowHz || hz > kLevelHighHz)
                continue;
            running += toPower(ghost.db[static_cast<size_t>(b)]) * earWeight(hz);
            if (!low && running >= 0.1 * own)
            {
                ghost.lowHz = hz;
                low = true;
            }
            if (running >= 0.9 * own)
            {
                ghost.highHz = hz;
                break;
            }
        }
    }
    out.sketch = !out.ghosts.empty();
    return out;
}

Curve addGhosts(const Curve& masterDb, const SketchAnalysis& sketch)
{
    if (!sketch.sketch)
        return masterDb;
    double master = 0.0, ours = 0.0;
    for (int b = 0; b < numBands(); ++b)
    {
        const double hz = hzOf(b);
        if (hz < kAnchorLowHz || hz > kAnchorHighHz)
            continue;
        master += toPower(masterDb[static_cast<size_t>(b)]);
        ours += toPower(sketch.oursDb[static_cast<size_t>(b)]);
    }
    if (master <= 0.0 || ours <= 0.0)
        return masterDb;
    const double carry = master / ours;
    Curve out = masterDb;
    for (int b = 0; b < numBands(); ++b)
        out[static_cast<size_t>(b)] = toDb(toPower(masterDb[static_cast<size_t>(b)]) + carry * toPower(sketch.ghostDb[static_cast<size_t>(b)]));
    return out;
}

std::vector<bool> arrangementRegions(const TonalComparison& plain, const TonalComparison& withGhosts)
{
    std::vector<bool> out(plain.regions.size(), false);
    if (!plain.valid || !withGhosts.valid)
        return out;
    for (size_t i = 0; i < plain.regions.size(); ++i)
    {
        const auto& region = plain.regions[i];
        double left = 0.0;
        int count = 0;
        for (int b = region.firstBand; b <= region.lastBand; ++b)
        {
            const double d = withGhosts.deviationDb[static_cast<size_t>(b)];
            left += region.hole() ? std::max(0.0, -d) : std::max(0.0, d);
            ++count;
        }
        left = count > 0 ? left / count : 0.0;
        out[i] = left <= std::abs(region.deviationDb) / 3.0;
    }
    return out;
}

} // namespace ma
