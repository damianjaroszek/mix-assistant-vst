#pragma once

#include "Biquad.h"
#include "Instrument.h"
#include "SpectrumAnalyzer.h"

#include <array>
#include <vector>

namespace ma
{

// Phase 33: where a channel sits between the speakers. The Agent pans its own output (the last stage
// of its chain) so the Brain can place every part in one plan; FL's mixer pan stays the user's.
// Below `monoBelowHz` the signal is left where it is (an LR4 split, so the two halves sum flat), so
// a part's low end stays in the centre with the kick and the bass.
struct PanSettings
{
    float position = 0.0f;      // -1 left .. 0 centre .. +1 right
    float monoBelowHz = 150.0f; // <= 0: the whole signal moves
};

bool panOn(const PanSettings& pan);
// Constant power, unity in the centre: a mono part keeps its loudness in stereo wherever it goes
// (L^2 + R^2 the same), its mono sum drops a little off centre (-0.7 dB at 0.5, -3 dB at the side).
void panGains(double position, double& left, double& right);
// What a pan does to the mono sum of a mono part, dB (<= 0).
double panMonoLossDb(double position);

class Panner
{
public:
    void prepare(double sampleRate);
    void reset();
    void setSettings(const PanSettings& settings);
    void process(float* const* channels, int numChannels, int numSamples);

private:
    void updateFilters();
    double sampleRate = 48000.0;
    PanSettings settings {};
    double filtersHz = -1.0;
    std::array<std::array<Biquad, 2>, 2> low {}, high {}; // [channel][stage], LR4
    double leftNow = 1.0, rightNow = 1.0;                // ramped per sample
    double smoothing = 0.001;
    bool running = false;
};

// The input's left/right balance (left power over right, dB) above the low end, roughly the last
// two seconds: a part that arrives panned (a panned sample, a hard-left guitar) is the producer's.
class BalanceMeter
{
public:
    void prepare(double sampleRate);
    void reset();
    void process(const float* left, const float* right, int numSamples);
    double balanceDb() const;
    bool valid() const { return seen > 0.5; }

private:
    std::array<Biquad, 2> highPass {};
    double leftPower = 0.0, rightPower = 0.0, coefficient = 0.0, seen = 0.0, sampleRate = 48000.0;
};

// ----- planning -----

struct PanChannel
{
    Role role = Role::other;
    int priority = 5;
    bool valid = false;
    SpectrumAnalyzer::Bands midDb {}, sideDb {}; // ERB, after the trim (where the Brain hears it)
    double inputBalanceDb = 0.0;  // the input's own left/right balance
    bool balanceKnown = false;
    PanSettings current {};
    bool userPan = false; // the Agent's pan was set by hand: it stays and counts as it is
};

struct PanPlan
{
    enum class Reason
    {
        none,       // not planned (no measurement)
        centreRole, // kick, bass, the main vocal and lead, buses and sends: the centre holds the mix
        mainPart,   // the loudest vocal or lead stays in the centre, the rest of them may move
        user,       // the user panned it in the Agent
        prePanned,  // it arrives panned (its input leans by >= kPrePannedDb)
        wide,       // already wide: panning a wide part only tilts it
        quiet,      // too quiet in the mix for its place to matter
        paired,     // opposite another part that covers the same range
        balance,    // on the side that keeps the mix balanced
        alone       // nothing to balance it: it stays in the centre
    };
    bool considered = false;
    PanSettings settings {};
    Reason reason = Reason::none;
    int partner = -1;     // with Reason::paired
    double overlap = 0.0; // how much of their spectra the pair shares (0..1)
    bool limited = false; // pulled towards the centre to keep the mix balanced
};

struct PanResult
{
    std::vector<PanPlan> plans;
    // Left minus right of the mix above the low end, dB: as it plays now (current pans) and planned.
    double balanceNowDb = 0.0, balancePlannedDb = 0.0;
    double worstRegionNowDb = 0.0, worstRegionPlannedDb = 0.0; // the most lopsided of 150-2k, 2-9k, 9-16k Hz
};

constexpr double kPrePannedDb = 3.0;
constexpr double kMaxMixBalanceDb = 0.5;
constexpr double kMaxRegionBalanceDb = 1.5;
constexpr double kPairOverlap = 0.35;

// How far each role goes when it moves (before the width and balance scale it down).
double panAmountForRole(Role role);

// Phase 33: spread the parts that can move so that parts covering the same range sit on opposite
// sides (they stop masking each other in stereo) and the mix stays balanced left to right, region by
// region. Kick, bass, buses, sends and the main vocal and lead stay in the centre; a part the user
// panned or that arrives panned is counted where it is. Works from pre-pan spectra, so planning again
// gives the same places.
PanResult planPan(const std::vector<PanChannel>& channels);

} // namespace ma
