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
constexpr int kErbBands = 40;

// ERB-rate scale (Glasberg & Moore 1990): number of ERBs below a frequency.
double erbRate(double hz)
{
    return 21.4 * std::log10(1.0 + 0.00437 * hz);
}

double erbRateToHz(double erb)
{
    return (std::pow(10.0, erb / 21.4) - 1.0) / 0.00437;
}

const double kErbLow = erbRate(20.0);
const double kErbStep = (erbRate(20000.0) - kErbLow) / kErbBands;

// Band index for a frequency, or -1 outside the layout.
int bandOf(BandScale scale, double hz)
{
    int band = -1;
    if (scale == BandScale::thirdOctave)
        band = static_cast<int>(std::floor(3.0 * std::log2(hz / 1000.0) + kBandOf1kHz + 0.5));
    else
        band = static_cast<int>(std::floor((erbRate(hz) - kErbLow) / kErbStep));
    return band >= 0 && band < SpectrumAnalyzer::numBandsFor(scale) ? band : -1;
}
} // namespace

int SpectrumAnalyzer::numBandsFor(BandScale scale)
{
    return scale == BandScale::thirdOctave ? kNumBands : kErbBands;
}

double SpectrumAnalyzer::bandCentreHz(BandScale scale, int band)
{
    if (scale == BandScale::thirdOctave)
        return 1000.0 * std::pow(2.0, (band - kBandOf1kHz) / 3.0);
    return erbRateToHz(kErbLow + (band + 0.5) * kErbStep);
}

double SpectrumAnalyzer::bandEdgeHz(BandScale scale, int edge)
{
    if (scale == BandScale::thirdOctave)
        return 1000.0 * std::pow(2.0, (edge - kBandOf1kHz - 0.5) / 3.0);
    return erbRateToHz(kErbLow + edge * kErbStep);
}

void SpectrumAnalyzer::prepare(double newSampleRate, BandScale newScale, int fftOrder)
{
    bandScale = newScale;
    bandCount = numBandsFor(newScale);
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
        binToBand[static_cast<size_t>(bin)] = bandOf(bandScale, bin * sampleRate / fftSize);
    }

    bitReversed.assign(static_cast<size_t>(fftSize), 0);
    for (int i = 1, j = 0; i < fftSize; ++i)
    {
        int bit = fftSize >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        bitReversed[static_cast<size_t>(i)] = j;
    }
    twiddles.resize(static_cast<size_t>(fftSize / 2));
    for (int k = 0; k < fftSize / 2; ++k)
        twiddles[static_cast<size_t>(k)] = std::polar(1.0, -2.0 * kPi * k / fftSize);

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

    for (int b = 0; b < bandCount; ++b)
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

    for (int i = 1; i < n; ++i)
    {
        const int j = bitReversed[static_cast<size_t>(i)];
        if (i < j)
            std::swap(data[static_cast<size_t>(i)], data[static_cast<size_t>(j)]);
    }

    // Iterative radix-2. Products are written out because std::complex multiplication
    // carries NaN/infinity handling that makes it several times slower.
    for (int len = 2; len <= n; len <<= 1)
    {
        const int half = len / 2;
        const int stride = n / len;
        for (int k = 0; k < half; ++k)
        {
            const double wr = twiddles[static_cast<size_t>(k * stride)].real();
            const double wi = twiddles[static_cast<size_t>(k * stride)].imag();
            for (int start = k; start < n; start += len)
            {
                auto& a = data[static_cast<size_t>(start)];
                auto& b = data[static_cast<size_t>(start + half)];
                const double tr = wr * b.real() - wi * b.imag();
                const double ti = wr * b.imag() + wi * b.real();
                b = { a.real() - tr, a.imag() - ti };
                a = { a.real() + tr, a.imag() + ti };
            }
        }
    }
}

SpectrumAnalyzer::Bands SpectrumAnalyzer::averageDb() const
{
    Bands out {};
    out.fill(kMinusInfinityDb);
    for (int b = 0; b < bandCount; ++b)
        out[static_cast<size_t>(b)] = frames > 0 ? powerToDb(sum[static_cast<size_t>(b)] / frames) : kMinusInfinityDb;
    return out;
}

SpectrumAnalyzer::Bands SpectrumAnalyzer::recentDb() const
{
    Bands out {};
    out.fill(kMinusInfinityDb);
    for (int b = 0; b < bandCount; ++b)
        out[static_cast<size_t>(b)] = frames > 0 ? powerToDb(recent[static_cast<size_t>(b)]) : kMinusInfinityDb;
    return out;
}

} // namespace ma
