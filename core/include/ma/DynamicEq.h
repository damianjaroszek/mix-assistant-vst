#pragma once

#include "Biquad.h"

#include <array>

// Phase 32: problems that come and go. A static cut takes the sibilance band off every word, the
// harsh 2-5 kHz off every note, the proximity boom off the whole vocal, so the part goes dull to
// fix a few moments. A dynamic band cuts only while its range sticks out of the part's own sound
// further than it usually does (relative detection: the band's level minus the whole part's level,
// so a louder phrase does not trigger it). The rider evens a vocal's level phrase to phrase before
// the compressor, so the compressor only has to hold the syllables.
namespace ma
{

constexpr int kDynamicBands = 3;

// The ranges the Learn pass measures, one per band slot: proximity boom, harshness, sibilance.
enum class DynamicProbe : int
{
    boom = 0,
    harsh = 1,
    sibilance = 2
};
double dynamicProbeHz(int probe);
double dynamicProbeQ(int probe);

struct DynamicBand
{
    float on = 0.0f;
    float hz = 6500.0f;
    float q = 1.5f;
    float thresholdDb = 0.0f; // band minus whole part (dB) above which it starts cutting
    float rangeDb = 6.0f;     // most it cuts
    float attackMs = 2.0f;
    float releaseMs = 60.0f;
};

struct RiderSettings
{
    float on = 0.0f;
    float targetDb = -20.0f; // the 100 ms level it rides towards (dBFS at its input)
    float rangeDb = 4.0f;    // most it lifts or lowers
    float speedMs = 300.0f;  // how fast the gain moves
};

struct DynamicEqSettings
{
    std::array<DynamicBand, kDynamicBands> bands {};
    RiderSettings rider {};
};

inline bool dynamicBandOn(const DynamicBand& b)
{
    return b.on > 0.5f && b.rangeDb > 0.05f;
}
inline bool riderOn(const RiderSettings& r)
{
    return r.on > 0.5f && r.rangeDb > 0.05f;
}
inline bool dynamicEqOn(const DynamicEqSettings& s)
{
    bool any = riderOn(s.rider);
    for (const auto& b : s.bands)
        any = any || dynamicBandOn(b);
    return any;
}

// A band slot as the planner and the Agent set it: the probe's centre and width, attack and
// release (fast for esses, slower for harshness and boom), off.
DynamicBand dynamicProbeBand(int probe);

// How much a band cuts at a steady excess over its threshold (dB, >= 0): 3:1 above it, up to range.
double dynamicBandCutDb(const DynamicBand& band, double excessDb);

class DynamicEq
{
public:
    static constexpr int kMaxChannels = 2;

    void prepare(double sampleRate);
    void reset();
    void setSettings(const DynamicEqSettings& settings);
    void process(float* const* channels, int numChannels, int numSamples);
    // Meters: the deepest cut of each band and the rider's gain in the last process().
    double reductionDb(int band) const { return peakCut[static_cast<size_t>(band)]; }
    double riderGainDb() const { return riderGain; }

private:
    void updateFilters();

    double sampleRate = 48000.0;
    DynamicEqSettings settings {};
    std::array<Biquad, kDynamicBands> detector {};
    std::array<std::array<Biquad, kDynamicBands>, kMaxChannels> cut {};
    std::array<double, kDynamicBands> bandPower {}, cutDb {}, peakCut {};
    std::array<double, kDynamicBands> attack {}, release {};
    double wholePower = 0.0, powerCoeff = 0.0;
    double riderPower = 0.0, riderPowerCoeff = 0.0, riderCoeff = 0.0;
    double riderGain = 0.0;
    int sinceUpdate = 0;
    bool detectorsStale = true;
};

} // namespace ma
