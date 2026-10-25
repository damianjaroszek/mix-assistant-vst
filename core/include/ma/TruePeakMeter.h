#pragma once

#include <array>
#include <vector>

namespace ma
{

// True-peak meter after ITU-R BS.1770-4 Annex 2: 4x polyphase oversampling,
// then absolute maximum. Also tracks the plain sample peak.
class TruePeakMeter
{
public:
    TruePeakMeter();

    void prepare(int numChannels);
    void reset();
    void process(const float* const* channels, int numChannels, int numSamples);

    double truePeakDb() const;   // dBTP, maximum since reset
    double samplePeakDb() const; // dBFS, maximum since reset

private:
    static constexpr int kPhases = 4;
    static constexpr int kTapsPerPhase = 12;

    std::array<std::array<double, kTapsPerPhase>, kPhases> coefficients {};
    std::vector<std::array<double, kTapsPerPhase>> history;
    std::vector<int> historyPos;
    double maxTruePeak = 0.0;
    double maxSamplePeak = 0.0;
};

} // namespace ma
