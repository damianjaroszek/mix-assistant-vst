#pragma once

#include "Biquad.h"

namespace ma
{

// Stereo phase correlation, full band and below a crossover (default 120 Hz),
// averaged over roughly the last second. +1 = mono, 0 = unrelated, -1 = out of phase.
// Club systems sum the low end to mono, so the low-band value is the one that matters.
class CorrelationMeter
{
public:
    void prepare(double sampleRate, double lowCrossoverHz = 120.0);
    void reset();
    void process(const float* left, const float* right, int numSamples);

    double fullBand() const { return correlation(full); }
    double lowBand() const { return correlation(low); }

private:
    struct Sums
    {
        double ll = 0.0, rr = 0.0, lr = 0.0;
    };

    static double correlation(const Sums& s);

    double decay = 0.9999;
    Biquad lowLeft[2], lowRight[2]; // 4th-order Linkwitz-Riley low-pass
    Sums full, low;
};

} // namespace ma
