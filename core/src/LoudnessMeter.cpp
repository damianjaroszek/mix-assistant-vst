#include "ma/LoudnessMeter.h"

#include "ma/Decibels.h"

#include <algorithm>
#include <cmath>

namespace ma
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

// K-weighting pre-filter stage 1: high shelf (head acoustics).
// Analog prototype values as used by libebur128, valid for any sample rate.
Biquad makeShelf(double sampleRate)
{
    const double f0 = 1681.974450955533;
    const double gainDb = 3.999843853973347;
    const double q = 0.7071752369554196;

    const double k = std::tan(kPi * f0 / sampleRate);
    const double vh = std::pow(10.0, gainDb / 20.0);
    const double vb = std::pow(vh, 0.4996667741545416);
    const double a0 = 1.0 + k / q + k * k;

    Biquad f;
    f.b0 = (vh + vb * k / q + k * k) / a0;
    f.b1 = 2.0 * (k * k - vh) / a0;
    f.b2 = (vh - vb * k / q + k * k) / a0;
    f.a1 = 2.0 * (k * k - 1.0) / a0;
    f.a2 = (1.0 - k / q + k * k) / a0;
    return f;
}

// K-weighting stage 2: RLB high-pass.
Biquad makeHighpass(double sampleRate)
{
    const double f0 = 38.13547087602444;
    const double q = 0.5003270373238773;

    const double k = std::tan(kPi * f0 / sampleRate);
    const double a0 = 1.0 + k / q + k * k;

    Biquad f;
    f.b0 = 1.0;
    f.b1 = -2.0;
    f.b2 = 1.0;
    f.a1 = 2.0 * (k * k - 1.0) / a0;
    f.a2 = (1.0 - k / q + k * k) / a0;
    return f;
}

double energyToLufs(double energy)
{
    return energy > 0.0 ? -0.691 + 10.0 * std::log10(energy) : kMinusInfinityDb;
}
} // namespace

void LoudnessMeter::prepare(double sampleRate, int numChannels)
{
    filters.assign(static_cast<size_t>(std::max(1, numChannels)), {});
    for (auto& f : filters)
    {
        f.shelf = makeShelf(sampleRate);
        f.highpass = makeHighpass(sampleRate);
    }
    subBlockLength = std::max(1, static_cast<int>(std::lround(sampleRate * 0.1)));
    reset();
}

void LoudnessMeter::reset()
{
    for (auto& f : filters)
    {
        f.shelf.reset();
        f.highpass.reset();
    }
    subBlockFill = 0;
    subBlockSum = 0.0;
    subBlocks.fill(0.0);
    subBlockWrite = 0;
    subBlocksAvailable = 0;
    histogramCount.fill(0);
    histogramEnergy.fill(0.0);
}

void LoudnessMeter::process(const float* const* channels, int numChannels, int numSamples)
{
    const int used = std::min(numChannels, static_cast<int>(filters.size()));

    for (int i = 0; i < numSamples; ++i)
    {
        // Channel weights are 1.0 for L/R/C; surround weighting is not needed here.
        double sum = 0.0;
        for (int c = 0; c < used; ++c)
        {
            auto& f = filters[static_cast<size_t>(c)];
            const double y = f.highpass.process(f.shelf.process(channels[c][i]));
            sum += y * y;
        }
        subBlockSum += sum;

        if (++subBlockFill == subBlockLength)
            finishSubBlock();
    }
}

void LoudnessMeter::finishSubBlock()
{
    subBlocks[static_cast<size_t>(subBlockWrite)] = subBlockSum / subBlockLength;
    subBlockWrite = (subBlockWrite + 1) % kSubBlocksShortTerm;
    subBlocksAvailable = std::min(subBlocksAvailable + 1, kSubBlocksShortTerm);
    subBlockFill = 0;
    subBlockSum = 0.0;

    // Gating blocks are 400 ms with 75 % overlap, i.e. one per 100 ms sub-block.
    if (subBlocksAvailable < kSubBlocksMomentary)
        return;

    const double energy = averageOfLastSubBlocks(kSubBlocksMomentary);
    const double lufs = energyToLufs(energy);
    if (lufs <= kHistogramMinLufs) // absolute gate
        return;

    int bin = static_cast<int>((lufs - kHistogramMinLufs) / kHistogramStep);
    bin = std::clamp(bin, 0, kHistogramBins - 1);
    histogramCount[static_cast<size_t>(bin)] += 1;
    histogramEnergy[static_cast<size_t>(bin)] += energy;
}

double LoudnessMeter::averageOfLastSubBlocks(int count) const
{
    count = std::min(count, subBlocksAvailable);
    if (count == 0)
        return 0.0;

    double sum = 0.0;
    for (int k = 1; k <= count; ++k)
    {
        const int index = (subBlockWrite - k + kSubBlocksShortTerm) % kSubBlocksShortTerm;
        sum += subBlocks[static_cast<size_t>(index)];
    }
    return sum / count;
}

double LoudnessMeter::momentaryLufs() const
{
    if (subBlocksAvailable < kSubBlocksMomentary)
        return kMinusInfinityDb;
    return energyToLufs(averageOfLastSubBlocks(kSubBlocksMomentary));
}

double LoudnessMeter::shortTermLufs() const
{
    // Until 3 s have passed, report over what is available so meters respond at once.
    if (subBlocksAvailable < kSubBlocksMomentary)
        return kMinusInfinityDb;
    return energyToLufs(averageOfLastSubBlocks(kSubBlocksShortTerm));
}

double LoudnessMeter::integratedLufs() const
{
    std::uint64_t count = 0;
    double energy = 0.0;
    for (int b = 0; b < kHistogramBins; ++b)
    {
        count += histogramCount[static_cast<size_t>(b)];
        energy += histogramEnergy[static_cast<size_t>(b)];
    }
    if (count == 0)
        return kMinusInfinityDb;

    const double relativeGate = energyToLufs(energy / static_cast<double>(count)) - 10.0;

    count = 0;
    energy = 0.0;
    for (int b = 0; b < kHistogramBins; ++b)
    {
        const double binCentre = kHistogramMinLufs + (b + 0.5) * kHistogramStep;
        if (binCentre < relativeGate)
            continue;
        count += histogramCount[static_cast<size_t>(b)];
        energy += histogramEnergy[static_cast<size_t>(b)];
    }
    return count == 0 ? kMinusInfinityDb : energyToLufs(energy / static_cast<double>(count));
}

} // namespace ma
