#pragma once

#include "LoudnessMeter.h"

#include <array>
#include <cmath>
#include <vector>

namespace ma
{

// Phase 11: a quick master preview, for analysis only (nothing of it reaches the output). The
// mix is brought up to a loudness target through a brick-wall lookahead limiter, and the
// preview measures how much the limiter has to take off. It answers "will this mix stand being
// mastered loud?" before it goes to Ozone: a limiter working hard flattens the kick.
struct PreviewStats
{
    double seconds = 0.0;          // playing time measured
    double averageReductionDb = 0.0; // per 100 ms the most the limiter takes off, averaged over playing time
    double typicalReductionDb = 0.0; // the same, 95th percentile (the loud part)
    double maxReductionDb = 0.0;
    double heavyShare = 0.0;       // share of playing time with more than 3 dB taken off
    double outputLufs = -INFINITY; // integrated loudness of the preview
    double outputPeakDb = -INFINITY; // sample peak of the preview (the ceiling holds)
};

class MasterPreview
{
public:
    static constexpr double kCeilingDb = -1.0;
    static constexpr double kHeavyReductionDb = 3.0;

    // Allocates.
    void prepare(double sampleRate, int numChannels);
    void reset(); // the statistics and the limiter, not the gain
    // The gain that brings the mix to the target; glides there over about 50 ms.
    void setGainDb(double gainDb) { targetGain = std::pow(10.0, gainDb / 20.0); }
    void process(const float* const* channels, int numChannels, int numSamples);
    PreviewStats stats() const;

private:
    static constexpr int kHistogramBins = 400; // 0..40 dB in 0.1 dB steps
    void finishFrame();

    double sampleRate = 48000.0;
    int channelCount = 2;
    int lookahead = 240;
    double ceiling = 0.0;
    double gain = 1.0, targetGain = 1.0, gainCoefficient = 0.0;
    double releaseCoefficient = 0.0;

    // Audio delayed by the lookahead, per channel, and the gains the limiter needs.
    std::vector<std::vector<double>> delay;
    int delayPos = 0;
    // Sliding minimum of the needed gain over the lookahead (a monotonic queue in a ring) ...
    std::vector<double> queueValue;
    std::vector<long long> queueIndex;
    int queueHead = 0, queueSize = 0;
    long long sampleIndex = 0;
    // ... then its moving average over the same length, so the gain is down before the peak.
    std::vector<double> averageRing;
    int averagePos = 0;
    double averageSum = 0.0;
    double limiterGain = 1.0;

    // Output for the loudness meter, in chunks.
    static constexpr int kChunk = 256;
    std::vector<std::vector<float>> chunk;
    std::vector<const float*> chunkPointers;
    int chunkFill = 0;
    LoudnessMeter meter;

    // Statistics per 100 ms frame.
    int frameLength = 4800, frameFill = 0;
    double frameMaxReduction = 0.0, frameInputPower = 0.0;
    long frames = 0, heavyFrames = 0;
    double reductionSum = 0.0, reductionMax = 0.0, outputPeak = 0.0;
    std::array<long, kHistogramBins> histogram {};
};

} // namespace ma
