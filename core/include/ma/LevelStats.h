#pragma once

namespace ma
{

// RMS, peak and crest factor over a passage. Blocks quieter than the gate
// are skipped, so silence between notes does not drag the RMS down.
class LevelStats
{
public:
    explicit LevelStats(double gateDb = -60.0);

    void reset();
    void process(const float* const* channels, int numChannels, int numSamples);

    double rmsDb() const;       // gated RMS over everything since reset
    double peakDb() const;      // sample peak since reset
    double crestDb() const;     // peak minus RMS
    double lastBlockRmsDb() const;
    long long gatedSamples() const { return samples; }

private:
    double gateMeanSquare;
    double sumSquares = 0.0;
    long long samples = 0;
    double peak = 0.0;
    double lastBlockMeanSquare = 0.0;
};

// Trim that brings a measured RMS level to the target, limited to +/- maxTrimDb.
// Returns 0 when nothing meaningful was measured.
double suggestTrimDb(double measuredRmsDb, double targetRmsDb, double maxTrimDb = 24.0);

} // namespace ma
