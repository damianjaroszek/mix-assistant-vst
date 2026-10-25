#pragma once

#include "Biquad.h"

#include <array>
#include <cstdint>
#include <vector>

namespace ma
{

// Loudness meter after ITU-R BS.1770-4 / EBU R128.
// Momentary (400 ms), short-term (3 s) and gated integrated loudness.
// Integrated loudness uses a fixed histogram, so process() never allocates
// and is safe to call from the audio thread.
class LoudnessMeter
{
public:
    void prepare(double sampleRate, int numChannels);
    void reset();

    // Planar input: channels[c][i]. Channels beyond the prepared count are ignored.
    void process(const float* const* channels, int numChannels, int numSamples);

    double momentaryLufs() const;
    double shortTermLufs() const;
    double integratedLufs() const;

private:
    struct ChannelFilter
    {
        Biquad shelf, highpass;
    };

    static constexpr int kSubBlocksShortTerm = 30; // 30 x 100 ms
    static constexpr int kSubBlocksMomentary = 4;  // 4 x 100 ms
    static constexpr double kHistogramMinLufs = -70.0;
    static constexpr double kHistogramMaxLufs = 10.0;
    static constexpr double kHistogramStep = 0.1;
    static constexpr int kHistogramBins = 800;

    void finishSubBlock();
    double averageOfLastSubBlocks(int count) const;

    std::vector<ChannelFilter> filters;
    int subBlockLength = 4800;
    int subBlockFill = 0;
    double subBlockSum = 0.0;

    std::array<double, kSubBlocksShortTerm> subBlocks {};
    int subBlockWrite = 0;
    int subBlocksAvailable = 0;

    std::array<std::uint64_t, kHistogramBins> histogramCount {};
    std::array<double, kHistogramBins> histogramEnergy {};
};

} // namespace ma
