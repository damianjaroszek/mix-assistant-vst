#pragma once

#include "Biquad.h"
#include "SpectrumAnalyzer.h"

#include <array>

namespace ma
{

// Attack emphasis: on the onsets of a sound (a kick's click, a snare's crack) the content
// above `hz` is lifted by up to `amountDb`; the sustained part passes untouched. It makes a
// kick read on a phone without making it louder overall the way a static boost would.
struct TransientSettings
{
    float on = 0.0f;
    float hz = 2000.0f;
    float amountDb = 4.0f;
};

// Allocation free; process() runs on the audio thread.
class TransientEnhancer
{
public:
    static constexpr int kMaxChannels = 2;

    void prepare(double sampleRate);
    void reset();
    void setSettings(const TransientSettings& settings);
    void process(float* const* channels, int numChannels, int numSamples);

    double lastBoostDb() const { return peakBoostDb; } // deepest lift in the last process()

private:
    void updateFilter();

    double sampleRate = 48000.0;
    TransientSettings settings {};
    std::array<Biquad, kMaxChannels> highPass {};
    double fast = 0.0, slow = 0.0;
    double fastAttack = 0.0, fastRelease = 0.0, slowAttack = 0.0, slowRelease = 0.0;
    double peakBoostDb = 0.0;
};

// Rough effect on an averaged spectrum: the bands above hz rise by the lift weighted by the
// share of the channel's energy that sits in onsets (about half for a kick, little for a pad).
SpectrumAnalyzer::Bands predictTransients(const SpectrumAnalyzer::Bands& bandsDb, const TransientSettings& settings,
                                          double onsetShare);

} // namespace ma
