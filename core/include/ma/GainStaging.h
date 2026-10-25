#pragma once

namespace ma
{

struct TrimPlan
{
    double trimDb = 0.0;
    bool valid = false;         // something was measured
    bool limitedByPeak = false; // the target would have pushed the true peak over the ceiling
};

// Gain-staging trim: brings `measuredDb` (RMS or LUFS) to `targetDb`, but never lets
// the measured true peak plus the trim exceed `peakCeilingDb`, and stays within +/- maxTrimDb.
TrimPlan planTrim(double measuredDb, double truePeakDb, double targetDb, double peakCeilingDb, double maxTrimDb = 24.0);

} // namespace ma
