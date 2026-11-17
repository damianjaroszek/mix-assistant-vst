#include "ma/Layers.h"

#include "ma/Decibels.h"

#include <algorithm>
#include <cmath>

namespace ma
{

bool layerRole(Role role)
{
    return role != Role::kick && role != Role::bass && role != Role::bus && role != Role::send;
}

int layerClass(Role role)
{
    return isPercussion(role) ? 1 : 0;
}

namespace
{
constexpr BandScale kScale = BandScale::erb;

struct Shape
{
    std::vector<double> share; // per band, sums to 1 over the layer range
    double regionDb = -200.0;
    bool valid = false;
};

double centreHz(int band)
{
    return SpectrumAnalyzer::bandCentreHz(kScale, band);
}

bool inRange(int band)
{
    const double hz = centreHz(band);
    return hz >= kLayerLowHz && hz <= kLayerHighHz;
}
} // namespace

std::vector<LayerGroup> findLayers(const std::vector<LayerChannel>& channels)
{
    const int bands = SpectrumAnalyzer::numBandsFor(kScale);
    const size_t count = channels.size();
    std::vector<Shape> shapes(count);
    double loudest = -200.0;
    for (size_t c = 0; c < count; ++c)
    {
        const auto& channel = channels[c];
        if (!channel.valid || !layerRole(channel.role))
            continue;
        auto& shape = shapes[c];
        shape.share.assign(static_cast<size_t>(bands), 0.0);
        double total = 0.0;
        for (int b = 0; b < bands; ++b)
        {
            if (!inRange(b))
                continue;
            const double power = std::pow(10.0, channel.bandsDb[static_cast<size_t>(b)] / 10.0);
            shape.share[static_cast<size_t>(b)] = power;
            total += power;
        }
        if (total <= 0.0)
            continue;
        for (auto& value : shape.share)
            value /= total;
        shape.regionDb = powerToDb(total);
        shape.valid = true;
        loudest = std::max(loudest, shape.regionDb);
    }
    for (auto& shape : shapes)
        if (shape.valid && shape.regionDb < loudest - kLayerQuietDb)
            shape.valid = false;

    // Pairs carrying the same register, then the groups they form.
    std::vector<int> group(count, -1);
    std::vector<std::vector<double>> overlap(count, std::vector<double>(count, 0.0));
    std::vector<std::vector<int>> linked(count);
    for (size_t a = 0; a < count; ++a)
    {
        if (!shapes[a].valid)
            continue;
        for (size_t b = a + 1; b < count; ++b)
        {
            if (!shapes[b].valid)
                continue;
            double shared = 0.0;
            for (int i = 0; i < bands; ++i)
                shared += std::min(shapes[a].share[static_cast<size_t>(i)], shapes[b].share[static_cast<size_t>(i)]);
            overlap[a][b] = overlap[b][a] = shared;
            if (shared >= kLayerOverlap && layerClass(channels[a].role) == layerClass(channels[b].role))
            {
                linked[a].push_back(static_cast<int>(b));
                linked[b].push_back(static_cast<int>(a));
            }
        }
    }

    std::vector<LayerGroup> groups;
    for (size_t seed = 0; seed < count; ++seed)
    {
        if (group[seed] >= 0 || linked[seed].empty())
            continue;
        // Everything connected to the seed, breadth first.
        std::vector<int> members { static_cast<int>(seed) };
        group[seed] = static_cast<int>(groups.size());
        for (size_t i = 0; i < members.size(); ++i)
            for (const int next : linked[static_cast<size_t>(members[i])])
                if (group[static_cast<size_t>(next)] < 0)
                {
                    group[static_cast<size_t>(next)] = static_cast<int>(groups.size());
                    members.push_back(next);
                }
        // The leader: the most important part of the stack, and the loudest one when that ties.
        // Everything is judged against it, so only the parts really carrying its register stay in
        // (a chain of overlapping pairs can wander away from where it started).
        const auto louder = [&](int a, int b) {
            if (channels[static_cast<size_t>(a)].priority != channels[static_cast<size_t>(b)].priority)
                return channels[static_cast<size_t>(a)].priority < channels[static_cast<size_t>(b)].priority;
            return shapes[static_cast<size_t>(a)].regionDb > shapes[static_cast<size_t>(b)].regionDb;
        };
        std::sort(members.begin(), members.end(), louder);
        const int leader = members.front();
        std::vector<int> kept { leader };
        for (const int member : members)
            if (member != leader && overlap[static_cast<size_t>(leader)][static_cast<size_t>(member)] >= kLayerOverlap)
                kept.push_back(member);
        for (const int member : members)
            group[static_cast<size_t>(member)] = -1;
        for (const int member : kept)
            group[static_cast<size_t>(member)] = static_cast<int>(groups.size());
        if (kept.size() < 2)
            continue;
        members = kept;
        LayerGroup found;
        found.overlap = 1.0;
        for (const int member : members)
            if (member != leader)
                found.overlap = std::min(found.overlap, overlap[static_cast<size_t>(leader)][static_cast<size_t>(member)]);

        // The register they share: where even the weakest of them puts its power.
        std::vector<double> common(static_cast<size_t>(bands), 0.0);
        int peak = -1;
        for (int b = 0; b < bands; ++b)
        {
            if (!inRange(b))
                continue;
            double smallest = 1.0;
            for (const int member : members)
                smallest = std::min(smallest, shapes[static_cast<size_t>(member)].share[static_cast<size_t>(b)]);
            common[static_cast<size_t>(b)] = smallest;
            if (peak < 0 || smallest > common[static_cast<size_t>(peak)])
                peak = b;
        }
        if (peak < 0 || common[static_cast<size_t>(peak)] <= 0.0)
            continue;
        const double edge = 0.25 * common[static_cast<size_t>(peak)];
        int first = peak, last = peak;
        while (first > 0 && inRange(first - 1) && common[static_cast<size_t>(first - 1)] >= edge)
            --first;
        while (last + 1 < bands && inRange(last + 1) && common[static_cast<size_t>(last + 1)] >= edge)
            ++last;
        found.lowHz = SpectrumAnalyzer::bandEdgeHz(kScale, first);
        found.highHz = SpectrumAnalyzer::bandEdgeHz(kScale, last + 1);

        const auto regionDb = [&](int channel) {
            double power = 0.0;
            for (int b = first; b <= last; ++b)
                power += std::pow(10.0, channels[static_cast<size_t>(channel)].bandsDb[static_cast<size_t>(b)] / 10.0);
            return powerToDb(power);
        };
        found.members = members;
        const double leaderDb = regionDb(members.front());
        const double centre = std::sqrt(found.lowHz * found.highHz);
        const double width = std::max(1.0, found.highHz - found.lowHz);
        for (size_t i = 1; i < members.size(); ++i)
        {
            const int member = members[i];
            const double own = regionDb(member);
            const double need = kLayerAheadDb - (leaderDb - own);
            if (need < kMinLayerCutDb)
                continue;
            LayerCut cut;
            cut.channel = member;
            cut.hz = std::round(centre);
            cut.q = std::clamp(centre / width, 0.5, 2.0);
            cut.gainDb = -std::round(std::min(need, kMaxLayerCutDb) * 2.0) / 2.0;
            cut.underLeaderDb = leaderDb - own;
            found.cuts.push_back(cut);
        }
        if (!found.cuts.empty())
            groups.push_back(found);
    }
    return groups;
}

} // namespace ma
