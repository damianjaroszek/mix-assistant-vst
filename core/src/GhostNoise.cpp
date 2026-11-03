#include "ma/GhostNoise.h"

#include <algorithm>
#include <cmath>

namespace ma
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
}

void GhostNoise::prepare(double rate, int fftOrder)
{
    sampleRate = rate;
    size = 1 << std::clamp(fftOrder, 8, 14);
    hop = size / 2;
    window.resize(static_cast<size_t>(size));
    for (int i = 0; i < size; ++i)
        window[static_cast<size_t>(i)] = std::sqrt(0.5 - 0.5 * std::cos(2.0 * kPi * i / size)); // square-root Hann (periodic)

    const int bands = SpectrumAnalyzer::numBandsFor(BandScale::erb);
    bandOfBin.assign(static_cast<size_t>(size / 2), -1);
    binsInBand.assign(static_cast<size_t>(bands), 0);
    firstBin.assign(static_cast<size_t>(bands), 0);
    for (int k = 1; k < size / 2; ++k)
    {
        const double hz = k * sampleRate / size;
        for (int b = 0; b < bands; ++b)
            if (hz >= SpectrumAnalyzer::bandEdgeHz(BandScale::erb, b) && hz < SpectrumAnalyzer::bandEdgeHz(BandScale::erb, b + 1))
            {
                bandOfBin[static_cast<size_t>(k)] = b;
                if (binsInBand[static_cast<size_t>(b)]++ == 0)
                    firstBin[static_cast<size_t>(b)] = k;
                break;
            }
    }
    binMagnitude.assign(static_cast<size_t>(size / 2), 0.0);
    spectrumMid.assign(static_cast<size_t>(size), {});
    spectrumSide.assign(static_cast<size_t>(size), {});
    twiddles.resize(static_cast<size_t>(size / 2));
    for (int i = 0; i < size / 2; ++i)
        twiddles[static_cast<size_t>(i)] = std::polar(1.0, 2.0 * kPi * i / size); // inverse: positive exponent
    bitReverse.resize(static_cast<size_t>(size));
    int bits = 0;
    while ((1 << bits) < size)
        ++bits;
    for (int i = 0; i < size; ++i)
    {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            r |= ((i >> b) & 1) << (bits - 1 - b);
        bitReverse[static_cast<size_t>(i)] = r;
    }
    outMid.assign(static_cast<size_t>(size), 0.0);
    outSide.assign(static_cast<size_t>(size), 0.0);
    reset();
}

void GhostNoise::reset()
{
    std::fill(outMid.begin(), outMid.end(), 0.0);
    std::fill(outSide.begin(), outSide.end(), 0.0);
    readPosition = hop; // a new frame is made before the first sample
}

void GhostNoise::setSpectrum(const SpectrumAnalyzer::Bands& db, double sideShare)
{
    side = std::clamp(sideShare, 0.0, 0.9);
    // |X_k|^2 = p_k N^2 / 2 gives a real signal of mean square p_k from bin k. A band's power is
    // spread evenly over its bins; a band narrower than a bin gives it to the bin nearest its centre.
    std::fill(binMagnitude.begin(), binMagnitude.end(), 0.0);
    const int bands = static_cast<int>(binsInBand.size());
    for (int b = 0; b < bands; ++b)
    {
        if (db[static_cast<size_t>(b)] <= -150.0)
            continue;
        const double power = std::pow(10.0, db[static_cast<size_t>(b)] / 10.0);
        const int count = binsInBand[static_cast<size_t>(b)];
        if (count > 0)
        {
            // A band's bins are contiguous.
            const int first = firstBin[static_cast<size_t>(b)];
            for (int k = first; k < first + count; ++k)
                binMagnitude[static_cast<size_t>(k)] += power / count;
        }
        else
        {
            const auto k = static_cast<size_t>(std::clamp(static_cast<int>(std::lround(SpectrumAnalyzer::bandCentreHz(BandScale::erb, b) * size / sampleRate)), 1,
                                                          size / 2 - 1));
            binMagnitude[k] += power;
        }
    }
    for (auto& m : binMagnitude)
        m = size * std::sqrt(m / 2.0);
}

