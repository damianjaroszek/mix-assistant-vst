#include "ma/EnergyMap.h"

#include <algorithm>
#include <cmath>

namespace ma
{
namespace
{
constexpr double kDropLevelDb = -3.0;
constexpr int kRunSeconds = 4;
constexpr int kGapSeconds = 4;
constexpr double kLagSeconds = 1.5;
constexpr int kDeepSeconds = 4;
constexpr double kLevelToleranceDb = 2.0;
constexpr double kFirstDropToleranceSeconds = 8.0;
constexpr double kDeepestToleranceDb = 2.0;
constexpr double kSpanOutsideDb = 3.0;
constexpr int kSpanSeconds = 8;

double median(std::vector<double> values)
{
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    return n % 2 == 1 ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

StemCorridor corridorOf(const std::vector<double>& values, double tolerance)
{
    StemCorridor c;
    if (values.empty())
        return c;
    c.valid = true;
    c.centre = median(values);
    c.low = std::min(*std::min_element(values.begin(), values.end()), c.centre - tolerance);
    c.high = std::max(*std::max_element(values.begin(), values.end()), c.centre + tolerance);
    return c;
}

bool atDropLevel(double v)
{
    return std::isfinite(v) && v >= kDropLevelDb;
}
} // namespace

std::vector<double> energyEntries(const std::vector<double>& levelDb)
{
    std::vector<double> entries;
    const int n = static_cast<int>(levelDb.size());
    int lastDrop = -1000000;
    for (int i = 0; i < n; ++i)
    {
        if (!atDropLevel(levelDb[static_cast<size_t>(i)]))
            continue;
        if (i - lastDrop > kGapSeconds)
        {
            bool run = i + kRunSeconds <= n;
            for (int k = 0; k < kRunSeconds && run; ++k)
                run = atDropLevel(levelDb[static_cast<size_t>(i + k)]);
            if (!run)
                continue; // a short peak is not a drop, and does not start the gap over
            entries.push_back(std::max(0.0, i - kLagSeconds));
        }
        lastDrop = i;
    }
    return entries;
}

double deepestBetweenDrops(const std::vector<double>& levelDb)
{
    const auto entries = energyEntries(levelDb);
    if (entries.empty())
        return NAN;
    const int first = static_cast<int>(std::ceil(entries.front() + kLagSeconds));
    int last = -1;
    for (int i = static_cast<int>(levelDb.size()) - 1; i >= 0 && last < 0; --i)
        if (atDropLevel(levelDb[static_cast<size_t>(i)]))
            last = i;
    double deepest = NAN;
    for (int i = first; i + kDeepSeconds <= last; ++i)
    {
        double sum = 0.0;
        bool heard = true;
        for (int k = 0; k < kDeepSeconds && heard; ++k)
        {
            const double v = levelDb[static_cast<size_t>(i + k)];
            heard = std::isfinite(v);
            sum += heard ? v : 0.0;
        }
        if (heard && !(sum / kDeepSeconds >= deepest))
            deepest = sum / kDeepSeconds;
    }
    return deepest;
}

bool EnergyCorridor::at(double secondsFromFirstDrop, double& lowDb, double& centreDb, double& highDb) const
{
    if (!valid)
        return false;
    const auto i = static_cast<long>(std::floor(secondsFromFirstDrop - fromSeconds));
    if (i < 0 || i >= static_cast<long>(centre.size()) || count[static_cast<size_t>(i)] * 2 < profiles)
        return false;
    lowDb = low[static_cast<size_t>(i)];
    centreDb = centre[static_cast<size_t>(i)];
    highDb = high[static_cast<size_t>(i)];
    return true;
}

EnergyCorridor energyCorridor(const StemTarget& target)
{
    EnergyCorridor out;
    struct Lined
    {
        const EnergyTimeline* energy;
        double firstDrop;
    };
    std::vector<Lined> lined;
    std::vector<double> firstDrops, drops, deepest;
    for (const auto& p : target.profiles)
    {
        if (!p.energy.valid)
            continue;
        const auto entries = energyEntries(p.energy.levelDb);
        if (entries.empty())
            continue;
        lined.push_back({ &p.energy, entries.front() });
        firstDrops.push_back(entries.front());
        drops.push_back(static_cast<double>(entries.size()));
        const double d = deepestBetweenDrops(p.energy.levelDb);
        if (std::isfinite(d))
            deepest.push_back(d);
    }
    if (lined.empty())
        return out;
    out.valid = true;
    out.profiles = static_cast<int>(lined.size());
    out.firstDrop = corridorOf(firstDrops, kFirstDropToleranceSeconds);
    out.drops = corridorOf(drops, 0.0);
    out.deepest = corridorOf(deepest, kDeepestToleranceDb);

    double from = 0.0, to = 0.0;
    for (const auto& l : lined)
    {
        from = std::min(from, -std::floor(l.firstDrop));
        to = std::max(to, static_cast<double>(l.energy->levelDb.size()) - std::floor(l.firstDrop));
    }
    out.fromSeconds = from;
    const auto points = static_cast<size_t>(std::max(0.0, to - from));
    out.low.assign(points, kEnergyFloorDb);
    out.centre.assign(points, kEnergyFloorDb);
    out.high.assign(points, kEnergyFloorDb);
    out.count.assign(points, 0);
    for (auto& g : out.groupCentre)
        g.assign(points, kEnergyFloorDb);
    for (size_t i = 0; i < points; ++i)
    {
        const double rel = from + static_cast<double>(i);
        std::vector<double> values;
        std::array<std::vector<double>, kStemGroups> groups;
        for (const auto& l : lined)
        {
            const double second = rel + std::floor(l.firstDrop);
            if (second < 0.0 || second >= static_cast<double>(l.energy->levelDb.size()))
                continue;
            const auto at = static_cast<size_t>(second);
            values.push_back(l.energy->levelDb[at]);
            for (int g = 0; g < kStemGroups; ++g)
            {
                const auto& v = l.energy->groupDb[static_cast<size_t>(g)];
                groups[static_cast<size_t>(g)].push_back(at < v.size() ? v[at] : kEnergyFloorDb);
            }
        }
        out.count[i] = static_cast<int>(values.size());
        if (values.empty())
            continue;
        const auto c = corridorOf(values, kLevelToleranceDb);
        out.low[i] = c.low;
        out.centre[i] = c.centre;
        out.high[i] = c.high;
        for (int g = 0; g < kStemGroups; ++g)
            out.groupCentre[static_cast<size_t>(g)][i] = median(groups[static_cast<size_t>(g)]);
    }
    return out;
}

EnergyComparison compareEnergy(const EnergyTimeline& ours, const EnergyCorridor& references)
{
    EnergyComparison out;
    const auto entries = energyEntries(ours.levelDb);
    if (!ours.valid || entries.empty())
        return out;
    out.valid = true;
    out.firstDropSeconds = entries.front();
    out.drops = static_cast<int>(entries.size());
    out.deepestDb = deepestBetweenDrops(ours.levelDb);
    if (!references.valid)
        return out;
    out.firstDropOutside = references.firstDrop.outside(out.firstDropSeconds);
    out.dropsOutside = references.drops.outside(out.drops);
    if (std::isfinite(out.deepestDb) && references.deepest.valid)
        out.deepestOutside = references.deepest.outside(out.deepestDb);

    // Our seconds against the references', lined up at the first drop.
    const auto n = ours.levelDb.size();
    std::vector<double> outside(n, 0.0);
    for (size_t i = 0; i < n; ++i)
    {
        double low = 0.0, centre = 0.0, high = 0.0;
        const double v = ours.levelDb[i];
        if (!std::isfinite(v) || !references.at(static_cast<double>(i) - std::floor(out.firstDropSeconds), low, centre, high))
            continue;
        outside[i] = v < low ? v - low : v > high ? v - high : 0.0;
    }
    for (size_t i = 0; i < n;)
    {
        const int sign = outside[i] >= kSpanOutsideDb ? 1 : outside[i] <= -kSpanOutsideDb ? -1 : 0;
        if (sign == 0)
        {
            ++i;
            continue;
        }
        size_t j = i;
        double sum = 0.0;
        while (j < n && (sign > 0 ? outside[j] >= kSpanOutsideDb : outside[j] <= -kSpanOutsideDb))
            sum += outside[j++];
        if (j - i >= static_cast<size_t>(kSpanSeconds))
            out.spans.push_back({ static_cast<double>(i), static_cast<double>(j), sum / static_cast<double>(j - i) });
        i = j;
    }
    std::sort(out.spans.begin(), out.spans.end(), [](const auto& a, const auto& b) {
        return std::abs(a.deviationDb) * (a.toSeconds - a.fromSeconds) > std::abs(b.deviationDb) * (b.toSeconds - b.fromSeconds);
    });
    if (out.spans.size() > 3)
        out.spans.resize(3);
    return out;
}

} // namespace ma
