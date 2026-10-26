#include "ma/ChannelChain.h"

#include "ma/EqualLoudness.h"

#include <algorithm>
#include <cmath>

namespace ma
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kButterworthQ[2] = { 0.54119610014619701, 1.3065629648763766 }; // 4th order

Biquad makeHighPass(double sampleRate, double hz, double q)
{
    const double w = 2.0 * kPi * std::min(hz, 0.45 * sampleRate) / sampleRate;
    const double cosw = std::cos(w), alpha = std::sin(w) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    Biquad f;
    f.b0 = (1.0 + cosw) / 2.0 / a0;
    f.b1 = -(1.0 + cosw) / a0;
    f.b2 = f.b0;
    f.a1 = -2.0 * cosw / a0;
    f.a2 = (1.0 - alpha) / a0;
    return f;
}

Biquad makeLowPass(double sampleRate, double hz, double q)
{
    const double w = 2.0 * kPi * std::min(hz, 0.45 * sampleRate) / sampleRate;
    const double cosw = std::cos(w), alpha = std::sin(w) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    Biquad f;
    f.b0 = (1.0 - cosw) / 2.0 / a0;
    f.b1 = (1.0 - cosw) / a0;
    f.b2 = f.b0;
    f.a1 = -2.0 * cosw / a0;
    f.a2 = (1.0 - alpha) / a0;
    return f;
}

// New coefficients, same state: a change does not click.
void retune(Biquad& target, const Biquad& source)
{
    target.b0 = source.b0;
    target.b1 = source.b1;
    target.b2 = source.b2;
    target.a1 = source.a1;
    target.a2 = source.a2;
}

double coefficient(double sampleRate, double ms)
{
    return std::exp(-1.0 / (std::max(ms, 0.01) * 0.001 * sampleRate));
}

double toDb(double gain)
{
    return 20.0 * std::log10(std::max(gain, 1e-12));
}
} // namespace

// ---- Cleanup ---------------------------------------------------------------------------

double cleanupResponseDb(const CleanupSettings& s, double hz)
{
    double db = 0.0;
    if (s.hpfHz > 0.0f)
        for (double q : kButterworthQ)
            db += makeHighPass(48000.0, s.hpfHz, q).magnitudeDb(hz, 48000.0);
    if (s.mudDb < -0.01f)
        db += Biquad::peaking(48000.0, s.mudHz, s.mudDb, s.mudQ).magnitudeDb(hz, 48000.0);
    return db;
}

void CleanupFilter::prepare(double newSampleRate)
{
    sampleRate = newSampleRate;
    initialised = false;
    update();
    reset();
}

void CleanupFilter::reset()
{
    for (auto& channel : highPass)
        for (auto& f : channel)
            f.reset();
    for (auto& f : mud)
        f.reset();
    for (auto* chain : { &sideHigh, &midLow, &midHigh })
        for (auto& f : *chain)
            f.reset();
}

void CleanupFilter::setSettings(const CleanupSettings& next)
{
    const bool changed = !initialised || next.hpfHz != settings.hpfHz || next.monoBelowHz != settings.monoBelowHz
                         || next.mudHz != settings.mudHz || next.mudDb != settings.mudDb || next.mudQ != settings.mudQ;
    settings = next;
    if (changed)
        update();
}

void CleanupFilter::update()
{
    initialised = true;
    if (settings.hpfHz > 0.0f)
        for (auto& channel : highPass)
            for (size_t k = 0; k < 2; ++k)
                retune(channel[k], makeHighPass(sampleRate, settings.hpfHz, kButterworthQ[k]));
    if (settings.mudDb < -0.01f)
        for (auto& f : mud)
            retune(f, Biquad::peaking(sampleRate, std::min<double>(settings.mudHz, 0.45 * sampleRate), settings.mudDb, settings.mudQ));
    if (settings.monoBelowHz > 0.0f)
    {
        const double q = 1.0 / std::sqrt(2.0);
        for (size_t k = 0; k < 2; ++k)
        {
            retune(sideHigh[k], makeHighPass(sampleRate, settings.monoBelowHz, q));
            retune(midHigh[k], makeHighPass(sampleRate, settings.monoBelowHz, q));
            retune(midLow[k], makeLowPass(sampleRate, settings.monoBelowHz, q));
        }
    }
}

