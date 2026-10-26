#pragma once

#include "Biquad.h"
#include "SpectrumAnalyzer.h"

#include <array>
#include <vector>

namespace ma
{

// Playback systems a mix has to translate to, as rough frequency responses.
// Not measurements of a particular device: typical shapes (a phone has nothing under
// ~300 Hz, a car adds cabin gain in the lows, a club adds sub and plays mono).
enum class PlaybackSystem
{
    fullRange = 0,
    phone,
    laptop,
    earbuds,
    car,
    club
};
constexpr int kPlaybackSystems = 6;

// Response of a system at `hz`, in dB.
double systemResponseDb(PlaybackSystem system, double hz);

// The same response as filters, for listening through a simulated system. Allocation free.
class SystemSimulator
{
public:
    static constexpr int kMaxChannels = 2;
    static constexpr int kSections = 4;

    void prepare(double sampleRate);
    void setSystem(PlaybackSystem system); // resets the filter state when it changes
    PlaybackSystem system() const { return current; }
    void process(float* const* channels, int numChannels, int numSamples);

private:
    double sampleRate = 48000.0;
    PlaybackSystem current = PlaybackSystem::fullRange;
    std::array<std::array<Biquad, kSections>, kMaxChannels> filters {};
};

// How each channel's share of the mix moves on every system compared with full range.
// -10 dB on the phone: the channel sounds 10 dB quieter against the rest of the mix there,
// i.e. it mostly disappears. Levels are ear-weighted (ISO 226) before summing.
struct TranslationChannel
{
    SpectrumAnalyzer::Bands rawDb {}; // ERB bands, dBFS, as the mix hears the channel
    bool valid = false;
};

struct TranslationResult
{
    std::vector<std::array<double, kPlaybackSystems>> shareChangeDb; // per channel, 0 for full range
    std::array<double, kPlaybackSystems> mixLevelDb {};               // ear-weighted level of the whole mix
    bool valid = false;
};

// Ear-weighted level of an ERB spectrum played on a system.
double perceivedLevelDb(const SpectrumAnalyzer::Bands& rawDb, PlaybackSystem system);
TranslationResult analyseTranslation(const std::vector<TranslationChannel>& channels);

} // namespace ma
