#include "ma/Alignment.h"

#include <algorithm>
#include <cmath>

namespace ma
{

bool alignOn(const AlignSettings& align)
{
    return align.delayMs > 0.05f;
}

void LowAlignMeter::prepare(double sampleRate)
{
    rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    factor = std::max(1, static_cast<int>(std::lround(rate / 2000.0)));
    const double decimated = rate / factor;
    maxLag = std::min(kMaxLag, static_cast<int>(std::floor(kMaxAlignLagMs * decimated / 1000.0)));
    for (auto* filters : { &mainLow, &keyLow })
    {
        (*filters)[0] = Biquad::lowPass(rate, 150.0);
        (*filters)[1] = Biquad::lowPass(rate, 150.0);
    }
    decay = std::exp(-1.0 / (2.0 * decimated)); // about two seconds
    reset();
}

void LowAlignMeter::reset()
{
    for (auto* filters : { &mainLow, &keyLow })
        for (auto& f : *filters)
            f.reset();
    mainHistory.fill(0.0);
    keyHistory.fill(0.0);
    cross.fill(0.0);
    mainEnergy = keyEnergy = seconds = keyShort = 0.0;
    phase = write = 0;
}

void LowAlignMeter::process(const float* main, const float* key, int numSamples)
{
    const double decimated = rate / factor;
    const double shortDecay = std::exp(-1.0 / (0.05 * decimated));
    for (int i = 0; i < numSamples; ++i)
    {
        const double m = mainLow[1].process(mainLow[0].process(main[i]));
        const double k = keyLow[1].process(keyLow[0].process(key[i]));
        if (++phase < factor)
            continue;
        phase = 0;
        mainHistory[static_cast<size_t>(write)] = m;
        keyHistory[static_cast<size_t>(write)] = k;
        keyShort = shortDecay * keyShort + (1.0 - shortDecay) * k * k;
        // Only while the kick plays (about -50 dBFS below 150 Hz): elsewhere the bass alone says nothing.
        if (keyShort > 1e-5)
        {
            // The main signal now against the key `lag` decimated samples ago (lag > 0: main later).
            const int centre = (write - maxLag + kHistory) % kHistory; // main delayed by maxLag, so negative lags fit too
            const double mc = mainHistory[static_cast<size_t>(centre)];
            for (int lag = -maxLag; lag <= maxLag; ++lag)
            {
                const double kc = keyHistory[static_cast<size_t>((centre - lag + kHistory) % kHistory)];
                auto& c = cross[static_cast<size_t>(lag + kMaxLag)];
                c = decay * c + mc * kc;
            }
            const double kz = keyHistory[static_cast<size_t>(centre)];
            mainEnergy = decay * mainEnergy + mc * mc;
            keyEnergy = decay * keyEnergy + kz * kz;
            seconds += 1.0 / decimated;
        }
        write = (write + 1) % kHistory;
    }
}

double LowAlignMeter::bestLagMs() const
{
    int best = 0;
    for (int lag = -maxLag; lag <= maxLag; ++lag)
        if (cross[static_cast<size_t>(lag + kMaxLag)] > cross[static_cast<size_t>(best + kMaxLag)])
            best = lag;
    // Parabolic interpolation between the neighbours for a finer lag than one decimated sample.
    double offset = 0.0;
    if (best > -maxLag && best < maxLag)
    {
        const double a = cross[static_cast<size_t>(best - 1 + kMaxLag)], b = cross[static_cast<size_t>(best + kMaxLag)],
                     c = cross[static_cast<size_t>(best + 1 + kMaxLag)];
        const double d = a - 2.0 * b + c;
        if (d < 0.0)
            offset = std::clamp(0.5 * (a - c) / d, -0.5, 0.5);
    }
    return (best + offset) * factor / rate * 1000.0;
}

double LowAlignMeter::correlationAtBest() const
{
    double best = -1e300;
    for (int lag = -maxLag; lag <= maxLag; ++lag)
        best = std::max(best, cross[static_cast<size_t>(lag + kMaxLag)]);
    const double norm = std::sqrt(mainEnergy * keyEnergy);
    return norm > 0.0 ? best / norm : 0.0;
}

double LowAlignMeter::correlationAtZero() const
{
    const double norm = std::sqrt(mainEnergy * keyEnergy);
    return norm > 0.0 ? cross[static_cast<size_t>(kMaxLag)] / norm : 0.0;
}

void AlignDelay::prepare(double sampleRate, int maxChannels)
{
    rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    const auto length = static_cast<size_t>(std::ceil(kMaxAlignDelayMs * rate / 1000.0)) + 2;
    lines.assign(static_cast<size_t>(std::max(1, maxChannels)), std::vector<float>(length, 0.0f));
    reset();
}

void AlignDelay::reset()
{
    for (auto& line : lines)
        std::fill(line.begin(), line.end(), 0.0f);
    pos = 0;
}

void AlignDelay::setSettings(const AlignSettings& settings)
{
    const int samples = alignOn(settings) ? static_cast<int>(std::lround(std::min(settings.delayMs, kMaxAlignDelayMs) * rate / 1000.0)) : 0;
    if (samples != delaySamples)
    {
        delaySamples = samples;
        reset(); // a jump, only on apply: the line starts silent
    }
}

void AlignDelay::process(float* const* channels, int numChannels, int numSamples)
{
    if (delaySamples <= 0 || lines.empty())
        return;
    const int length = static_cast<int>(lines[0].size());
    const int n = std::min(numChannels, static_cast<int>(lines.size()));
    int p = pos;
    for (int i = 0; i < numSamples; ++i)
    {
        const int read = (p - delaySamples + length) % length;
        for (int c = 0; c < n; ++c)
        {
            auto& line = lines[static_cast<size_t>(c)];
            line[static_cast<size_t>(p)] = channels[c][i];
            channels[c][i] = line[static_cast<size_t>(read)];
        }
        p = (p + 1) % length;
    }
    pos = p;
}

} // namespace ma
