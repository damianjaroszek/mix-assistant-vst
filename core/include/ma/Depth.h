#pragma once

#include "Biquad.h"
#include "SpectrumAnalyzer.h"

#include <array>
#include <vector>

namespace ma
{

// Phase 34: depth. A reverb or delay return sits behind the dry parts when it is darker than them
// and gets out of the way while the main voice sings. The high cut is the send's own (after its
// cleanup); the give-way under the voice is the Agent's duck keyed over the bus (ma/Unmask.h).
struct DepthSettings
{
    float highCutHz = 0.0f; // 12 dB/oct low-pass; <= 0 = off
};

bool depthOn(const DepthSettings& depth);
// What the high cut does at `hz`, dB (<= 0).
double depthResponseDb(const DepthSettings& depth, double hz);

class HighCut
{
public:
    void prepare(double sampleRate);
    void reset();
    void setSettings(const DepthSettings& settings);
    void process(float* const* channels, int numChannels, int numSamples);

private:
    double sampleRate = 48000.0;
    double filterHz = -1.0;
    std::array<Biquad, 2> lowPass {};
    DepthSettings settings {};
};

// Times that sit on the groove, for the user's reverb and delay (ms, at `bpm`).
struct DepthTimes
{
    bool valid = false;
    double preDelayMs = 0.0;   // a 1/64: the dry hit comes first, the room after it
    double decaySeconds = 0.0; // half a bar: the tail is gone before the next phrase starts
    double eighthMs = 0.0, dottedEighthMs = 0.0, quarterMs = 0.0;
};
DepthTimes depthTimes(double bpm);

// Brightness as the depth step reads it: level 8-16 kHz minus level 1-4 kHz (power sums, dB).
double topTiltDb(const SpectrumAnalyzer::Bands& erbDb);

constexpr double kSendHighCutHz = 9000.0;
constexpr double kDarkerThanMixDb = 3.0; // a send at least this much darker than the mix already sits behind
constexpr float kSendDuckDb = 3.0f;      // under the main voice when the levels are not known yet

// Phase 36: how a send gives way to the main voice. Only in the voice's own range (a bell over where it
// is loudest, 800 Hz-5 kHz), so the room keeps its width and low end while the words clear, and as deep
// as it takes to put the tail `kSendUnderVoiceDb` under the voice there (from the drop's spectra, both
// after their trims, before the duck), between kSendDuckMinDb (the room still breathes with the phrases)
// and kSendDuckMaxDb.
struct SendDuck
{
    bool valid = false;  // both spectra known and the voice plays in its range
    double hz = 2500.0;  // bell centre
    double q = 0.8;      // about two octaves
    double depthDb = kSendDuckDb;
    double tailVsVoiceDb = 0.0; // the send against the voice, 1-5 kHz, before the duck
};
constexpr double kSendUnderVoiceDb = 12.0;
constexpr double kSendDuckMinDb = 2.0;
constexpr double kSendDuckMaxDb = 8.0;
constexpr double kSendDuckSourceMaxDb = 6.0; // phase 37: under its own source (drums, a lead)
SendDuck planSendDuck(const SpectrumAnalyzer::Bands& sendDb, const SpectrumAnalyzer::Bands& voiceDb);
// Phase 37: the same for a send giving way to its own source: the depth over `fromHz`-`toHz`, the bell
// over the key's loudest band between `peakFromHz` and `peakToHz`, the depth clamped to `maxDb`.
SendDuck planSendDuck(const SpectrumAnalyzer::Bands& sendDb, const SpectrumAnalyzer::Bands& keyDb, double fromHz, double toHz,
                      double peakFromHz, double peakToHz, double maxDb);

// Phase 37: which channels feed a send. FL tells a plugin nothing about the mixer's routing, but a
// send's level follows its sources over the song (the vocal reverb fades when the vocal stops) and its
// spectrum is theirs, smeared. Per second of the song and per broad region, the send's power is fitted
// as a non-negative sum of the candidates' (this second and a little of the last one, for the tail).
constexpr int kSourceRegions = 8;          // log-spaced, 150 Hz-12 kHz
using SourceFrame = std::array<double, kSourceRegions>;
SourceFrame sourceFrame(const SpectrumAnalyzer::Bands& powerLinear); // ERB band powers (linear) into regions
struct SendSources
{
    bool valid = false;         // enough seconds and the fit explains the send
    std::vector<double> share;  // per candidate: the part of the send's power it explains (0..1)
    double fit = 0.0;           // 1 - residual / sum of squares, on region-normalised power
    int seconds = 0;
    int dominant = -1;          // the candidate with the largest share
};
constexpr int kMinSourceSeconds = 12;
constexpr double kMinSourceFit = 0.7;
// `send[t]` and `candidates[c][t]` the same seconds (a second the send is silent in is skipped).
SendSources findSendSources(const std::vector<SourceFrame>& send, const std::vector<std::vector<SourceFrame>>& candidates);

} // namespace ma
