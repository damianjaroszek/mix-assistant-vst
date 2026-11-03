#pragma once

#include "SpectrumAnalyzer.h"

#include <complex>
#include <cstdint>
#include <vector>

namespace ma
{

// Phase 21: a sketch's ghost parts made audible. Noise shaped to an ERB spectrum given as
// SpectrumAnalyzer reads one (mean-square dB per band, so the bands add up to the RMS level),
// made by overlap-adding random-phase FFT frames (square-root Hann, half overlap, so the power
// stays even). The spectrum is the mid's (the analysers read (L+R)/2); an independent side
// stream adds the width in the ratio of the side share. Optionally pulsed on the beat (a drum part), its average power kept.
// prepare() allocates; setSpectrum() and process() do not, so they run on the audio thread.
class GhostNoise
{
public:
    void prepare(double sampleRate, int fftOrder = 13);
    void reset();

    // The spectrum to make (dB per ERB band, -200 = none) and its side share (0..1). Takes
    // effect at the next frame.
    void setSpectrum(const SpectrumAnalyzer::Bands& db, double sideShare);
    // Pulsed: a hit on every beat decaying with `decaySeconds`, louder so the average power
    // stays the spectrum's. 0 = steady.
    void setPulse(double decaySeconds) { pulseDecay = decaySeconds; }

    // Adds `numSamples` of the noise times `gain` into the two channels. `beatPhase` is where the
    // first sample sits within a beat (0..1) and `beatSeconds` how long a beat lasts; both are
    // only used when pulsed (a beat of 0 s: steady after all).
    void process(float* left, float* right, int numSamples, float gain, double beatPhase = 0.0, double beatSeconds = 0.0);

private:
    void makeFrame();
    void transform(std::vector<std::complex<double>>& data) const; // inverse FFT, unscaled
    double random(); // uniform 0..1

    double sampleRate = 48000.0;
    int size = 8192, hop = 4096;
    std::vector<double> window;
    std::vector<int> bandOfBin;
    std::vector<int> binsInBand, firstBin;
    std::vector<double> binMagnitude; // per bin, for the current spectrum
    double side = 0.0;
    std::vector<std::complex<double>> spectrumMid, spectrumSide;
    std::vector<std::complex<double>> twiddles;
    std::vector<int> bitReverse;
    // Overlap-add output of the two streams: the frame being read out and the tail of the last.
    std::vector<double> outMid, outSide;
    int readPosition = 0;
    double pulseDecay = 0.0;
    std::uint64_t seed = 0x9e3779b97f4a7c15ull;
};

} // namespace ma
