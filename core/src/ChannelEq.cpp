#include "ma/ChannelEq.h"

#include <algorithm>
#include <cmath>

namespace ma
{

double eqResponseDb(const EqSettings& eq, double hz)
{
    double db = 0.0;
    for (const auto& band : eq)
        if (isOn(band))
            db += Biquad::peaking(48000.0, band.hz, band.gainDb, band.q).magnitudeDb(hz, 48000.0);
    return db;
}

void ChannelEq::prepare(double newSampleRate)
{
    sampleRate = newSampleRate;
    initialised = false;
    setBands(current);
    reset();
}

void ChannelEq::reset()
{
    for (auto& channel : filters)
        for (auto& f : channel)
            f.reset();
}

void ChannelEq::setBands(const EqSettings& settings)
{
    bool changed = !initialised;
    for (int b = 0; b < kEqBands; ++b)
        changed = changed || !sameBand(settings[static_cast<size_t>(b)], current[static_cast<size_t>(b)]);
    if (!changed)
        return;

    current = settings;
    initialised = true;
    active = false;
    for (int b = 0; b < kEqBands; ++b)
    {
        const auto& band = current[static_cast<size_t>(b)];
        const double nyquistSafe = std::min(static_cast<double>(band.hz), 0.45 * sampleRate);
        const auto f = isOn(band) ? Biquad::peaking(sampleRate, nyquistSafe, band.gainDb, band.q) : Biquad {};
        active = active || isOn(band);
        for (auto& channel : filters)
        {
            // Keep the filter state so a change does not click.
            auto& target = channel[static_cast<size_t>(b)];
            target.b0 = f.b0;
            target.b1 = f.b1;
            target.b2 = f.b2;
            target.a1 = f.a1;
            target.a2 = f.a2;
        }
    }
}

void ChannelEq::process(float* const* channels, int numChannels, int numSamples)
{
    if (!active)
        return;
    for (int c = 0; c < std::min(numChannels, kMaxChannels); ++c)
    {
        auto* data = channels[c];
        auto& chain = filters[static_cast<size_t>(c)];
        for (int i = 0; i < numSamples; ++i)
        {
            double x = data[i];
            for (int b = 0; b < kEqBands; ++b)
                if (isOn(current[static_cast<size_t>(b)]))
                    x = chain[static_cast<size_t>(b)].process(x);
            data[i] = static_cast<float>(x);
        }
    }
}

} // namespace ma
