#include "ma/Key.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace ma
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr int kFftOrder = 13; // 8192 points
constexpr int kHop = 2048;
constexpr double kTargetRate = 11025.0;
constexpr double kLowestHz = 38.0;   // just under E1, the lowest bass note in practice
constexpr double kHighestHz = 4000.0;
constexpr double kSilentMeanSquare = 1e-6; // -60 dBFS in the analysed range
constexpr double kPeakOverNeighbours = 3.0; // a peak stands 5 dB over the semitone around it
constexpr double kConfidentCorrelation = 0.55;
constexpr double kConfidentMargin = 0.03;
constexpr double kAbsentDb = -200.0;
constexpr double kBassHz = 160.0;
constexpr double kRelativeTieScore = 0.08;
constexpr double kBassPreference = 1.25;

// Key profiles tuned on popular and electronic music (Sha'ath, KeyFinder), C major and C minor.
constexpr std::array<double, 12> kMajorProfile { 7.24, 3.50, 3.58, 2.85, 5.82, 4.56, 2.45, 6.99, 3.39, 4.56, 4.07, 4.46 };
constexpr std::array<double, 12> kMinorProfile { 7.00, 3.14, 4.36, 5.40, 3.67, 4.09, 3.91, 6.20, 3.63, 2.87, 5.35, 3.83 };

int wrap12(int value)
{
    return ((value % 12) + 12) % 12;
}

int relativeMajor(MusicalKey key)
{
    return key.minor ? wrap12(key.tonic + 3) : key.tonic;
}

double correlation(const std::array<double, 12>& chroma, const std::array<double, 12>& profile, int tonic)
{
    double meanC = 0.0, meanP = 0.0;
    for (int i = 0; i < 12; ++i)
    {
        meanC += chroma[static_cast<size_t>(i)];
        meanP += profile[static_cast<size_t>(i)];
    }
    meanC /= 12.0;
    meanP /= 12.0;
    double cp = 0.0, cc = 0.0, pp = 0.0;
    for (int i = 0; i < 12; ++i)
    {
        const double c = chroma[static_cast<size_t>(i)] - meanC;
        const double p = profile[static_cast<size_t>(wrap12(i - tonic))] - meanP;
        cp += c * p;
        cc += c * c;
        pp += p * p;
    }
    return cc > 0.0 && pp > 0.0 ? cp / std::sqrt(cc * pp) : 0.0;
}

Biquad lowPass(double sampleRate, double hz, double q)
{
    const double w = 2.0 * kPi * hz / sampleRate;
    const double alpha = std::sin(w) / (2.0 * q);
    const double cosw = std::cos(w);
    const double a0 = 1.0 + alpha;
    Biquad f;
    f.b0 = (1.0 - cosw) / 2.0 / a0;
    f.b1 = (1.0 - cosw) / a0;
    f.b2 = f.b0;
    f.a1 = -2.0 * cosw / a0;
    f.a2 = (1.0 - alpha) / a0;
    return f;
}
} // namespace

std::string keyName(MusicalKey key)
{
    if (!key.valid())
        return "?";
    static const char* const kMajor[] { "C", "Des", "D", "Es", "E", "F", "Fis", "G", "As", "A", "B", "H" };
    static const char* const kMinor[] { "c", "cis", "d", "es", "e", "f", "fis", "g", "gis", "a", "b", "h" };
    return key.minor ? std::string(kMinor[key.tonic]) + "-moll" : std::string(kMajor[key.tonic]) + "-dur";
}

int camelotNumber(MusicalKey key)
{
    if (!key.valid())
        return 0;
    // C major is 8B and each step is a fifth; a minor key shares its relative major's number.
    return wrap12(7 * relativeMajor(key) + 7) + 1;
}

std::string camelotCode(MusicalKey key)
{
    if (!key.valid())
        return "?";
    return std::to_string(camelotNumber(key)) + (key.minor ? "A" : "B");
}

MusicalKey keyFromCamelot(int number, bool minor)
{
    if (number < 1 || number > 12)
        return {};
    // Inverse of camelotNumber: 7 is its own inverse mod 12.
    const int major = wrap12(7 * (number - 1 - 7));
    return { minor ? wrap12(major - 3) : major, minor };
}

int keyIndex(MusicalKey key)
{
    return key.valid() ? key.tonic + (key.minor ? 12 : 0) : -1;
}

MusicalKey keyFromIndex(int index)
{
    if (index < 0 || index >= 24)
        return {};
    return { index % 12, index >= 12 };
}

int keyShiftSemitones(MusicalKey from, MusicalKey to)
{
    if (!from.valid() || !to.valid())
        return 0;
    const int up = wrap12(relativeMajor(to) - relativeMajor(from));
    return up > 6 ? up - 12 : up;
}

