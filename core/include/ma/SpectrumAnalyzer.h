#pragma once

#include <array>
#include <complex>
#include <vector>

namespace ma
{

// Band layouts the analyser can sum FFT bins into.
enum class BandScale
{
    thirdOctave, // 31 bands, 20 Hz to 20 kHz, ISO 266 centres
    erb          // 40 bands one ERB wide (Glasberg & Moore), 20 Hz to 20 kHz: spacing as the ear resolves it
};

// Spectrum analyser on a fixed band layout. Mono sum of all channels, Hann window,
// 50 % overlap. Band levels are mean-square levels in dB, so the bands add up to the
// signal's RMS level: a 1 kHz sine with amplitude 1.0 reads -3 dB in its band.
class SpectrumAnalyzer
{
public:
    static constexpr int kMaxBands = 40;
    using Bands = std::array<double, kMaxBands>;

    static int numBandsFor(BandScale scale);
    static double bandCentreHz(BandScale scale, int band);
    // Lower edge of a band; edge numBands is the upper edge of the last band.
    static double bandEdgeHz(BandScale scale, int edge);

    // Kept for the third-octave layout, which the Brain and the lab report on.
    static constexpr int kNumBands = 31;
    static double bandCentreHz(int band) { return bandCentreHz(BandScale::thirdOctave, band); }

    void prepare(double sampleRate, BandScale scale = BandScale::thirdOctave, int fftOrder = 12);
    void reset();
    void process(const float* const* channels, int numChannels, int numSamples);

    BandScale scale() const { return bandScale; }
    int numBands() const { return bandCount; }
    double centreHz(int band) const { return bandCentreHz(bandScale, band); }

    // Average over everything since reset (used for learning a whole passage).
    // Entries from numBands() on are unused.
    Bands averageDb() const;
    // Exponentially smoothed, roughly the last second (used for live display).
    Bands recentDb() const;

    int framesAnalysed() const { return frames; }
    // Mean-square power per band of the newest frame (linear, not dB).
    const Bands& lastFramePower() const { return frame; }

private:
    void analyseFrame();
    void fft(std::vector<std::complex<double>>& data) const;

    BandScale bandScale = BandScale::thirdOctave;
    int bandCount = kNumBands;
    double sampleRate = 48000.0;
    int fftSize = 4096;
    int hopSize = 2048;
    double recentSmoothing = 0.9;

    std::vector<double> window;
    std::vector<double> input;
    int inputFill = 0;
    std::vector<std::complex<double>> buffer;
    std::vector<int> binToBand;
    std::vector<int> bitReversed;
    std::vector<std::complex<double>> twiddles; // e^(-2 pi i k / fftSize), k < fftSize / 2
    double powerScale = 1.0;

    Bands frame {};
    Bands sum {};
    Bands recent {};
    int frames = 0;
};

} // namespace ma
