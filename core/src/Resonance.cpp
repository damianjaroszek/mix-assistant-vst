#include "ma/Resonance.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ma
{
namespace
{
constexpr double kMinProminenceDb = 6.0;
constexpr double kMinPersistence = 0.7; // harmonics of different notes can share a bin half the time
constexpr double kBelowLoudestDb = 35.0; // quieter peaks than this under the channel's loudest are inaudible
constexpr int kMinFrames = 20;
} // namespace

Resonances findResonances(const SpectrumAnalyzer& analyser, double minHz, double maxHz)
{
    Resonances found {};
    const auto& sum = analyser.binPowerSum();
    const auto& peaks = analyser.binStandsOut();
    const int bins = static_cast<int>(sum.size());
    const int frames = analyser.framesAnalysed();
    const int active = analyser.activeFrames();
    if (bins < 16 || frames < kMinFrames || active < kMinFrames)
        return found;

    const double binHz = analyser.binHz();
    std::vector<double> db(static_cast<size_t>(bins), -200.0);
    double loudest = -200.0;
    for (int k = 1; k < bins; ++k)
    {
        db[static_cast<size_t>(k)] = 10.0 * std::log10(std::max(sum[static_cast<size_t>(k)] / frames, 1e-30));
        loudest = std::max(loudest, db[static_cast<size_t>(k)]);
    }

    struct Candidate
    {
        double hz, prominence, q, persistence;
    };
    std::vector<Candidate> candidates;
    std::vector<double> scratch;
    const int first = std::max(3, static_cast<int>(minHz / binHz));
    const int last = std::min(bins - 3, static_cast<int>(maxHz / binHz));
    for (int k = first; k <= last; ++k)
    {
        const double level = db[static_cast<size_t>(k)];
        if (level < loudest - kBelowLoudestDb)
            continue;
        const int near = std::max(2, static_cast<int>(k * 0.03));
        bool isMax = true;
        for (int j = std::max(1, k - near); j <= std::min(bins - 1, k + near) && isMax; ++j)
            isMax = j == k || db[static_cast<size_t>(j)] < level;
        if (!isMax)
            continue;

        // Baseline: the median level over a third of an octave either side.
        const int lo = std::max(1, static_cast<int>(k / 1.26));
        const int hi = std::min(bins - 1, static_cast<int>(k * 1.26));
        scratch.assign(db.begin() + lo, db.begin() + hi + 1);
        std::nth_element(scratch.begin(), scratch.begin() + static_cast<long>(scratch.size() / 2), scratch.end());
        const double prominence = level - scratch[scratch.size() / 2];
        if (prominence < kMinProminenceDb)
            continue;

        const int standsOut = peaks[static_cast<size_t>(k - 1)] + peaks[static_cast<size_t>(k)] + peaks[static_cast<size_t>(k + 1)];
        const double persistence = std::min(1.0, static_cast<double>(standsOut) / active);
        if (persistence < kMinPersistence)
            continue;

        // Width at half the prominence gives the Q; the centre is refined between bins.
        const double halfway = level - 0.5 * prominence;
        int left = k, right = k;
        while (left > lo && db[static_cast<size_t>(left - 1)] > halfway)
            --left;
        while (right < hi && db[static_cast<size_t>(right + 1)] > halfway)
            ++right;
        const double a = db[static_cast<size_t>(k - 1)], b = level, c = db[static_cast<size_t>(k + 1)];
        const double denominator = a - 2.0 * b + c;
        const double offset = std::abs(denominator) > 1e-9 ? std::clamp(0.5 * (a - c) / denominator, -0.5, 0.5) : 0.0;
        const double hz = (k + offset) * binHz;
        const double width = std::max(1.0, static_cast<double>(right - left + 1)) * binHz;
        candidates.push_back({ hz, prominence, std::clamp(hz / width, 2.0, 16.0), persistence });
    }

    std::sort(candidates.begin(), candidates.end(), [](const auto& x, const auto& y) { return x.prominence > y.prominence; });
    size_t count = 0;
    for (const auto& candidate : candidates)
    {
        if (count == found.size())
            break;
        const bool apart = std::none_of(found.begin(), found.begin() + static_cast<long>(count), [&](const Resonance& r) {
            return std::abs(std::log2(candidate.hz / r.hz)) < 1.0 / 6.0;
        });
        if (!apart)
            continue;
        found[count++] = { static_cast<float>(candidate.hz), static_cast<float>(candidate.prominence), static_cast<float>(candidate.q),
                           static_cast<float>(candidate.persistence) };
    }
    return found;
}

} // namespace ma
