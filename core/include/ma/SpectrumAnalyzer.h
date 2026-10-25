#pragma once

#include <array>
#include <complex>
#include <vector>

namespace ma
{

// Third-octave spectrum analyser (31 bands, 20 Hz to 20 kHz, ISO 266 centres).
// Mono sum of all channels, Hann window, 50 % overlap.
// Band levels are mean-square levels in dB, so the bands add up to the
// signal's RMS level: a 1 kHz sine with amplitude 1.0 reads -3 dB in its band.
class SpectrumAnalyzer
{
public:
    static constexpr int kNumBands = 31;
    using Bands = std::array<double, kNumBands>;

    static double bandCentreHz(int band);

    void prepare(double sampleRate, int fftOrder = 12);
    void reset();
    void process(const float* const* channels, int numChannels, int numSamples);

    // Average over everything since reset (used for learning a whole passage).
    Bands averageDb() const;
    // Exponentially smoothed, roughly the last second (used for live display).
    Bands recentDb() const;

    int framesAnalysed() const { return frames; }

private:
    void analyseFrame();
    void fft(std::vector<std::complex<double>>& data) const;

    double sampleRate = 48000.0;
    int fftSize = 4096;
    int hopSize = 2048;
    double recentSmoothing = 0.9;

    std::vector<double> window;
    std::vector<double> input;
    int inputFill = 0;
    std::vector<std::complex<double>> buffer;
    std::vector<int> binToBand;
    double powerScale = 1.0;

    Bands frame {};
    Bands sum {};
    Bands recent {};
    int frames = 0;
};

} // namespace ma