void CleanupFilter::process(float* const* channels, int numChannels, int numSamples)
{
    const int count = std::min(numChannels, 2);
    if (settings.monoBelowHz > 0.0f && count == 2)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const double l = channels[0][i], r = channels[1][i];
            const double m = 0.5 * (l + r);
            const double side = 0.5 * (l - r);
            const double kept = sideHigh[1].process(sideHigh[0].process(side));
            const double mid = midLow[1].process(midLow[0].process(m)) + midHigh[1].process(midHigh[0].process(m));
            channels[0][i] = static_cast<float>(mid + kept);
            channels[1][i] = static_cast<float>(mid - kept);
        }
    }
    const bool hpf = settings.hpfHz > 0.0f, cut = settings.mudDb < -0.01f;
    if (!hpf && !cut)
        return;
    for (int c = 0; c < count; ++c)
    {
        auto* data = channels[c];
        auto& chain = highPass[static_cast<size_t>(c)];
        auto& bell = mud[static_cast<size_t>(c)];
        for (int i = 0; i < numSamples; ++i)
        {
            double x = data[i];
            if (hpf)
                x = chain[1].process(chain[0].process(x));
            if (cut)
                x = bell.process(x);
            data[i] = static_cast<float>(x);
        }
    }
}

// ---- Compressor ------------------------------------------------------------------------

double compressorReductionDb(const CompressorSettings& s, double inputDb)
{
    if (s.on < 0.5f || s.ratio <= 1.0f)
        return 0.0;
    const double slope = 1.0 - 1.0 / s.ratio;
    const double over = inputDb - s.thresholdDb;
    const double knee = std::max(0.0f, s.kneeDb);
    if (2.0 * over <= -knee)
        return 0.0;
    if (knee > 0.0 && 2.0 * std::abs(over) < knee)
        return slope * (over + knee / 2.0) * (over + knee / 2.0) / (2.0 * knee);
    return slope * over;
}

void Compressor::prepare(double newSampleRate)
{
    sampleRate = newSampleRate;
    rmsCoeff = coefficient(sampleRate, 10.0);
    setSettings(settings);
    reset();
}

void Compressor::reset()
{
    meanSquare = 0.0;
    reduction = 0.0;
    peakReductionDb = 0.0;
}

void Compressor::setSettings(const CompressorSettings& next)
{
    settings = next;
    attackCoeff = coefficient(sampleRate, settings.attackMs);
    releaseCoeff = coefficient(sampleRate, settings.releaseMs);
}

void Compressor::process(float* const* channels, int numChannels, int numSamples)
{
    peakReductionDb = 0.0;
    if (settings.on < 0.5f || numChannels <= 0)
        return;
    const int count = std::min(numChannels, 2);
    for (int i = 0; i < numSamples; ++i)
    {
        double sum = 0.0;
        for (int c = 0; c < count; ++c)
            sum += static_cast<double>(channels[c][i]) * channels[c][i];
        meanSquare = rmsCoeff * meanSquare + (1.0 - rmsCoeff) * sum / count;
        const double target = compressorReductionDb(settings, 10.0 * std::log10(std::max(meanSquare, 1e-20)));
        const double k = target > reduction ? attackCoeff : releaseCoeff;
        reduction = k * reduction + (1.0 - k) * target;
        peakReductionDb = std::max(peakReductionDb, reduction);
        const auto gain = static_cast<float>(std::pow(10.0, (settings.makeupDb - reduction) / 20.0));
        for (int c = 0; c < count; ++c)
            channels[c][i] *= gain;
    }
}

// ---- Clipper ---------------------------------------------------------------------------

double Clipper::shape(double x, double c, double k)
{
    const double a = std::abs(x);
    if (a <= k)
        return x;
    if (c - k < 1e-9)
        return std::copysign(c, x);
    return std::copysign(k + (c - k) * std::tanh((a - k) / (c - k)), x);
}

void Clipper::prepare(double)
{
    setSettings(settings);
    reset();
}

void Clipper::reset()
{
    for (auto& o : oversamplers)
        o.reset();
    peakClipDb = 0.0;
}

void Clipper::setSettings(const ClipperSettings& next)
{
    const bool switchedOn = next.on >= 0.5f && settings.on < 0.5f;
    settings = next;
    ceiling = std::pow(10.0, std::min(settings.ceilingDb, 6.0f) / 20.0);
    kneeStart = ceiling * std::pow(10.0, -std::clamp(settings.softness, 0.0f, 1.0f) * 6.0 / 20.0);
    if (switchedOn)
        reset();
}

void Clipper::process(float* const* channels, int numChannels, int numSamples)
{
    peakClipDb = 0.0;
    if (settings.on < 0.5f)
        return;
    double worst = 1.0;
    for (int c = 0; c < std::min(numChannels, kMaxChannels); ++c)
    {
        auto& o = oversamplers[static_cast<size_t>(c)];
        float* data = channels[c];
        for (int i = 0; i < numSamples; ++i)
        {
            double up[Oversampler4x::kFactor];
            o.up(data[i], up);
            for (auto& v : up)
            {
                const double y = shape(v, ceiling, kneeStart);
                if (std::abs(y) > 1e-9)
                    worst = std::max(worst, std::abs(v / y));
                v = y;
            }
            data[i] = static_cast<float>(o.down(up));
        }
    }
    peakClipDb = toDb(worst);
}

