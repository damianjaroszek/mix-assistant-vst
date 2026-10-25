#include "ma/TruePeakMeter.h"

#include "ma/Decibels.h"

#include <algorithm>
#include <cmath>

namespace ma
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
}

TruePeakMeter::TruePeakMeter()
{
    // Windowed-sinc interpolator, cut-off at the original Nyquist frequency.
    constexpr int length = kPhases * kTapsPerPhase;
    const double centre = (length - 1) / 2.0;

    for (int n = 0; n < length; ++n)
    {
        const double x = (n - centre) / kPhases;
        const double sinc = x == 0.0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
        // Blackman window.
        const double w = 0.42 - 0.5 * std::cos(2.0 * kPi * n / (length - 1))
                         + 0.08 * std::cos(4.0 * kPi * n / (length - 1));
        coefficients[static_cast<size_t>(n % kPhases)][static_cast<size_t>(n / kPhases)] = sinc * w;
    }

    // Normalise every phase to unity DC gain.
    for (auto& phase : coefficients)
    {
        double sum = 0.0;
        for (double c : phase)
            sum += c;
        for (double& c : phase)
            c /= sum;
    }
}

void TruePeakMeter::prepare(int numChannels)
{
    history.assign(static_cast<size_t>(std::max(1, numChannels)), {});
    historyPos.assign(history.size(), 0);
    reset();
}

void TruePeakMeter::reset()
{
    for (auto& h : history)
        h.fill(0.0);
    std::fill(historyPos.begin(), historyPos.end(), 0);
    maxTruePeak = 0.0;
    maxSamplePeak = 0.0;
}

void TruePeakMeter::process(const float* const* channels, int numChannels, int numSamples)
{
    const int used = std::min(numChannels, static_cast<int>(history.size()));

    for (int c = 0; c < used; ++c)
    {
        auto& h = history[static_cast<size_t>(c)];
        int pos = historyPos[static_cast<size_t>(c)];

        for (int i = 0; i < numSamples; ++i)
        {
            const double x = channels[c][i];
            maxSamplePeak = std::max(maxSamplePeak, std::abs(x));

            pos = pos == 0 ? kTapsPerPhase - 1 : pos - 1;
            h[static_cast<size_t>(pos)] = x;
            h[static_cast<size_t>(pos + kTapsPerPhase)] = x;
            const double* newestFirst = h.data() + pos;

            for (const auto& phase : coefficients)
            {
                double y = 0.0;
                for (int k = 0; k < kTapsPerPhase; ++k)
                    y += phase[static_cast<size_t>(k)] * newestFirst[k];
                maxTruePeak = std::max(maxTruePeak, std::abs(y));
            }
        }
        historyPos[static_cast<size_t>(c)] = pos;
    }
}

double TruePeakMeter::truePeakDb() const
{
    // The interpolated peak can never be below the sample peak.
    return gainToDb(std::max(maxTruePeak, maxSamplePeak));
}

double TruePeakMeter::samplePeakDb() const
{
    return gainToDb(maxSamplePeak);
}

} // namespace ma
