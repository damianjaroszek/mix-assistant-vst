#include "ma/GainStaging.h"

#include "ma/LevelStats.h"

#include <cmath>

namespace ma
{

TrimPlan planTrim(double measuredDb, double truePeakDb, double targetDb, double peakCeilingDb, double maxTrimDb)
{
    TrimPlan plan;
    if (!std::isfinite(measuredDb) || measuredDb < -90.0)
        return plan;

    plan.valid = true;
    plan.trimDb = suggestTrimDb(measuredDb, targetDb, maxTrimDb);

    if (std::isfinite(truePeakDb) && truePeakDb + plan.trimDb > peakCeilingDb)
    {
        plan.trimDb = peakCeilingDb - truePeakDb;
        plan.limitedByPeak = true;
    }
    return plan;
}

} // namespace ma