// ---- Bass pitch and evener -------------------------------------------------------------

void LowPitchTracker::prepare(double newSampleRate)
{
    sampleRate = newSampleRate;
    for (size_t k = 0; k < 2; ++k)
        lowPass[k] = makeLowPass(sampleRate, 160.0, kButterworthQ[k]);
    levelCoeff = coefficient(sampleRate, 50.0);
    reset();
}

void LowPitchTracker::reset()
{
    for (auto& f : lowPass)
        f.reset();
    meanSquare = previous = sinceCrossing = lastPeriod = pitch = 0.0;
    armed = false;
    holdSamples = 0;
}

double LowPitchTracker::levelDb() const
{
    return 10.0 * std::log10(std::max(meanSquare, 1e-20));
}

double LowPitchTracker::process(double x)
{
    const double y = lowPass[1].process(lowPass[0].process(x));
    meanSquare = levelCoeff * meanSquare + (1.0 - levelCoeff) * y * y;
    const double threshold = 0.3 * std::sqrt(meanSquare);
    sinceCrossing += 1.0;
    if (y < -threshold)
        armed = true;
    if (armed && previous < 0.0 && y >= 0.0 && meanSquare > 1e-7) // above -70 dBFS
    {
        const double fraction = -previous / (y - previous); // where between the samples it crossed
        const double period = sinceCrossing - 1.0 + fraction;
        const double hz = sampleRate / period;
        // Two similar periods in a row make a note; one alone may be a harmonic's crossing.
        if (hz >= 25.0 && hz <= 160.0 && lastPeriod > 0.0 && std::abs(period - lastPeriod) < 0.15 * period)
        {
            pitch = pitch > 0.0 && std::abs(hz - pitch) < 0.06 * pitch ? 0.7 * pitch + 0.3 * hz : hz;
            holdSamples = static_cast<int>(sampleRate / 20.0);
        }
        lastPeriod = period;
        sinceCrossing = 1.0 - fraction;
        armed = false;
    }
    previous = y;
    if (holdSamples > 0 && --holdSamples == 0)
        pitch = 0.0;
    return pitch;
}

double bassEvenerGainDb(const BassEvenerSettings& s, double noteHz)
{
    if (noteHz <= 0.0 || s.referenceHz <= 0.0f)
        return 0.0;
    const double gap = equalLoudnessWeightDb(noteHz) - equalLoudnessWeightDb(s.referenceHz);
    return std::clamp(s.amount * gap, -6.0, 6.0);
}

void BassEvener::prepare(double newSampleRate)
{
    sampleRate = newSampleRate;
    tracker.prepare(sampleRate);
    smoothCoeff = coefficient(sampleRate, 30.0);
    for (size_t hz = 0; hz < table.size(); ++hz)
        table[hz] = static_cast<float>(bassEvenerGainDb(settings, static_cast<double>(hz)));
    reset();
}

void BassEvener::reset()
{
    tracker.reset();
    gainDb = 0.0;
    appliedGain = 1.0;
}

void BassEvener::setSettings(const BassEvenerSettings& next)
{
    const bool changed = next.amount != settings.amount || next.referenceHz != settings.referenceHz;
    const bool switchedOff = settings.on >= 0.5f && next.on < 0.5f;
    settings = next;
    if (switchedOff)
        reset(); // a later switch-on starts from unity, not from the last note's gain
    if (changed)
        for (size_t hz = 0; hz < table.size(); ++hz)
            table[hz] = static_cast<float>(bassEvenerGainDb(settings, static_cast<double>(hz)));
}

void BassEvener::process(float* const* channels, int numChannels, int numSamples)
{
    if (settings.on < 0.5f || numChannels <= 0)
        return;
    const int count = std::min(numChannels, 2);
    for (int i = 0; i < numSamples; ++i)
    {
        double mono = 0.0;
        for (int c = 0; c < count; ++c)
            mono += channels[c][i];
        const double note = tracker.process(mono / count);
        // Between notes the last gain holds, so tails do not jump.
        if (note > 0.0)
        {
            const auto index = static_cast<size_t>(std::clamp(std::lround(note), 0L, static_cast<long>(table.size() - 1)));
            gainDb = smoothCoeff * gainDb + (1.0 - smoothCoeff) * table[index];
        }
        const auto gain = static_cast<float>(std::pow(10.0, gainDb / 20.0));
        for (int c = 0; c < count; ++c)
            channels[c][i] *= gain;
    }
}

} // namespace ma