double GhostNoise::random()
{
    // xorshift64*: cheap, no allocation, good enough for phases.
    seed ^= seed >> 12;
    seed ^= seed << 25;
    seed ^= seed >> 27;
    return static_cast<double>((seed * 0x2545F4914F6CDD1Dull) >> 11) * (1.0 / 9007199254740992.0);
}

void GhostNoise::transform(std::vector<std::complex<double>>& data) const
{
    const auto n = static_cast<size_t>(size);
    for (size_t i = 0; i < n; ++i)
    {
        const auto j = static_cast<size_t>(bitReverse[i]);
        if (i < j)
            std::swap(data[i], data[j]);
    }
    for (size_t length = 2; length <= n; length <<= 1)
    {
        const size_t half = length / 2, step = n / length;
        for (size_t start = 0; start < n; start += length)
            for (size_t k = 0; k < half; ++k)
            {
                const auto t = twiddles[k * step] * data[start + k + half];
                data[start + k + half] = data[start + k] - t;
                data[start + k] += t;
            }
    }
}

void GhostNoise::makeFrame()
{
    // Shift the overlap: the second half of the last frame becomes the first.
    std::copy(outMid.begin() + hop, outMid.end(), outMid.begin());
    std::copy(outSide.begin() + hop, outSide.end(), outSide.begin());
    std::fill(outMid.begin() + hop, outMid.end(), 0.0);
    std::fill(outSide.begin() + hop, outSide.end(), 0.0);

    // L = m + s, R = m - s, so the mid ((L+R)/2, what the analysers read as the spectrum) has m
    // and the side ((L-R)/2) s, in the side share's ratio.
    const double midScale = 1.0, sideScale = std::sqrt(side / (1.0 - side));
    const auto half = static_cast<size_t>(size / 2);
    for (auto* spectrum : { &spectrumMid, &spectrumSide })
    {
        auto& x = *spectrum;
        const double scale = spectrum == &spectrumMid ? midScale : sideScale;
        x[0] = {};
        x[half] = {};
        for (size_t k = 1; k < half; ++k)
        {
            x[k] = std::polar(binMagnitude[k] * scale, 2.0 * kPi * random());
            x[static_cast<size_t>(size) - k] = std::conj(x[k]);
        }
        transform(x);
    }
    const double norm = 1.0 / size;
    for (int i = 0; i < size; ++i)
    {
        const auto at = static_cast<size_t>(i);
        outMid[at] += spectrumMid[at].real() * norm * window[at];
        outSide[at] += spectrumSide[at].real() * norm * window[at];
    }
    readPosition = 0;
}

void GhostNoise::process(float* left, float* right, int numSamples, float gain, double beatPhase, double beatSeconds)
{
    const bool pulsed = pulseDecay > 0.0 && beatSeconds > 0.0;
    // A decaying hit per beat, scaled so its mean square over the beat is 1.
    double pulseGain = 1.0;
    if (pulsed)
    {
        const double ratio = beatSeconds / pulseDecay;
        const double meanSquare = (1.0 - std::exp(-2.0 * ratio)) / (2.0 * ratio);
        pulseGain = 1.0 / std::sqrt(std::max(1e-6, meanSquare));
    }
    const double phaseStep = pulsed ? 1.0 / (beatSeconds * sampleRate) : 0.0;
    double phase = beatPhase - std::floor(beatPhase);
    for (int i = 0; i < numSamples; ++i)
    {
        if (readPosition >= hop)
            makeFrame();
        const auto at = static_cast<size_t>(readPosition++);
        double g = gain;
        if (pulsed)
        {
            g *= pulseGain * std::exp(-phase * beatSeconds / pulseDecay);
            phase += phaseStep;
            if (phase >= 1.0)
                phase -= 1.0;
        }
        left[i] += static_cast<float>((outMid[at] + outSide[at]) * g);
        if (right != nullptr)
            right[i] += static_cast<float>((outMid[at] - outSide[at]) * g);
    }
}

} // namespace ma
