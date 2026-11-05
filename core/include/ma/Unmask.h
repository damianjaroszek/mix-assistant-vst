#pragma once

#include "Biquad.h"

#include <array>
#include <cstdint>

// Phase 30: dynamic unmasking without a sidechain cable.
//
// A channel that covers another (the bass over a lead, a pad over the vocal) dips a bell in the
// covered range only while the other one plays, like a sidechained dynamic EQ. The key does not
// come in as audio: the covered channel's Agent measures its own output in octave bands
// (`KeyFollower`) and publishes the envelope through the shared bus; the covering Agent reads it
// once a block (`Unmasker`). One block of delay is far below the attack, so it needs no routing.
namespace ma
{

constexpr int kKeyBands = 8;      // octave bands, 125 Hz to 16 kHz
constexpr int kMaxUnmask = 2;     // keys a channel can give way to
constexpr float kKeySilentDb = -200.0f;

inline double keyBandCentreHz(int band)
{
    return 125.0 * static_cast<double>(1 << band);
}

// Octave bands whose centre falls in lowHz..highHz (at least the one nearest the middle).
std::int32_t keyBandsFor(double lowHz, double highHz);

struct UnmaskSettings
{
    float on = 0.0f;       // 0 / 1
    float hz = 2000.0f;    // dip centre
    float q = 0.7f;
    float depthDb = 4.0f;  // most it dips, when the key plays at its usual loud level
    float keyBands = 0.0f; // bit mask of the key's octave bands it listens to (exact in a float)
};

inline bool unmaskOn(const UnmaskSettings& s)
{
    return s.on > 0.5f && s.depthDb > 0.05f && static_cast<std::int32_t>(s.keyBands) != 0;
}

// The key side: mean-square envelope per octave band of a channel's output. Allocation free.
class KeyFollower
{
public:
    void prepare(double sampleRate);
    void reset();
    // Mono sum of the channels; levels() then holds the envelope at the end of the block.
    void process(const float* const* channels, int numChannels, int numSamples);
    // Mean-square level per band in dB (the band's RMS), kKeySilentDb when silent.
    const std::array<float, kKeyBands>& levels() const { return levelsDb; }

private:
    double sampleRate = 48000.0;
    std::array<Biquad, kKeyBands> filters {};
    std::array<double, kKeyBands> power {}, meanSquare {};
    std::array<float, kKeyBands> levelsDb {};
    double mean = 0.0, attack = 0.0, release = 0.0;
};

// The key's level in the bands a dip listens to (power sum), kKeySilentDb when none of them plays.
double keyLevelDb(const float* bandDb, std::int32_t bands);

// The covering side: up to kMaxUnmask dips, each following one key. Allocation free.
class Unmasker
{
public:
    static constexpr int kMaxChannels = 2;
    // The dip starts this far under the key's recent loud level and is full kFullUnderDb under it.
    static constexpr double kStartUnderDb = 20.0;
    static constexpr double kFullUnderDb = 6.0;
    static constexpr double kAttackMs = 10.0;
    static constexpr double kReleaseMs = 150.0;
    static constexpr double kReferenceFallDbPerSecond = 1.0;

    void prepare(double sampleRate);
    void reset();
    void setSettings(int index, const UnmaskSettings& settings);
    // `keyDb`: each dip's key level for this block (keyLevelDb), kKeySilentDb when the key is
    // not there. `main` is processed in place.
    void process(float* const* main, int numMain, int numSamples, const double* keyDb);

    double reductionDb(int index) const { return peakReductionDb[static_cast<size_t>(index)]; } // deepest in the last block
    // How far the dip would go for a key level, given the reference (for tests and the editor).
    static double amountFor(double keyDb, double referenceDb);

private:
    double sampleRate = 48000.0;
    std::array<UnmaskSettings, kMaxUnmask> settings {};
    std::array<std::array<Biquad, kMaxChannels>, kMaxUnmask> dip {};
    std::array<double, kMaxUnmask> referenceDb {};
    std::array<double, kMaxUnmask> currentDb {};   // smoothed dip depth now
    std::array<double, kMaxUnmask> peakReductionDb {};
};

} // namespace ma
