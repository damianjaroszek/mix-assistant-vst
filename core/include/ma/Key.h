#pragma once

#include "Biquad.h"
#include "SpectrumAnalyzer.h"

#include <array>
#include <complex>
#include <string>
#include <vector>

namespace ma
{

// Phase 10: the musical key, so reference profiles can be moved to the key of the track
// (the profile is shifted, never the audio). A reference in A minor has its bass a few
// semitones away from a track in F minor; compared as they are, the sub and bass regions
// would disagree for no reason.
struct MusicalKey
{
    int tonic = -1;     // pitch class, 0 = C; -1 = unknown
    bool minor = false;

    bool valid() const { return tonic >= 0 && tonic < 12; }
    bool operator==(const MusicalKey& other) const { return tonic == other.tonic && minor == other.minor; }
    bool operator!=(const MusicalKey& other) const { return !(*this == other); }
};

// Polish names, ASCII only: "C-dur", "fis-moll", "B-dur" (B flat), "h-moll".
std::string keyName(MusicalKey key);
// Camelot wheel ("8A" = A minor, "8B" = C major), the DJ notation.
std::string camelotCode(MusicalKey key);
int camelotNumber(MusicalKey key); // 1..12, 0 when unknown
MusicalKey keyFromCamelot(int number, bool minor);
// 0..23 (tonic, + 12 for minor) for saving; -1 = unknown.
int keyIndex(MusicalKey key);
MusicalKey keyFromIndex(int index);

// Semitones to move a profile measured in `from` so it sits in `to`: the shortest way,
// -5..+6. Relative keys (A minor, C major) share their notes and give 0.
int keyShiftSemitones(MusicalKey from, MusicalKey to);

struct KeyEstimate
{
    MusicalKey key;
    double correlation = 0.0; // with the key's profile, -1..1
    double margin = 0.0;      // over the best key with other notes (another Camelot number)
    bool confident = false;
};
// From a pitch class profile (C, C#, ... B), any scale; all zero gives an unknown key. A key and
// its relative (A minor, C major) share their notes; when they score alike, the bass decides
// (`bass`: the profile below 160 Hz, may be all zero), and with no clear answer the minor wins,
// as it mostly does in house and trance.
KeyEstimate estimateKey(const std::array<double, 12>& chroma, const std::array<double, 12>& bass = {});

// Pitch class profile of what it hears: downsampled to about 11 kHz, 8192-point FFT (1.4 Hz
// bins, enough for bass notes), spectral peaks only (drums and noise add little), each peak
// to its nearest semitone. Allocates in prepare() only.
class KeyDetector
{
public:
    void prepare(double sampleRate);
    void reset();
    void process(const float* const* channels, int numChannels, int numSamples);

    // Accumulated since reset; each frame weighs by its level, quiet frames not at all.
    const std::array<double, 12>& chroma() const { return sum; }
    const std::array<double, 12>& bassChroma() const { return bassSum; }
    KeyEstimate estimate() const { return estimateKey(sum, bassSum); }
    double secondsHeard() const { return heardSeconds; }

private:
    void analyseFrame();

    int factor = 4;
    double rate = 12000.0; // after downsampling
    int phase = 0;
    std::array<Biquad, 2> antiAlias {};
    std::vector<double> window, input, power;
    std::vector<std::complex<double>> buffer, twiddles;
    std::vector<int> bitReversed;
    int fill = 0;
    double frameSeconds = 0.0;
    std::array<double, 12> sum {}, bassSum {};
    double heardSeconds = 0.0;
};

// An ERB spectrum (band powers in dB, as SpectrumAnalyzer reads them) moved by a number of
// semitones, the power in each band taken from the range that lands there.
SpectrumAnalyzer::Bands shiftErbSpectrum(const SpectrumAnalyzer::Bands& db, int semitones);

} // namespace ma