KeyEstimate estimateKey(const std::array<double, 12>& chroma, const std::array<double, 12>& bass)
{
    KeyEstimate out;
    if (std::accumulate(chroma.begin(), chroma.end(), 0.0) <= 0.0)
        return out;
    std::array<double, 24> scores {};
    for (int k = 0; k < 24; ++k)
        scores[static_cast<size_t>(k)] = correlation(chroma, k < 12 ? kMajorProfile : kMinorProfile, k % 12);
    auto best = static_cast<int>(std::max_element(scores.begin(), scores.end()) - scores.begin());
    // Its relative: the minor a minor third under the major, the major a minor third over the minor.
    const int relative = best < 12 ? 12 + wrap12(best - 3) : wrap12(best - 12 + 3);
    if (scores[static_cast<size_t>(relative)] >= scores[static_cast<size_t>(best)] - kRelativeTieScore)
    {
        const int minorKey = best >= 12 ? best : relative, majorKey = best < 12 ? best : relative;
        const double onMinor = bass[static_cast<size_t>(minorKey - 12)], onMajor = bass[static_cast<size_t>(majorKey)];
        best = onMajor > kBassPreference * onMinor ? majorKey : minorKey;
    }
    out.key = keyFromIndex(best);
    out.correlation = scores[static_cast<size_t>(best)];
    // The relative key has the same notes and the same shift, so it is not a rival.
    double rival = -1.0;
    for (int k = 0; k < 24; ++k)
        if (camelotNumber(keyFromIndex(k)) != camelotNumber(out.key))
            rival = std::max(rival, scores[static_cast<size_t>(k)]);
    out.margin = out.correlation - rival;
    out.confident = out.correlation >= kConfidentCorrelation && out.margin >= kConfidentMargin;
    return out;
}

void KeyDetector::prepare(double sampleRate)
{
    factor = std::max(1, static_cast<int>(std::floor(sampleRate / kTargetRate + 1e-9)));
    rate = sampleRate / factor;
    const double cutoff = std::min(kHighestHz * 1.1, 0.42 * rate);
    antiAlias[0] = lowPass(sampleRate, cutoff, 0.5412); // 4th order Butterworth
    antiAlias[1] = lowPass(sampleRate, cutoff, 1.3066);

    const int n = 1 << kFftOrder;
    window.resize(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        window[static_cast<size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / n);
    input.assign(static_cast<size_t>(n), 0.0);
    power.assign(static_cast<size_t>(n / 2 + 1), 0.0);
    buffer.assign(static_cast<size_t>(n), {});
    twiddles.resize(static_cast<size_t>(n / 2));
    for (int k = 0; k < n / 2; ++k)
        twiddles[static_cast<size_t>(k)] = std::polar(1.0, -2.0 * kPi * k / n);
    bitReversed.assign(static_cast<size_t>(n), 0);
    for (int i = 1, j = 0; i < n; ++i)
    {
        int bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1)
            j ^= bit;
        j ^= bit;
        bitReversed[static_cast<size_t>(i)] = j;
    }
    frameSeconds = kHop / rate;
    reset();
}

void KeyDetector::reset()
{
    for (auto& f : antiAlias)
        f.reset();
    std::fill(input.begin(), input.end(), 0.0);
    phase = 0;
    fill = 0;
    sum = {};
    bassSum = {};
    heardSeconds = 0.0;
}

void KeyDetector::process(const float* const* channels, int numChannels, int numSamples)
{
    if (numChannels <= 0 || input.empty())
        return;
    const double scale = 1.0 / numChannels;
    const int n = static_cast<int>(input.size());
    for (int i = 0; i < numSamples; ++i)
    {
        double x = 0.0;
        for (int c = 0; c < numChannels; ++c)
            x += channels[c][i];
        x *= scale;
        for (auto& f : antiAlias)
            x = f.process(x);
        if (++phase < factor)
            continue;
        phase = 0;
        input[static_cast<size_t>(fill++)] = x;
        if (fill == n)
        {
            analyseFrame();
            std::copy(input.begin() + kHop, input.end(), input.begin());
            fill = n - kHop;
        }
    }
}

