#include "ma/Solver.h"

#include "ma/EqualLoudness.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace ma
{
namespace
{
constexpr int kMaxVisitsPerPair = 3;
constexpr double kDeepenStepDb = 1.5;
constexpr double kMinimumCutDb = 1.5;
constexpr double kContentRangeDb = 30.0;

std::vector<MaskingChannel> withEq(const std::vector<SolverChannel>& channels, const std::vector<EqSettings>& eq)
{
    const int numBands = SpectrumAnalyzer::numBandsFor(BandScale::erb);
    std::vector<MaskingChannel> out(channels.size());
    for (size_t c = 0; c < channels.size(); ++c)
    {
        out[c].priority = channels[c].priority;
        out[c].valid = channels[c].valid;
        for (int b = 0; b < numBands; ++b)
        {
            const double hz = SpectrumAnalyzer::bandCentreHz(BandScale::erb, b);
            out[c].rawDb[static_cast<size_t>(b)] = channels[c].rawDb[static_cast<size_t>(b)] + channels[c].gainDb + eqResponseDb(eq[c], hz);
        }
    }
    return out;
}

// Sum of the collisions that count: less (or equally) important channels covering more important ones.
double totalScore(const MaskingResult& result, const std::vector<SolverChannel>& channels, double minimum)
{
    double sum = 0.0;
    for (size_t m = 0; m < channels.size(); ++m)
        for (size_t k = 0; k < channels.size(); ++k)
            if (m != k && channels[k].priority <= channels[m].priority && result.amount[m][k] >= minimum)
                sum += result.amount[m][k];
    return sum;
}

double qForRange(double lowHz, double highHz)
{
    const double octaves = std::max(0.25, std::log2(highHz / lowHz));
    const double ratio = std::pow(2.0, octaves);
    return std::clamp(std::sqrt(ratio) / (ratio - 1.0), 0.5, 4.0);
}

// Free band, or one already close (half an octave) with the same sign of gain.
int findSlot(const EqSettings& eq, double centreHz, bool boost)
{
    for (int b = 0; b < kEqBands; ++b)
    {
        const auto& band = eq[static_cast<size_t>(b)];
        if (isOn(band) && (band.gainDb > 0.0f) == boost && std::abs(std::log2(band.hz / centreHz)) <= 0.5)
            return b;
    }
    for (int b = 0; b < kEqBands; ++b)
        if (!isOn(eq[static_cast<size_t>(b)]))
            return b;
    return -1;
}

// Gain on a channel holding share p of a region's power that moves the region by targetDb.
double gainForShare(double p, double targetDb)
{
    const double needed = (std::pow(10.0, targetDb / 10.0) - 1.0 + p) / p;
    return needed > 1e-6 ? 10.0 * std::log10(needed) : -1e9;
}

double roundHalf(double db)
{
    return std::round(db * 2.0) / 2.0;
}

double perceivedDb(const MaskingChannel& channel, int band)
{
    const double hz = SpectrumAnalyzer::bandCentreHz(BandScale::erb, band);
    return channel.rawDb[static_cast<size_t>(band)] - equalLoudnessWeightDb(hz);
}

// Does a boost on channel c make it cover a more important channel it did not cover before?
bool createsCollision(const std::vector<SolverChannel>& channels, const std::vector<EqSettings>& eq, const MaskingResult& before,
                      int c, double minimum)
{
    const auto after = analyseMasking(withEq(channels, eq), minimum);
    for (size_t k = 0; k < channels.size(); ++k)
    {
        if (static_cast<int>(k) == c || channels[k].priority > channels[static_cast<size_t>(c)].priority)
            continue;
        const double was = before.amount[static_cast<size_t>(c)][k];
        const double now = after.amount[static_cast<size_t>(c)][k];
        if (now >= minimum && now - was > 0.05)
            return true;
    }
    return false;
}

void solveStemShape(const std::vector<SolverChannel>& channels, const SolverOptions& options, SolverResult& result)
{
    constexpr int kMaxFixes = 6;
    constexpr double kMinimumDeviationDb = 1.5;
    const auto& in = *options.stems;
    const int numBands = SpectrumAnalyzer::numBandsFor(BandScale::erb);
    auto groupOf = [&](size_t c) { return c < in.groupOf.size() && c < in.dropDb.size() && channels[c].valid ? in.groupOf[c] : -1; };
    auto heard = [&](size_t c, int b) {
        return in.dropDb[c][static_cast<size_t>(b)] + eqResponseDb(result.eq[c], SpectrumAnalyzer::bandCentreHz(BandScale::erb, b));
    };

    // The groups as the drop sounds with the EQ so far.
    auto measureNow = [&]() {
        std::array<SpectrumAnalyzer::Bands, kStemGroups> power {}, side {};
        std::array<bool, kStemGroups> present {};
        for (size_t c = 0; c < channels.size(); ++c)
        {
            const int g = groupOf(c);
            if (g < 0 || g >= kStemGroups)
                continue;
            present[static_cast<size_t>(g)] = true;
            for (int b = 0; b < numBands; ++b)
            {
                const double eqDb = eqResponseDb(result.eq[c], SpectrumAnalyzer::bandCentreHz(BandScale::erb, b));
                power[static_cast<size_t>(g)][static_cast<size_t>(b)] += std::pow(10.0, (in.dropDb[c][static_cast<size_t>(b)] + eqDb) / 10.0);
                if (c < in.dropSideDb.size())
                    side[static_cast<size_t>(g)][static_cast<size_t>(b)] += std::pow(10.0, (in.dropSideDb[c][static_cast<size_t>(b)] + eqDb) / 10.0);
            }
        }
        for (int g = 0; g < kStemGroups; ++g)
            for (int b = 0; b < numBands; ++b)
            {
                auto& p = power[static_cast<size_t>(g)][static_cast<size_t>(b)];
                auto& s = side[static_cast<size_t>(g)][static_cast<size_t>(b)];
                p = p > 1e-20 ? 10.0 * std::log10(p) : -200.0;
                s = s > 1e-20 ? 10.0 * std::log10(s) : -200.0;
            }
        return std::make_pair(measureStemGroups(power, side, present), power);
    };

    struct Candidate
    {
        int group = -1, region = -1;
        double deviationDb = 0.0;
    };
    // Largest first, measured again after each fix (a bell reaches into the next region).
    std::vector<std::pair<int, int>> tried;
    int fixes = 0;
    for (int attempts = 0; fixes < kMaxFixes && attempts < 3 * kMaxFixes; ++attempts)
    {
        Candidate candidate;
        const auto measured = measureNow().first;
        const auto comparison = compareStems(measured, *in.target);
        for (int g = 0; g < kStemGroups; ++g)
            for (int r = 0; r < kStemRegions; ++r)
            {
                const auto& c = comparison.groups[static_cast<size_t>(g)];
                const double deviation = c.shapeDeviationDb[static_cast<size_t>(r)];
                const bool done = std::find(tried.begin(), tried.end(), std::make_pair(g, r)) != tried.end();
                const bool arrangement = deviation < 0.0 && in.arrangementHole[static_cast<size_t>(g)][static_cast<size_t>(r)];
                if (c.compared && c.shapeCompared[static_cast<size_t>(r)] && !done && !arrangement && std::abs(deviation) >= kMinimumDeviationDb
                    && std::abs(deviation) > std::abs(candidate.deviationDb))
                    candidate = { g, r, deviation };
            }
        if (candidate.group < 0)
            break;
        tried.emplace_back(candidate.group, candidate.region);
        const double lowHz = stemRegionEdgeHz(candidate.region), highHz = stemRegionEdgeHz(candidate.region + 1);
        const auto groups = measureNow().second;

        // The region's power: of the whole (every group), of this group, of each of its channels.
        double total = 0.0, group = 0.0;
        std::vector<double> own(channels.size(), 0.0);
        for (int b = 0; b < numBands; ++b)
        {
            const double hz = SpectrumAnalyzer::bandCentreHz(BandScale::erb, b);
            if (hz < lowHz || hz >= highHz)
                continue;
            for (int g = 0; g < kStemGroups; ++g)
            {
                const double p = std::pow(10.0, groups[static_cast<size_t>(g)][static_cast<size_t>(b)] / 10.0);
                total += p;
                if (g == candidate.group)
                    group += p;
            }
            for (size_t c = 0; c < channels.size(); ++c)
                if (groupOf(c) == candidate.group)
                    own[c] += std::pow(10.0, heard(c, b) / 10.0);
        }
        // The group's channel that carries the region, with real content there.
        int best = -1;
        for (size_t c = 0; c < channels.size(); ++c)
        {
            if (groupOf(c) != candidate.group || own[c] <= 0.0)
                continue;
            if (candidate.deviationDb < 0.0 && c < in.lowestBoostHz.size() && std::sqrt(lowHz * highHz) <= in.lowestBoostHz[c])
                continue;
            double peak = -200.0;
            for (int b = 0; b < numBands; ++b)
                peak = std::max(peak, heard(c, b));
            double inRegion = -200.0;
            for (int b = 0; b < numBands; ++b)
            {
                const double hz = SpectrumAnalyzer::bandCentreHz(BandScale::erb, b);
                if (hz >= lowHz && hz < highHz)
                    inRegion = std::max(inRegion, heard(c, b));
            }
            if (inRegion < peak - kContentRangeDb)
                continue;
            if (best < 0 || own[c] > own[static_cast<size_t>(best)])
                best = static_cast<int>(c);
        }
        if (best < 0 || group <= 0.0)
            continue;

        // Gain x on the channel (power C in the region) so the group's own level there moves by
        // -deviation; the group's average over its regions moves along by 1/n of that.
        const bool boost = candidate.deviationDb < 0.0;
        int regions = 0;
        for (bool v : measured.groups[static_cast<size_t>(candidate.group)].shapeValid)
            regions += v ? 1 : 0;
        const double wanted = -candidate.deviationDb / std::max(0.5, 1.0 - 1.0 / std::max(1, regions));
        const double carrier = own[static_cast<size_t>(best)];
        const double x = 1.0 + group * (std::pow(10.0, wanted / 10.0) - 1.0) / carrier;
        double gain = x > 1e-6 ? 10.0 * std::log10(x) : -options.maxStemCutDb;
        gain = boost ? std::clamp(gain, 1.0, options.maxStemBoostDb) : std::clamp(gain, -options.maxStemCutDb, -1.0);
        gain = roundHalf(gain);

        auto& eq = result.eq[static_cast<size_t>(best)];
        const double centre = std::sqrt(lowHz * highHz);
        const double q = qForRange(lowHz, highHz);
        int slot = -1;
        if (boost)
        {
            for (int b = 0; b < kEqBands && slot < 0; ++b)
                if (!isOn(eq[static_cast<size_t>(b)]))
                    slot = b;
        }
        else
        {
            slot = findSlot(eq, centre, false);
        }
        if (slot < 0)
            continue;

        const auto before = boost ? analyseMasking(withEq(channels, result.eq), options.minimumScore) : MaskingResult {};
        const auto saved = eq[static_cast<size_t>(slot)];
        auto& band = eq[static_cast<size_t>(slot)];
        if (!boost && isOn(band))
        {
            band.gainDb = static_cast<float>(std::min(static_cast<double>(band.gainDb), gain));
            band.q = std::min(band.q, static_cast<float>(q));
        }
        else
        {
            band = { static_cast<float>(centre), static_cast<float>(gain), static_cast<float>(q) };
        }
        if (boost && createsCollision(channels, result.eq, before, best, options.minimumScore))
        {
            eq[static_cast<size_t>(slot)] = saved;
            continue;
        }

        SolverChange change;
        change.kind = boost ? SolverChange::Kind::stemBoost : SolverChange::Kind::stemCut;
        change.channel = best;
        change.eqSlot = slot;
        change.gainDb = band.gainDb;
        change.group = candidate.group;
        change.lowHz = lowHz;
        change.highHz = highHz;
        change.deviationDb = candidate.deviationDb;
        result.changes.push_back(change);
        ++fixes;
    }
}
// Phase 24: once the mix sits inside the corridor, pull each of TBC's broad ranges towards its
// centre. Largest offset first, each range once, on the master as the proposal leaves it.
// A tonal bell this proposal already has in the range, the same way, grows instead of a second
// one joining it (one correction per problem); otherwise one broad bell on a free EQ band of the
// carrier: an excess off the least important channel holding a quarter of the range, a lack on
// the most important holding 15%, as long as that raises no collision with a more important part.
template <typename Predict>
void centreZones(const std::vector<SolverChannel>& channels, const SolverOptions& options, Predict predict, SolverResult& result)
{
    const auto& target = *options.tonalTarget;
    std::array<bool, kTonalZones> done {};
    for (int round = 0; round < kTonalZones; ++round)
    {
        const auto before = predict();
        const auto zones = zoneOffsets(before, target);
        int z = -1;
        for (int k = 0; k < kTonalZones; ++k)
        {
            const auto& zone = zones[static_cast<size_t>(k)];
            if (done[static_cast<size_t>(k)] || !zone.valid || std::abs(zone.offsetDb) <= options.centreDeadZoneDb)
                continue;
            // Still outside the corridor there (the corridor fix is at its limit): not a centring job.
            const bool outside = std::any_of(before.regions.begin(), before.regions.end(), [&](const TonalRegion& region) {
                return region.lowHz < zone.highHz && region.highHz > zone.lowHz;
            });
            if (outside)
                continue;
            if (z < 0 || std::abs(zone.offsetDb) > std::abs(zones[static_cast<size_t>(z)].offsetDb))
                z = k;
        }
        if (z < 0)
            break;
        done[static_cast<size_t>(z)] = true;
        const auto start = zones[static_cast<size_t>(z)];
        const bool cut = start.offsetDb > 0.0;
        const auto kind = cut ? SolverChange::Kind::tonalCut : SolverChange::Kind::tonalBoost;

        int index = -1;
        for (size_t i = 0; i < result.changes.size() && index < 0; ++i)
        {
            const auto& change = result.changes[i];
            if (change.kind != kind || change.channel < 0 || change.eqSlot < 0)
                continue;
            const auto& band = result.eq[static_cast<size_t>(change.channel)][static_cast<size_t>(change.eqSlot)];
            if (band.hz >= start.lowHz && band.hz < start.highHz)
                index = static_cast<int>(i);
        }

        const auto eqBefore = result.eq;
        int channel = -1, slot = -1;
        if (index >= 0)
        {
            channel = result.changes[static_cast<size_t>(index)].channel;
            slot = result.changes[static_cast<size_t>(index)].eqSlot;
        }
        else
        {
            const auto current = withEq(channels, result.eq);
            std::vector<double> power(channels.size(), 0.0);
            double total = 0.0;
            for (size_t c = 0; c < channels.size(); ++c)
            {
                if (!channels[c].valid)
                    continue;
                const auto& levels = current[c].rawDb;
                const double peak = *std::max_element(levels.begin(), levels.begin() + SpectrumAnalyzer::numBandsFor(BandScale::erb));
                double inZone = -200.0;
                for (int b = start.firstBand; b <= start.lastBand; ++b)
                    inZone = std::max(inZone, levels[static_cast<size_t>(b)]);
                if (inZone < peak - kContentRangeDb || inZone < -100.0)
                    continue;
                for (int b = start.firstBand; b <= start.lastBand; ++b)
                    power[c] += std::pow(10.0, levels[static_cast<size_t>(b)] / 10.0);
                total += power[c];
            }
            for (size_t c = 0; c < channels.size(); ++c)
            {
                if (total <= 0.0 || channels[c].ghost || power[c] / total < (cut ? 0.25 : 0.15))
                    continue;
                bool free = false;
                for (const auto& band : result.eq[c])
                    free = free || !isOn(band);
                if (!free)
                    continue;
                const bool better = channel < 0
                                    || (channels[c].priority != channels[static_cast<size_t>(channel)].priority
                                            ? (cut ? channels[c].priority > channels[static_cast<size_t>(channel)].priority
                                                   : channels[c].priority < channels[static_cast<size_t>(channel)].priority)
                                            : power[c] > power[static_cast<size_t>(channel)]);
                if (better)
                    channel = static_cast<int>(c);
            }
            if (channel < 0)
                continue;
            for (int b = 0; b < kEqBands && slot < 0; ++b)
                if (!isOn(result.eq[static_cast<size_t>(channel)][static_cast<size_t>(b)]))
                    slot = b;
            result.eq[static_cast<size_t>(channel)][static_cast<size_t>(slot)]
                = { static_cast<float>(std::sqrt(start.lowHz * start.highHz)), 0.0f, static_cast<float>(qForRange(start.lowHz, start.highHz)) };
        }

        // Half a dB at a time while the range is still off the centre, without crossing it, moving
        // it more than allowed, or pushing anything out of the corridor.
        auto& band = result.eq[static_cast<size_t>(channel)][static_cast<size_t>(slot)];
        const float original = band.gainDb;
        const double outsideBefore = outsideCorridorDb(before);
        double after = start.offsetDb;
        for (int step = 0; step < 16 && std::abs(after) > 0.5 * options.centreDeadZoneDb; ++step)
        {
            const float saved = band.gainDb;
            const double next = band.gainDb + (cut ? -0.5 : 0.5);
            if (cut ? next < -options.maxCutDb : next > options.maxBoostDb)
                break;
            if (std::abs(next - original) > options.maxCentreBellDb + 0.01)
                break;
            band.gainDb = static_cast<float>(next);
            const auto now = predict();
            const double offset = zoneOffsets(now, target)[static_cast<size_t>(z)].offsetDb;
            const bool crossed = cut ? offset < -0.25 : offset > 0.25;
            if (crossed || std::abs(start.offsetDb - offset) > options.maxCentreMoveDb + 0.01
                || outsideCorridorDb(now) > outsideBefore + 0.25)
            {
                band.gainDb = saved;
                break;
            }
            after = offset;
        }

        // A lift must not bury a more important part.
        if (!cut && band.gainDb > original)
        {
            const auto masked = analyseMasking(withEq(channels, eqBefore), options.minimumScore);
            while (band.gainDb > original)
            {
                const auto now = analyseMasking(withEq(channels, result.eq), options.minimumScore);
                bool collision = false;
                for (size_t k = 0; k < channels.size(); ++k)
                {
                    if (static_cast<int>(k) == channel || channels[k].priority > channels[static_cast<size_t>(channel)].priority)
                        continue;
                    const double was = masked.amount[static_cast<size_t>(channel)][k];
                    const double is = now.amount[static_cast<size_t>(channel)][k];
                    collision = collision || (is >= options.minimumScore && is - was > 0.05);
                }
                if (!collision)
                    break;
                band.gainDb = std::max(original, band.gainDb - 0.5f);
            }
            after = zoneOffsets(predict(), target)[static_cast<size_t>(z)].offsetDb;
        }

        // Under 1 dB a new bell is not worth a band; a grown one keeps whatever it gained.
        if ((index < 0 && std::abs(band.gainDb) < 1.0f) || band.gainDb == original)
        {
            result.eq = eqBefore;
            continue;
        }
        SolverChange change = index >= 0 ? result.changes[static_cast<size_t>(index)] : SolverChange {};
        if (index < 0)
        {
            change.kind = kind;
            change.channel = channel;
            change.eqSlot = slot;
            change.lowHz = start.lowHz;
            change.highHz = start.highHz;
        }
        change.gainDb = band.gainDb;
        change.centre = true;
        change.zone = z;
        change.zoneOffsetDb = start.offsetDb;
        change.zoneAfterDb = after;
        if (index >= 0)
            result.changes[static_cast<size_t>(index)] = change;
        else
            result.changes.push_back(change);
    }
}

} // namespace

SolverResult solve(const std::vector<SolverChannel>& channels, const SolverOptions& options)
{
    SolverResult result;
    result.eq.assign(channels.size(), EqSettings {});

    const auto initial = analyseMasking(withEq(channels, result.eq), options.minimumScore);
    result.crowdingBefore = totalScore(initial, channels, options.minimumScore);

    std::map<std::pair<int, int>, int> visits;
    std::map<std::pair<int, int>, size_t> changeFor; // (masker, masked) -> index in changes

    for (int round = 0; round < 32; ++round)
    {
        const auto current = withEq(channels, result.eq);
        const auto masking = analyseMasking(current, options.minimumScore);

        // Most important collision that can still be worked on.
        const MaskingIssue* issue = nullptr;
        for (const auto& candidate : masking.issues)
        {
            if (candidate.score < options.minimumScore || channels[static_cast<size_t>(candidate.masker)].ghost)
                continue;
            if (visits[{ candidate.masker, candidate.masked }] < kMaxVisitsPerPair)
            {
                issue = &candidate;
                break;
            }
        }
        if (issue == nullptr)
            break;

        const auto key = std::make_pair(issue->masker, issue->masked);
        const int visit = visits[key]++;
        auto& eq = result.eq[static_cast<size_t>(issue->masker)];
        // Room for a part that is not there yet is kept smaller: the sketch must still sound.
        const double maxCut = channels[static_cast<size_t>(issue->masked)].ghost ? std::min(options.maxCutDb, options.maxGhostCutDb)
                                                                                : options.maxCutDb;

        // How far the masker sits above the masked channel over the range, as the ear hears it.
        double excess = 0.0;
        int count = 0;
        for (int b = issue->lowBand; b <= issue->highBand; ++b)
        {
            excess += perceivedDb(current[static_cast<size_t>(issue->masker)], b)
                      - perceivedDb(current[static_cast<size_t>(issue->masked)], b);
            ++count;
        }
        excess = count > 0 ? excess / count : 0.0;

        const double centre = std::sqrt(issue->lowHz * issue->highHz);
        const double q = qForRange(issue->lowHz, issue->highHz);

        // Reuse a band already close to this range (half an octave), else take a free one.
        // A collision worked on before keeps its own band, so each change knows the band it deepens.
        int slot = -1;
        if (const auto known = changeFor.find(key); known != changeFor.end())
            slot = result.changes[known->second].eqSlot;
        for (int b = 0; b < kEqBands && changeFor.count(key) == 0; ++b)
        {
            const auto& band = eq[static_cast<size_t>(b)];
            if (band.gainDb < 0.0f && std::abs(std::log2(band.hz / centre)) <= 0.5)
                slot = b;
        }
        if (slot < 0)
            for (int b = 0; b < kEqBands && slot < 0; ++b)
                if (!isOn(eq[static_cast<size_t>(b)]))
                    slot = b;
        if (slot < 0)
            continue; // no band left on this channel

        auto& band = eq[static_cast<size_t>(slot)];
        const double previous = band.gainDb;
        double gain;
        if (previous < 0.0)
        {
            gain = visit > 0 ? previous - kDeepenStepDb : std::min(previous, -std::clamp(excess + 1.0, kMinimumCutDb, maxCut));
            band.q = std::min(band.q, static_cast<float>(q));
        }
        else
        {
            gain = -std::clamp(excess + 1.0, kMinimumCutDb, maxCut);
            band.hz = static_cast<float>(centre);
            band.q = static_cast<float>(q);
        }
        gain = std::max(gain, -maxCut);
        if (gain >= previous - 0.05)
        {
            visits[key] = kMaxVisitsPerPair; // already as deep as allowed
            continue;
        }
        band.gainDb = static_cast<float>(std::round(gain * 2.0) / 2.0);

        if (changeFor.count(key) == 0)
        {
            SolverChange change;
            change.channel = issue->masker;
            change.eqSlot = slot;
            change.maskedChannel = issue->masked;
            change.lowHz = issue->lowHz;
            change.highHz = issue->highHz;
            change.scoreBefore = initial.amount[static_cast<size_t>(issue->masker)][static_cast<size_t>(issue->masked)];
            changeFor[key] = result.changes.size();
            result.changes.push_back(change);
        }
        // How deep this collision took the band: a cut dropped below gives the band back to the
        // depth the changes that stay asked for.
        result.changes[changeFor[key]].gainDb = band.gainDb;
    }

    const auto final = analyseMasking(withEq(channels, result.eq), options.minimumScore);
    result.crowdingAfter = totalScore(final, channels, options.minimumScore);
    for (auto& change : result.changes)
        change.scoreAfter = final.amount[static_cast<size_t>(change.channel)][static_cast<size_t>(change.maskedChannel)];

    // A cut that did not reduce its collision is not worth having.
    std::vector<SolverChange> kept;
    for (const auto& change : result.changes)
    {
        if (change.scoreBefore - change.scoreAfter >= 0.05)
            kept.push_back(change);
        else
        {
            bool shared = false;
            double needed = 0.0; // the deepest the changes that stay set the band to
            for (const auto& other : result.changes)
                if (&other != &change && other.channel == change.channel && other.eqSlot == change.eqSlot
                    && other.scoreBefore - other.scoreAfter >= 0.05)
                {
                    shared = true;
                    needed = std::min(needed, other.gainDb);
                }
            auto& band = result.eq[static_cast<size_t>(change.channel)][static_cast<size_t>(change.eqSlot)];
            if (!shared)
                band = EqBand {};
            else
                band.gainDb = std::max(band.gainDb, static_cast<float>(needed)); // a ghost's room stays within its cap
        }
    }
    if (kept.size() != result.changes.size())
    {
        // Bands given back above: the scores of what stays, measured again.
        const auto again = analyseMasking(withEq(channels, result.eq), options.minimumScore);
        result.crowdingAfter = totalScore(again, channels, options.minimumScore);
        for (auto& change : kept)
            change.scoreAfter = again.amount[static_cast<size_t>(change.channel)][static_cast<size_t>(change.maskedChannel)];
    }
    result.changes = std::move(kept);

    if (options.stems != nullptr && options.stems->target != nullptr && options.stems->target->valid())
        solveStemShape(channels, options, result);

    // One tonal region: fill a hole on the most important carrier, take an excess off the least important.
    auto fixRegion = [&](const TonalRegion& region) {
        const auto current = withEq(channels, result.eq);
        std::vector<double> power(channels.size(), 0.0);
        double total = 0.0;
        for (size_t c = 0; c < channels.size(); ++c)
        {
            if (!channels[c].valid)
                continue;
            // Only real content counts: within 30 dB of the channel's own loudest band.
            const auto& levels = current[c].rawDb;
            const double peak = *std::max_element(levels.begin(), levels.begin() + SpectrumAnalyzer::numBandsFor(BandScale::erb));
            double inRegion = -200.0;
            for (int b = region.firstBand; b <= region.lastBand; ++b)
                inRegion = std::max(inRegion, levels[static_cast<size_t>(b)]);
            if (inRegion < peak - kContentRangeDb || inRegion < -100.0)
                continue;
            for (int b = region.firstBand; b <= region.lastBand; ++b)
                power[c] += std::pow(10.0, levels[static_cast<size_t>(b)] / 10.0);
            total += power[c];
        }

        double ghostShare = 0.0;
        for (size_t c = 0; c < channels.size(); ++c)
            ghostShare += channels[c].ghost && total > 0.0 ? power[c] / total : 0.0;
        std::vector<int> order;
        for (size_t c = 0; c < channels.size(); ++c)
            if (total > 0.0 && !channels[c].ghost && power[c] / total >= (region.hole() ? 0.15 : 0.25))
                order.push_back(static_cast<int>(c));

        const double centre = std::sqrt(region.lowHz * region.highHz);
        const double q = qForRange(region.lowHz, region.highHz);
        SolverChange change;
        change.lowHz = region.lowHz;
        change.highHz = region.highHz;
        change.deviationDb = region.deviationDb;

        if (region.hole())
        {
            // Most important channel first: it is the one that should be heard there.
            std::sort(order.begin(), order.end(), [&](int a, int b) {
                return channels[static_cast<size_t>(a)].priority != channels[static_cast<size_t>(b)].priority
                           ? channels[static_cast<size_t>(a)].priority < channels[static_cast<size_t>(b)].priority
                           : power[static_cast<size_t>(a)] > power[static_cast<size_t>(b)];
            });
            const auto before = analyseMasking(current, options.minimumScore);
            bool filled = false;
            for (int c : order)
            {
                auto& eq = result.eq[static_cast<size_t>(c)];
                // A free band only: reusing a nearby boost would move it off the region it was set for.
                int slot = -1;
                for (int b = 0; b < kEqBands && slot < 0; ++b)
                    if (!isOn(eq[static_cast<size_t>(b)]))
                        slot = b;
                if (slot < 0)
                    continue;
                const double share = power[static_cast<size_t>(c)] / total;
                double gain = roundHalf(std::clamp(gainForShare(share, -region.deviationDb), 1.0, options.maxBoostDb));
                const auto saved = eq[static_cast<size_t>(slot)];

                for (int attempt = 0; attempt < 2 && !filled; ++attempt, gain = roundHalf(gain / 2.0))
                {
                    if (gain < 1.0)
                        break;
                    eq[static_cast<size_t>(slot)] = { static_cast<float>(centre), static_cast<float>(gain), static_cast<float>(q) };
                    const auto after = analyseMasking(withEq(channels, result.eq), options.minimumScore);
                    bool newCollision = false;
                    for (size_t k = 0; k < channels.size(); ++k)
                    {
                        if (static_cast<int>(k) == c || channels[k].priority > channels[static_cast<size_t>(c)].priority)
                            continue;
                        const double was = before.amount[static_cast<size_t>(c)][k];
                        const double now = after.amount[static_cast<size_t>(c)][k];
                        newCollision = newCollision || (now >= options.minimumScore && now - was > 0.05);
                    }
                    if (!newCollision)
                    {
                        filled = true;
                        change.kind = SolverChange::Kind::tonalBoost;
                        change.channel = c;
                        change.eqSlot = slot;
                        change.gainDb = gain;
                    }
                }
                if (filled)
                    break;
                eq[static_cast<size_t>(slot)] = saved;
            }

            if (!filled)
            {
                // Nothing to boost: the octave below is where harmonics would come from.
                change.kind = SolverChange::Kind::tonalAdvice;
                double best = 0.0;
                for (size_t c = 0; c < channels.size(); ++c)
                {
                    if (!channels[c].valid || channels[c].ghost)
                        continue;
                    double below = 0.0;
                    for (int b = 0; b < SpectrumAnalyzer::numBandsFor(BandScale::erb); ++b)
                    {
                        const double hz = SpectrumAnalyzer::bandCentreHz(BandScale::erb, b);
                        if (hz >= region.lowHz / 2.0 && hz <= region.highHz / 2.0)
                            below += std::pow(10.0, current[c].rawDb[static_cast<size_t>(b)] / 10.0);
                    }
                    if (below > best)
                    {
                        best = below;
                        change.sourceChannel = static_cast<int>(c);
                    }
                }
            }
            result.changes.push_back(change);
        }
        else if (!order.empty() && ghostShare < 0.3)
        {
            // Least important carrier first. (An excess the ghosts carry much of is left alone:
            // the part that comes may well be played quieter than its stand-in.)
            std::sort(order.begin(), order.end(), [&](int a, int b) {
                return channels[static_cast<size_t>(a)].priority != channels[static_cast<size_t>(b)].priority
                           ? channels[static_cast<size_t>(a)].priority > channels[static_cast<size_t>(b)].priority
                           : power[static_cast<size_t>(a)] > power[static_cast<size_t>(b)];
            });
            const int c = order.front();
            auto& eq = result.eq[static_cast<size_t>(c)];
            const int slot = findSlot(eq, centre, false);
            if (slot < 0)
                return;
            const double share = power[static_cast<size_t>(c)] / total;
            const double gain = roundHalf(std::clamp(gainForShare(share, -region.deviationDb), -options.maxCutDb, -1.0));
            auto& band = eq[static_cast<size_t>(slot)];
            if (isOn(band))
            {
                // A cut already there (a collision's or a stem group's) that is deep enough is the same correction.
                if (band.gainDb <= gain + 0.01)
                    return;
                band.gainDb = static_cast<float>(gain);
                band.q = std::min(band.q, static_cast<float>(q));
            }
            else
            {
                band = { static_cast<float>(centre), static_cast<float>(gain), static_cast<float>(q) };
            }
            change.kind = SolverChange::Kind::tonalCut;
            change.channel = c;
            change.eqSlot = slot;
            change.gainDb = band.gainDb;
            result.changes.push_back(change);
        }
    };

    const bool predicting = options.tonalTarget != nullptr && options.tonalTarget->valid();
    if (predicting)
    {
        // The master as this proposal leaves it: the change from each channel's current EQ to the
        // proposed one, and its planned level move, added to what was measured.
        const int numBands = SpectrumAnalyzer::numBandsFor(BandScale::erb);
        auto predict = [&]() {
            Curve master = options.tonalMasterDb;
            for (int b = 0; b < numBands; ++b)
            {
                const double hz = SpectrumAnalyzer::bandCentreHz(BandScale::erb, b);
                double before = 0.0, after = 0.0;
                for (size_t c = 0; c < channels.size(); ++c)
                {
                    if (!channels[c].valid)
                        continue;
                    const double raw = channels[c].rawDb[static_cast<size_t>(b)];
                    before += std::pow(10.0, (channels[c].hasNow ? channels[c].nowDb[static_cast<size_t>(b)]
                                                                 : raw + eqResponseDb(channels[c].currentEq, hz)) / 10.0);
                    after += std::pow(10.0, (raw + channels[c].gainDb + eqResponseDb(result.eq[c], hz) + channels[c].extraDb[static_cast<size_t>(b)]) / 10.0);
                }
                if (before > 1e-20 && after > 1e-20)
                    master[static_cast<size_t>(b)] += 10.0 * std::log10(after / before);
            }
            return compareToTarget(master, *options.tonalTarget);
        };
        // Largest region first, measured again after each fix (a bell reaches its neighbours),
        // each range worked on once.
        constexpr int kMaxRegions = 4;
        std::vector<std::pair<double, double>> tried;
        for (int handled = 0; handled < kMaxRegions; ++handled)
        {
            const auto now = predict();
            const TonalRegion* next = nullptr;
            for (const auto& region : now.regions)
            {
                const bool done = std::any_of(tried.begin(), tried.end(),
                                              [&](const auto& t) { return region.lowHz < t.second && region.highHz > t.first; });
                if (!done)
                {
                    next = &region;
                    break;
                }
            }
            if (next == nullptr)
                break;
            tried.emplace_back(next->lowHz, next->highHz);
            const TonalRegion region = *next;
            const size_t before = result.changes.size();
            const auto eqBefore = result.eq;
            fixRegion(region);
            if (result.changes.size() == before)
                continue;
            auto& change = result.changes.back();
            if (change.kind != SolverChange::Kind::tonalBoost && change.kind != SolverChange::Kind::tonalCut)
                continue;
            // Sized from the carrier's share alone, a bell can overshoot to the other side of the
            // corridor: step it back while it does, and drop it under 1 dB.
            auto& band = result.eq[static_cast<size_t>(change.channel)][static_cast<size_t>(change.eqSlot)];
            for (int step = 0; step < 6; ++step)
            {
                const auto after = predict();
                double over = 0.0;
                for (int b = region.firstBand; b <= region.lastBand; ++b)
                {
                    const double d = after.deviationDb[static_cast<size_t>(b)];
                    over += region.hole() ? std::max(0.0, d) : std::max(0.0, -d);
                }
                over /= region.lastBand - region.firstBand + 1;
                if (over < 0.5)
                    break;
                const double smaller = band.gainDb > 0.0f ? band.gainDb - 0.5 : band.gainDb + 0.5;
                if (std::abs(smaller) < 1.0)
                {
                    band = eqBefore[static_cast<size_t>(change.channel)][static_cast<size_t>(change.eqSlot)];
                    result.changes.pop_back();
                    break;
                }
                band.gainDb = static_cast<float>(smaller);
                change.gainDb = smaller;
            }
        }
        if (options.centreZones)
            centreZones(channels, options, predict, result);
    }
    else if (options.tonal != nullptr && options.tonal->valid)
    {
        constexpr int kMaxRegions = 4;
        int handled = 0;
        for (const auto& region : options.tonal->regions)
        {
            if (handled++ >= kMaxRegions)
                break;
            fixRegion(region);
        }
    }

    if (std::abs(options.headroomGainDb) > 0.05)
    {
        SolverChange change;
        change.kind = SolverChange::Kind::globalGain;
        change.gainDb = options.headroomGainDb;
        result.changes.insert(result.changes.begin(), change);
    }
    return result;
}

} // namespace ma
