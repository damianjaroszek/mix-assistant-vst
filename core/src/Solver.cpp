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
            out[c].rawDb[static_cast<size_t>(b)] = channels[c].rawDb[static_cast<size_t>(b)] + eqResponseDb(eq[c], hz);
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

double perceivedDb(const MaskingChannel& channel, int band)
{
    const double hz = SpectrumAnalyzer::bandCentreHz(BandScale::erb, band);
    return channel.rawDb[static_cast<size_t>(band)] - equalLoudnessWeightDb(hz);
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
            if (candidate.score < options.minimumScore)
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
        int slot = -1;
        for (int b = 0; b < kEqBands; ++b)
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
            gain = visit > 0 ? previous - kDeepenStepDb : std::min(previous, -std::clamp(excess + 1.0, kMinimumCutDb, options.maxCutDb));
            band.q = std::min(band.q, static_cast<float>(q));
        }
        else
        {
            gain = -std::clamp(excess + 1.0, kMinimumCutDb, options.maxCutDb);
            band.hz = static_cast<float>(centre);
            band.q = static_cast<float>(q);
        }
        gain = std::max(gain, -options.maxCutDb);
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
            for (const auto& other : result.changes)
                shared = shared || (&other != &change && other.channel == change.channel && other.eqSlot == change.eqSlot
                                    && other.scoreBefore - other.scoreAfter >= 0.05);
            if (!shared)
                result.eq[static_cast<size_t>(change.channel)][static_cast<size_t>(change.eqSlot)] = EqBand {};
        }
    }
    result.changes = std::move(kept);

    if (options.headroomGainDb < -0.05)
    {
        SolverChange change;
        change.kind = SolverChange::Kind::globalGain;
        change.gainDb = options.headroomGainDb;
        result.changes.insert(result.changes.begin(), change);
    }
    return result;
}

} // namespace ma
