#include "ma/SpectrumAnalyzer.h"

#include "ma/Decibels.h"

#include <algorithm>
#include <cmath>

namespace ma
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr int kBandOf1kHz = 17;
} // namespace

double SpectrumAnalyzer::bandCentreHz(int band)
{
    return 1000.0 * std::pow(2.0, (band - kBandOf1kHz) / 3.0);
}

void SpectrumAnalyzer::prepare(double newSampleRate, int fftOrder)
{
    sampleRate = newSampleRate;
    fftSize = 1 << fftOrder;
    hopSize = fftSize / 2;

    // About one second of smoothing for the live display.
    const double framesPerSecond = sampleRate / hopSize;
    recentSmoothing = std::exp(-1.0 / framesPerSecond);

    window.resize(static_cast<size_t>(fftSize));
    double windowEnergy = 0.0;
    for (int n = 0; n < fftSize; ++n)
    {
        window[static_cast<size_t>(n)] = 0.5 - 0.5 * std::cos(2.0 * kPi * n / fftSize);
        windowEnergy += window[static_cast<size_t>(n)] * window[static_cast<size_t>(n)];
    }
    // One-sided power spectrum scaled so that the bins sum to the mean square.
    powerScale = 2.0 / (fftSize * windowEnergy);

    binToBand.assign(static_cast<size_t>(fftSize / 2), -1);
    for (int bin = 1; bin < fftSize / 2; ++bin)
    {
        const double hz = bin * sampleRate / fftSize;
        const int band = static_cast<int>(std::floor(3.0 * std::log2(hz / 1000.0) + kBandOf1kHz + 0.5));
        if (band >= 0 && band < kNumBands)
            binToBand[static_cast<size_t>(bin)] = band;
    }

    input.assign(static_cast<size_t>(fftSize), 0.0);
    buffer.assign(static_cast<size_t>(fftSize), {});
    reset();
}

void SpectrumAnalyzer::reset()
{
    std::fill(input.begin(), input.end(), 0.0);
    inputFill = 0;
    sum.fill(0.0);
    recent.fill(0.0);
    frames = 0;
}

void SpectrumAnalyzer::process(const float* const* channels, int numChannels, int numSamples)
{
    if (numChannels <= 0)
        return;

    const double channelScale = 1.0 / numChannels;
    for (int i = 0; i < numSamples; ++i)
    {
        double x = 0.0;
        for (int c = 0; c < numChannels; ++c)
            x += channels[c][i];
        input[static_cast<size_t>(inputFill++)] = x * channelScale;

        if (inputFill == fftSize)
        {
            analyseFrame();
            // Keep the second half for the next, overlapping frame.
            std::copy(input.begin() + hopSize, input.end(), input.begin());
            inputFill = fftSize - hopSize;
        }
    }
}

void SpectrumAnalyzer::analyseFrame()
{
    for (int n = 0; n < fftSize; ++n)
        buffer[static_cast<size_t>(n)] = { input[static_cast<size_t>(n)] * window[static_cast<size_t>(n)], 0.0 };

    fft(buffer);

    frame.fill(0.0);
    for (int bin = 1; bin < fftSize / 2; ++bin)
    {
        const int band = binToBand[static_cast<size_t>(bin)];
        if (band >= 0)
            frame[static_cast<size_t>(band)] += std::norm(buffer[static_cast<size_t>(bin)]) * powerScale;
    }

    for (int b = 0; b < kNumBands; ++b)
    {
        sum[static_cast<size_t>(b)] += frame[static_cast<size_t>(b)];
        recent[static_cast<size_t>(b)] = frames == 0
            ? frame[static_cast<size_t>(b)]
            : recentSmoothing * recent[static_cast<size_t>(b)] + (1.0 - recentSmoothing) * frame[static_cast<size_t>(b)];
    }
    ++frames;
}

void SpectrumAnalyzer::fft(std::vector<std::complex<double>>& data) const
{
    const int n = static_cast<int>(data.size());

    for (int i = 1, j = 0; i < n; ++i)
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(data[static_cast<size_t>(i)], data[static_cast<size_t>(j)]);
    }

    for (int len = 2; len <= n; len <<= 1)
    {
        const double angle = -2.0 * kPi / len;
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (int start = 0; start < n; start += len)
        {
            std::complex<double> w(1.0, 0.0);
            for (int k = 0; k < len / 2; ++k)
            {
                auto& a = data[static_cast<size_t>(start + k)];
                auto& b = data[static_cast<size_t>(start + k + len / 2)];
                const auto t = w * b;
                b = a - t;
                a = a + t;
                w *= step;
            }
        }
    }
}

SpectrumAnalyzer::Bands SpectrumAnalyzer::averageDb() const
{
    Bands out {};
    for (int b = 0; b < kNumBands; ++b)
        out[static_cast<size_t>(b)] = frames > 0 ? powerToDb(sum[static_cast<size_t>(b)] / frames) : kMinusInfinityDb;
    return out;
}

SpectrumAnalyzer::Bands SpectrumAnalyzer::recentDb() const
{
    Bands out {};
    for (int b = 0; b < kNumBands; ++b)
        out[static_cast<size_t>(b)] = frames > 0 ? powerToDb(recent[static_cast<size_t>(b)]) : kMinusInfinityDb;
    return out;
}

} // namespace ma
