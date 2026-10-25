#include "ma/CorrelationMeter.h"

#include <cmath>

namespace ma
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

// Butterworth low-pass section (Q = 1/sqrt(2)); two in series make a Linkwitz-Riley.
Biquad makeLowpass(double sampleRate, double hz)
{
    const double k = std::tan(kPi * hz / sampleRate);
    const double q = 1.0 / std::sqrt(2.0);
    const double norm = 1.0 / (1.0 + k / q + k * k);
    Biquad f;
    f.b0 = k * k * norm;
    f.b1 = 2.0 * f.b0;
    f.b2 = f.b0;
    f.a1 = 2.0 * (k * k - 1.0) * norm;
    f.a2 = (1.0 - k / q + k * k) * norm;
    return f;
}
} // namespace

void CorrelationMeter::prepare(double sampleRate, double lowCrossoverHz)
{
    decay = std::exp(-1.0 / sampleRate); // ~1 s time constant
    for (int i = 0; i < 2; ++i)
    {
        lowLeft[i] = makeLowpass(sampleRate, lowCrossoverHz);
        lowRight[i] = makeLowpass(sampleRate, lowCrossoverHz);
    }
    reset();
}

void CorrelationMeter::reset()
{
    for (int i = 0; i < 2; ++i)
    {
        lowLeft[i].reset();
        lowRight[i].reset();
    }
    full = {};
    low = {};
}

void CorrelationMeter::process(const float* left, const float* right, int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
    {
        const double l = left[i], r = right[i];
        full.ll = decay * full.ll + l * l;
        full.rr = decay * full.rr + r * r;
        full.lr = decay * full.lr + l * r;

        const double ll = lowLeft[1].process(lowLeft[0].process(l));
        const double lr = lowRight[1].process(lowRight[0].process(r));
        low.ll = decay * low.ll + ll * ll;
        low.rr = decay * low.rr + lr * lr;
        low.lr = decay * low.lr + ll * lr;
    }
}

double CorrelationMeter::correlation(const Sums& s)
{
    const double denominator = std::sqrt(s.ll * s.rr);
    return denominator > 1e-12 ? s.lr / denominator : 1.0; // silence counts as mono
}

} // namespace ma