void KeyDetector::analyseFrame()
{
    const int n = static_cast<int>(input.size());
    for (int i = 0; i < n; ++i)
        buffer[static_cast<size_t>(bitReversed[static_cast<size_t>(i)])] = { input[static_cast<size_t>(i)] * window[static_cast<size_t>(i)], 0.0 };
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
                auto& a = buffer[static_cast<size_t>(start)];
                auto& b = buffer[static_cast<size_t>(start + half)];
                const double tr = wr * b.real() - wi * b.imag();
                const double ti = wr * b.imag() + wi * b.real();
                b = { a.real() - tr, a.imag() - ti };
                a = { a.real() + tr, a.imag() + ti };
            }
        }
    }

    // Mean-square scale of a Hann-windowed bin (as the spectrum analyser reads it).
    const double meanSquare = 2.0 / (static_cast<double>(n) * 0.375 * n);
    const double binHz = rate / n;
    const int first = std::max(2, static_cast<int>(std::ceil(kLowestHz / binHz)));
    const int last = std::min(n / 2 - 2, static_cast<int>(std::floor(kHighestHz / binHz)));
    double total = 0.0;
    for (int k = first - 1; k <= last + 1; ++k)
    {
        power[static_cast<size_t>(k)] = std::norm(buffer[static_cast<size_t>(k)]) * meanSquare;
        if (k >= first && k <= last)
            total += power[static_cast<size_t>(k)];
    }
    if (total < kSilentMeanSquare)
        return;

    std::array<double, 12> frame {}, bassFrame {};
    for (int k = first; k <= last; ++k)
    {
        const double p = power[static_cast<size_t>(k)];
        if (p <= power[static_cast<size_t>(k - 1)] || p < power[static_cast<size_t>(k + 1)])
            continue;
        // Its neighbourhood: about a semitone either side, outside the window's main lobe.
        const int reach = std::max(4, static_cast<int>(k * 0.06));
        double around = 0.0;
        int count = 0;
        for (int j = std::max(first - 1, k - reach); j <= std::min(last + 1, k + reach); ++j)
            if (std::abs(j - k) > 2)
            {
                around += power[static_cast<size_t>(j)];
                ++count;
            }
        if (count == 0 || p < kPeakOverNeighbours * around / count)
            continue;
        // Parabolic interpolation on the log power for the true frequency.
        const double l = std::log(power[static_cast<size_t>(k - 1)] + 1e-30), c = std::log(p + 1e-30),
                     r = std::log(power[static_cast<size_t>(k + 1)] + 1e-30);
        const double denominator = l - 2.0 * c + r;
        const double offset = std::abs(denominator) > 1e-12 ? std::clamp(0.5 * (l - r) / denominator, -0.5, 0.5) : 0.0;
        const double hz = (k + offset) * binHz;
        const double note = 69.0 + 12.0 * std::log2(hz / 440.0);
        const double nearest = std::round(note);
        const double off = note - nearest; // -0.5..0.5; between two notes counts for neither
        const double weight = std::sqrt(p) * (1.0 - 4.0 * off * off);
        frame[static_cast<size_t>(wrap12(static_cast<int>(nearest)))] += weight;
        if (hz < kBassHz)
            bassFrame[static_cast<size_t>(wrap12(static_cast<int>(nearest)))] += weight;
    }
    const double frameSum = std::accumulate(frame.begin(), frame.end(), 0.0);
    if (frameSum <= 0.0)
        return;
    // Every frame adds up to its level, so a loud drop counts more than a quiet break.
    const double level = std::sqrt(total);
    for (int i = 0; i < 12; ++i)
    {
        sum[static_cast<size_t>(i)] += frame[static_cast<size_t>(i)] / frameSum * level;
        bassSum[static_cast<size_t>(i)] += bassFrame[static_cast<size_t>(i)] / frameSum * level;
    }
    heardSeconds += frameSeconds;
}

SpectrumAnalyzer::Bands shiftErbSpectrum(const SpectrumAnalyzer::Bands& db, int semitones)
{
    const int bands = SpectrumAnalyzer::numBandsFor(BandScale::erb);
    if (semitones == 0)
        return db;
    std::array<double, SpectrumAnalyzer::kMaxBands + 1> edge {}, below {};
    for (int e = 0; e <= bands; ++e)
        edge[static_cast<size_t>(e)] = SpectrumAnalyzer::bandEdgeHz(BandScale::erb, e);
    for (int b = 0; b < bands; ++b)
    {
        const double v = db[static_cast<size_t>(b)];
        below[static_cast<size_t>(b + 1)] = below[static_cast<size_t>(b)] + (v > kAbsentDb + 1.0 ? std::pow(10.0, v / 10.0) : 0.0);
    }
    // Power below a frequency, spread evenly over each band.
    auto cumulative = [&](double hz) {
        if (hz <= edge[0])
            return 0.0;
        for (int b = 0; b < bands; ++b)
            if (hz < edge[static_cast<size_t>(b + 1)])
            {
                const double t = (hz - edge[static_cast<size_t>(b)]) / (edge[static_cast<size_t>(b + 1)] - edge[static_cast<size_t>(b)]);
                return below[static_cast<size_t>(b)] + t * (below[static_cast<size_t>(b + 1)] - below[static_cast<size_t>(b)]);
            }
        return below[static_cast<size_t>(bands)];
    };
    const double ratio = std::pow(2.0, semitones / 12.0);
    SpectrumAnalyzer::Bands out;
    out.fill(kAbsentDb);
    for (int b = 0; b < bands; ++b)
    {
        // What now lands in this band was 1/ratio as high.
        const double p = cumulative(edge[static_cast<size_t>(b + 1)] / ratio) - cumulative(edge[static_cast<size_t>(b)] / ratio);
        out[static_cast<size_t>(b)] = p > 1e-20 ? 10.0 * std::log10(p) : kAbsentDb;
    }
    return out;
}

} // namespace ma
