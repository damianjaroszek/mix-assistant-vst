#include "ma/EqualLoudness.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace ma
{
namespace
{
// ISO 226:2003 Table 1: frequency, exponent alpha_f, magnitude L_U, hearing threshold T_f.
constexpr int kRows = 29;
constexpr std::array<double, kRows> kHz { 20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500,
                                          630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000,
                                          10000, 12500 };
constexpr std::array<double, kRows> kAlpha { 0.532, 0.506, 0.480, 0.455, 0.432, 0.409, 0.387, 0.367, 0.349, 0.330,
                                             0.315, 0.301, 0.288, 0.276, 0.267, 0.259, 0.253, 0.250, 0.246, 0.244,
                                             0.243, 0.243, 0.243, 0.242, 0.242, 0.245, 0.254, 0.271, 0.301 };
constexpr std::array<double, kRows> kLu { -31.6, -27.2, -23.0, -19.1, -15.9, -13.0, -10.3, -8.1, -6.2, -4.5,
                                          -3.1, -2.0, -1.1, -0.4, 0.0, 0.3, 0.5, 0.0, -2.7, -4.1,
                                          -1.0, 1.7, 2.5, 1.2, -2.1, -7.1, -11.2, -10.7, -3.1 };
constexpr std::array<double, kRows> kTf { 78.5, 68.7, 59.5, 51.1, 44.0, 37.5, 31.5, 26.5, 22.1, 17.9,
                                          14.4, 11.4, 8.6, 6.2, 4.4, 3.0, 2.2, 2.4, 3.5, 1.7,
                                          -1.3, -4.2, -6.0, -5.4, -1.5, 6.0, 12.6, 13.9, 12.3 };

double splAtRow(int row, double phon)
{
    const double alpha = kAlpha[static_cast<size_t>(row)];
    const double lu = kLu[static_cast<size_t>(row)];
    const double tf = kTf[static_cast<size_t>(row)];

    const double af = 4.47e-3 * (std::pow(10.0, 0.025 * phon) - 1.15)
                      + std::pow(0.4 * std::pow(10.0, (tf + lu) / 10.0 - 9.0), alpha);
    return 10.0 / alpha * std::log10(af) - lu + 94.0;
}
} // namespace

double equalLoudnessSpl(double hz, double phon)
{
    if (hz <= kHz.front())
        return splAtRow(0, phon);
    if (hz >= kHz.back())
        return splAtRow(kRows - 1, phon);

    // Interpolate between the two table rows on a log-frequency axis.
    const auto upper = std::upper_bound(kHz.begin(), kHz.end(), hz);
    const int hi = static_cast<int>(upper - kHz.begin());
    const int lo = hi - 1;
    const double t = std::log(hz / kHz[static_cast<size_t>(lo)])
                     / std::log(kHz[static_cast<size_t>(hi)] / kHz[static_cast<size_t>(lo)]);
    return (1.0 - t) * splAtRow(lo, phon) + t * splAtRow(hi, phon);
}

double equalLoudnessWeightDb(double hz, double phon)
{
    return equalLoudnessSpl(hz, phon) - equalLoudnessSpl(1000.0, phon);
}

} // namespace ma
