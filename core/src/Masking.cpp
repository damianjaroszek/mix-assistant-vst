#include "ma/Masking.h"

#include "ma/EqualLoudness.h"

#include <algorithm>
#include <cmath>

namespace ma
{
namespace
{
constexpr double kSpreadTowardsHighDbPerBand = 12.0; // masking reaches up in frequency
constexpr double kSpreadTowardsLowDbPerBand = 24.0;  // and falls off fast downwards
constexpr double kRelevantRangeDb = 24.0;
constexpr double kCrowdingRangeDb = 6.0;
constexpr double kFullMaskingMarginDb = 9.0;

using Levels = std::vector<double>;

Levels perceived(const MaskingChannel& channel, int numBands)
{
    Levels out(static_cast<size_t>(numBands));
    for (int b = 0; b < numBands; ++b)
    {
        const double raw = channel.rawDb[static_cast<size_t>(b)];
        const double hz = SpectrumAnalyzer::bandCentreHz(BandScale::erb, b);
        out[static_cast<size_t>(b)] = std::isfinite(raw) ? raw - equalLoudnessWeightDb(hz) : -200.0;
    }
    return out;
}

Levels excitation(const Levels& levels)
{
    const int n = static_cast<int>(levels.size());
    Levels out(levels.size(), -200.0);
    for (int b = 0; b < n; ++b)
    {
        for (int k = 0; k < n; ++k)
        {
            const double slope = k <= b ? kSpreadTowardsHighDbPerBand : kSpreadTowardsLowDbPerBand;
            const double spread = levels[static_cast<size_t>(k)] - slope * std::abs(b - k);
            out[static_cast<size_t>(b)] = std::max(out[static_cast<size_t>(b)], spread);
        }
    }
    return out;
}
} // namespace

MaskingResult analyseMasking(const std::vector<MaskingChannel>& channels, double minimumScore)
{
    const int numBands = SpectrumAnalyzer::numBandsFor(BandScale::erb);
    const int n = static_cast<int>(channels.size());

    std::vector<Levels> levels, excitations;
    std::vector<std::vector<bool>> relevant;
    for (const auto& channel : channels)
    {
        levels.push_back(perceived(channel, numBands));
        excitations.push_back(excitation(levels.back()));

        const double peak = *std::max_element(levels.back().begin(), levels.back().end());
        std::vector<bool> r(static_cast<size_t>(numBands), false);
        for (int b = 0; b < numBands; ++b)
            r[static_cast<size_t>(b)] = channel.valid && peak > -150.0
                                        && levels.back()[static_cast<size_t>(b)] >= peak - kRelevantRangeDb;
        relevant.push_back(std::move(r));
    }

    MaskingResult result;
    result.amount.assign(static_cast<size_t>(n), std::vector<double>(static_cast<size_t>(n), 0.0));
    result.crowding.assign(static_cast<size_t>(numBands), 0);

    for (int masker = 0; masker < n; ++masker)
    {
        if (!channels[static_cast<size_t>(masker)].valid)
            continue;

        for (int masked = 0; masked < n; ++masked)
        {
            if (masked == masker || !channels[static_cast<size_t>(masked)].valid)
                continue;

            std::vector<double> perBand(static_cast<size_t>(numBands), 0.0);
            double sum = 0.0;
            int count = 0;
            for (int b = 0; b < numBands; ++b)
            {
                if (!relevant[static_cast<size_t>(masked)][static_cast<size_t>(b)])
                    continue;
                const double margin = excitations[static_cast<size_t>(masker)][static_cast<size_t>(b)]
                                      - levels[static_cast<size_t>(masked)][static_cast<size_t>(b)];
                // 0 when the masker sits 3 dB under, 1 when it is 9 dB over.
                const double amount = std::clamp((margin + 3.0) / (kFullMaskingMarginDb + 3.0), 0.0, 1.0);
                perBand[static_cast<size_t>(b)] = amount;
                sum += amount;
                ++count;
            }
            const double score = count > 0 ? sum / count : 0.0;
            result.amount[static_cast<size_t>(masker)][static_cast<size_t>(masked)] = score;

            const bool matters = channels[static_cast<size_t>(masked)].priority <= channels[static_cast<size_t>(masker)].priority;
            if (!matters || score < minimumScore)
                continue;

            // Strongest contiguous run of clearly masked bands.
            int bestStart = -1, bestEnd = -1;
            double bestWeight = 0.0;
            for (int b = 0; b < numBands;)
            {
                if (perBand[static_cast<size_t>(b)] < 0.5)
                {
                    ++b;
                    continue;
                }
                const int start = b;
                double weight = 0.0;
                while (b < numBands && perBand[static_cast<size_t>(b)] >= 0.5)
                    weight += perBand[static_cast<size_t>(b++)];
                if (weight > bestWeight)
                {
                    bestWeight = weight;
                    bestStart = start;
                    bestEnd = b - 1;
                }
            }
            if (bestStart < 0)
                continue;

            MaskingIssue issue;
            issue.masker = masker;
            issue.masked = masked;
            issue.score = score;
            issue.lowHz = SpectrumAnalyzer::bandEdgeHz(BandScale::erb, bestStart);
            issue.highHz = SpectrumAnalyzer::bandEdgeHz(BandScale::erb, bestEnd + 1);
            result.issues.push_back(issue);
        }
    }

    for (int b = 0; b < numBands; ++b)
    {
        double top = -200.0;
        for (int c = 0; c < n; ++c)
            if (relevant[static_cast<size_t>(c)][static_cast<size_t>(b)])
                top = std::max(top, levels[static_cast<size_t>(c)][static_cast<size_t>(b)]);
        for (int c = 0; c < n; ++c)
            if (relevant[static_cast<size_t>(c)][static_cast<size_t>(b)]
                && levels[static_cast<size_t>(c)][static_cast<size_t>(b)] >= top - kCrowdingRangeDb)
                result.crowding[static_cast<size_t>(b)] += 1;
    }

    // Most important masked channel first, then the strongest masking.
    std::sort(result.issues.begin(), result.issues.end(), [&](const MaskingIssue& a, const MaskingIssue& b) {
        const int pa = channels[static_cast<size_t>(a.masked)].priority;
        const int pb = channels[static_cast<size_t>(b.masked)].priority;
        return pa != pb ? pa < pb : a.score > b.score;
    });
    return result;
}

} // namespace ma
