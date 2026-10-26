#include "ma/MasterPreview.h"

#include <algorithm>
#include <cmath>

namespace ma
{
namespace
{
constexpr double kLookaheadSeconds = 0.005;
constexpr double kReleaseSeconds = 0.08;
constexpr double kGainGlideSeconds = 0.05;
constexpr double kPlayingPower = 1e-5; // -50 dBFS mean square: quieter frames do not count
} // namespace

void MasterPreview::prepare(double rate, int numChannels)
{
    sampleRate = rate;
    channelCount = std::max(1, numChannels);
    lookahead = std::max(1, static_cast<int>(std::lround(kLookaheadSeconds * rate)));
    ceiling = std::pow(10.0, kCeilingDb / 20.0);
    gainCoefficient = std::exp(-1.0 / (kGainGlideSeconds * rate));
    releaseCoefficient = std::exp(-1.0 / (kReleaseSeconds * rate));
    delay.assign(static_cast<size_t>(channelCount), std::vector<double>(static_cast<size_t>(lookahead), 0.0));
    queueValue.assign(static_cast<size_t>(lookahead + 1), 1.0);
    queueIndex.assign(static_cast<size_t>(lookahead + 1), 0);
    averageRing.assign(static_cast<size_t>(lookahead), 1.0);
    chunk.assign(static_cast<size_t>(channelCount), std::vector<float>(kChunk, 0.0f));
    chunkPointers.resize(static_cast<size_t>(channelCount));
    for (int c = 0; c < channelCount; ++c)
        chunkPointers[static_cast<size_t>(c)] = chunk[static_cast<size_t>(c)].data();
    meter.prepare(rate, channelCount);
    frameLength = std::max(1, static_cast<int>(std::lround(0.1 * rate)));
    gain = targetGain;
    reset();
}

void MasterPreview::reset()
{
    for (auto& d : delay)
        std::fill(d.begin(), d.end(), 0.0);
    delayPos = 0;
    queueHead = queueSize = 0;
    sampleIndex = 0;
    std::fill(averageRing.begin(), averageRing.end(), 1.0);
    averagePos = 0;
    averageSum = static_cast<double>(lookahead);
    limiterGain = 1.0;
    chunkFill = 0;
    meter.reset();
    frameFill = 0;
    frameMaxReduction = frameInputPower = 0.0;
    frames = heavyFrames = 0;
    reductionSum = reductionMax = outputPeak = 0.0;
    histogram.fill(0);
}

void MasterPreview::process(const float* const* channels, int numChannels, int numSamples)
{
    if (delay.empty())
        return;
    const int used = std::min(numChannels, channelCount);
    const int capacity = lookahead + 1;
    for (int i = 0; i < numSamples; ++i)
    {
        gain = targetGain + (gain - targetGain) * gainCoefficient;

        // The gain this sample needs to stay under the ceiling.
        double peak = 0.0, power = 0.0;
        for (int c = 0; c < used; ++c)
        {
            const double x = channels[c][i];
            power += x * x;
            peak = std::max(peak, std::abs(x * gain));
        }
        frameInputPower += power / std::max(1, used);
        const double needed = peak > ceiling ? ceiling / peak : 1.0;

        // Sliding minimum over the last `lookahead` samples.
        while (queueSize > 0 && queueValue[static_cast<size_t>((queueHead + queueSize - 1) % capacity)] >= needed)
            --queueSize;
        const int tail = (queueHead + queueSize) % capacity;
        queueValue[static_cast<size_t>(tail)] = needed;
        queueIndex[static_cast<size_t>(tail)] = sampleIndex;
        ++queueSize;
        while (queueIndex[static_cast<size_t>(queueHead)] <= sampleIndex - lookahead)
        {
            queueHead = (queueHead + 1) % capacity;
            --queueSize;
        }
        const double held = queueValue[static_cast<size_t>(queueHead)];
        ++sampleIndex;

        // Its moving average: a smooth attack that is down in time for the peak.
        averageSum += held - averageRing[static_cast<size_t>(averagePos)];
        averageRing[static_cast<size_t>(averagePos)] = held;
        averagePos = (averagePos + 1) % lookahead;
        const double smooth = std::min(1.0, averageSum / lookahead);
        limiterGain = smooth < limiterGain ? smooth : std::min(smooth, smooth + (limiterGain - smooth) * releaseCoefficient);

        // The oldest delayed sample goes out with that gain.
        for (int c = 0; c < channelCount; ++c)
        {
            auto& d = delay[static_cast<size_t>(c)];
            const double out = d[static_cast<size_t>(delayPos)] * limiterGain;
            outputPeak = std::max(outputPeak, std::abs(out));
            d[static_cast<size_t>(delayPos)] = c < used ? channels[c][i] * gain : 0.0;
            chunk[static_cast<size_t>(c)][static_cast<size_t>(chunkFill)] = static_cast<float>(out);
        }
        delayPos = (delayPos + 1) % lookahead;
        if (++chunkFill == kChunk)
        {
            meter.process(chunkPointers.data(), channelCount, kChunk);
            chunkFill = 0;
        }

        frameMaxReduction = std::max(frameMaxReduction, -20.0 * std::log10(std::max(limiterGain, 1e-6)));
        if (++frameFill == frameLength)
            finishFrame();
    }
}

void MasterPreview::finishFrame()
{
    if (frameInputPower / frameLength > kPlayingPower)
    {
        ++frames;
        reductionSum += frameMaxReduction;
        reductionMax = std::max(reductionMax, frameMaxReduction);
        heavyFrames += frameMaxReduction > kHeavyReductionDb ? 1 : 0;
        ++histogram[static_cast<size_t>(std::clamp(static_cast<int>(frameMaxReduction * 10.0), 0, kHistogramBins - 1))];
    }
    frameFill = 0;
    frameMaxReduction = frameInputPower = 0.0;
}

PreviewStats MasterPreview::stats() const
{
    PreviewStats s;
    s.seconds = frames * static_cast<double>(frameLength) / sampleRate;
    if (frames == 0)
        return s;
    s.averageReductionDb = reductionSum / static_cast<double>(frames);
    s.maxReductionDb = reductionMax;
    s.heavyShare = static_cast<double>(heavyFrames) / static_cast<double>(frames);
    long below = 0;
    const long wanted = static_cast<long>(std::ceil(0.95 * static_cast<double>(frames)));
    for (int b = 0; b < kHistogramBins; ++b)
    {
        below += histogram[static_cast<size_t>(b)];
        if (below >= wanted)
        {
            s.typicalReductionDb = (b + 1) * 0.1;
            break;
        }
    }
    s.outputLufs = meter.integratedLufs();
    s.outputPeakDb = outputPeak > 0.0 ? 20.0 * std::log10(outputPeak) : -INFINITY;
    return s;
}

} // namespace ma
