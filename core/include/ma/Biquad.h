#pragma once

#include <cmath>
#include <algorithm>
#include <complex>

namespace ma
{

// Second-order IIR section, transposed direct form II, double precision.
// Coefficients are normalised so that a0 == 1.
struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;

    void reset() { z1 = z2 = 0.0; }

    // RBJ cookbook peaking (bell) filter.
    static Biquad peaking(double sampleRate, double hz, double gainDb, double q)
    {
        constexpr double kPi = 3.14159265358979323846;
        const double a = std::pow(10.0, gainDb / 40.0);
        const double w = 2.0 * kPi * hz / sampleRate;
        const double alpha = std::sin(w) / (2.0 * q);
        const double cosw = std::cos(w);
        const double a0 = 1.0 + alpha / a;
        Biquad f;
        f.b0 = (1.0 + alpha * a) / a0;
        f.b1 = -2.0 * cosw / a0;
        f.b2 = (1.0 - alpha * a) / a0;
        f.a1 = -2.0 * cosw / a0;
        f.a2 = (1.0 - alpha / a) / a0;
        return f;
    }

    // Magnitude response at `hz`, in dB.
    double magnitudeDb(double hz, double sampleRate) const
    {
        constexpr double kPi = 3.14159265358979323846;
        const std::complex<double> z1inv = std::polar(1.0, -2.0 * kPi * hz / sampleRate);
        const std::complex<double> z2inv = z1inv * z1inv;
        const auto h = (b0 + b1 * z1inv + b2 * z2inv) / (1.0 + a1 * z1inv + a2 * z2inv);
        return 20.0 * std::log10(std::max(std::abs(h), 1e-12));
    }

    double process(double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

} // namespace ma
