#pragma once

#include "ChannelChain.h"
#include "DynamicEq.h"
#include "SpectrumAnalyzer.h"

#include <array>

namespace ma
{

// Channel roles, in the order of the Agent's "role" parameter (the index travels on the bus).
enum class Role
{
    other = 0,
    kick,
    bass,
    percussion,
    vocal,
    lead,
    pad,
    effects,
    bus,
    send,
    clap // the backbeat (clap, snare): added last, so saved role indices keep their meaning
};
constexpr int kRoles = 11;

// A drum hit other than the kick: hats, shakers, toms (percussion) and the backbeat.
constexpr bool isPercussion(Role role) { return role == Role::percussion || role == Role::clap; }

// What the role recogniser listens for besides the spectrum: how the level moves in time.
// Fed while the Agent learns a passage; cheap, allocation free after prepare().
class InstrumentFeatures
{
public:
    void prepare(double sampleRate);
    void reset();
    void process(const float* const* channels, int numChannels, int numSamples);
    // One analyser frame (linear band powers), each time the learning analyser makes one:
    // how much the spectrum's shape moves tells a riff from a held chord.
    void addSpectrumFrame(const SpectrumAnalyzer::Bands& power, int numBands);

    struct Summary
    {
        double seconds = 0.0;        // audible time measured
        double onsetsPerSecond = 0.0;
        double decayDb = 0.0;        // average drop 150 ms after an onset: large for drums, small for pads
        double activeShare = 0.0;    // share of the passage the channel plays at all (vocals pause)
        double width = 0.0;          // side energy over mid energy
        double movement = 0.0;       // average change of spectral shape between frames, 0..1
        // Dynamics for the mix engineer: 100 ms RMS levels while playing (dBFS, as fed in).
        double levelP10 = -200.0, levelP50 = -200.0, levelP95 = -200.0;
        double lowNoteHz = 0.0;      // the bass line's usual note (median), 0 = no clear low notes
        // Phase 14: the pitch a hit's low tail settles on (150-300 ms after onsets, 30-130 Hz,
        // median in 10-cent steps): a kick's tuning. 0 = no hits with a pitched tail.
        double tailPitchHz = 0.0;
        // Phase 32: each probe range (DynamicProbe order) against the whole part, 20 ms at a time
        // while it plays (dB, band minus mid): the usual share and the moments it sticks out.
        std::array<double, kDynamicBands> probeP50 {}, probeP95 {};
        bool probesValid = false;
        bool valid = false;
    };
    Summary summary() const;

private:
    void endHop();

    double sampleRate = 48000.0;
    int hopLength = 240; // 5 ms
    int hopFill = 0;
    double hopMid = 0.0, hopSide = 0.0;
    std::array<double, 4> lastHops {}; // powers of the last four hops: levels are read over 20 ms
    int hopIndex = 0;
    double slowPower = 0.0, slowCoeff = 0.0;
    double recentPeakDb = -200.0; // loudest hop in the last few seconds, decays slowly
    long long hops = 0, activeHops = 0, onsets = 0, decays = 0;
    int sinceOnset = 1 << 20; // hops
    double onsetPeakDb = -200.0, onsetLowDb = 200.0, decaySum = 0.0;
    double midSum = 0.0, sideSum = 0.0;
    SpectrumAnalyzer::Bands previousShape {};
    bool havePrevious = false;
    double movementSum = 0.0;
    long long movementFrames = 0;
    std::array<double, 20> recentHops {}; // last 100 ms of hop powers
    int recentIndex = 0;
    std::array<long long, 240> levelHistogram {}; // 0.5 dB steps from -120 dBFS
    LowPitchTracker pitchTracker;
    std::array<long long, 161> noteHistogram {}; // hops per detected note, 1 Hz steps
    long long noteHops = 0;
    std::array<long long, 253> tailHistogram {}; // hops per tail pitch, 10 cents from 30 Hz (to ~130 Hz)
    long long tailHops = 0;
    // Phase 32: probe ranges for the dynamic EQ.
    std::array<Biquad, kDynamicBands> probeFilters {};
    std::array<double, kDynamicBands> hopProbe {};
    std::array<std::array<double, 4>, kDynamicBands> lastProbe {};
    std::array<double, 4> lastMid {};
    std::array<std::array<long long, 160>, kDynamicBands> probeHistogram {}; // 0.5 dB steps from -60 dB
    long long probeHops = 0;
};

struct RoleGuess
{
    Role role = Role::other;
    double confidence = 0.0;  // 0..1; the Agent only takes guesses of 0.5 and up
    Role runnerUp = Role::other;
    double centroidHz = 0.0;  // where the energy sits (log-frequency centre), for display
    std::array<double, kRoles> scores {};
};

// Rule-based guess from the temporal summary and the raw ERB spectrum (dBFS) of the same
// passage. It knows the usual parts of house and trance; buses and sends are never guessed.
RoleGuess recogniseRole(const InstrumentFeatures::Summary& features, const SpectrumAnalyzer::Bands& rawBandsDb);

// Starting priority for a recognised role (1 = most important), as a house mix is usually built.
int defaultPriority(Role role);
// Phase 29: where "Naucz poziom" puts a part against the level target, as a house / tech house
// mix starts out: the kick at the target (its RMS is low for how loud it sounds: short hits,
// mostly sub), the bass just under, the vocal, the lead, the percussion and the pads further
// back (sustained and in the ear's most sensitive range), effects and returns furthest. Unknown
// roles and buses stay at the target. The Brain's group balance refines it ("Analizuj").
// With the part's raw ERB spectrum (dBFS), percussion that lives only up top (hats, shakers,
// rides: most of its power above 5 kHz) goes further back than a clap or a snare: the kick
// and the hats share the drums group, so nothing after "Naucz poziom" sets the two apart.
double roleLevelOffsetDb(Role role, const SpectrumAnalyzer::Bands* rawBandsDb = nullptr);
// Phase 32: the share of a raw ERB spectrum's power above `hz`.
double powerShareAbove(const SpectrumAnalyzer::Bands& rawBandsDb, double hz);

} // namespace ma
