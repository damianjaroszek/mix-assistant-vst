#include "ma/LevelStats.h"

#include "ma/Decibels.h"

#include <algorithm>
#include <cmath>

namespace ma
{

LevelStats::LevelStats(double gateDb)
    : gateMeanSquare(std::pow(10.0, gateDb / 10.0))
{
}

void LevelStats::reset()
{
    sumSquares = 0.0;
    samples = 0;
    peak = 0.0;
    lastBlockMeanSquare = 0.0;
}

void LevelStats::process(const float* const* channels, int numChannels, int numSamples)
{
    if (numChannels <= 0 || numSamples <= 0)
        return;

    double blockSum = 0.0;
    for (int c = 0; c < numChannels; ++c)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const double x = channels[c][i];
            blockSum += x * x;
            peak = std::max(peak, std::abs(x));
        }
    }

    // Mean square per sample frame, averaged over channels.
    lastBlockMeanSquare = blockSum / (static_cast<double>(numSamples) * numChannels);
    if (lastBlockMeanSquare < gateMeanSquare)
        return;

    sumSquares += lastBlockMeanSquare * numSamples;
    samples += numSamples;
}

double LevelStats::rmsDb() const
{
    return samples > 0 ? powerToDb(sumSquares / static_cast<double>(samples)) : kMinusInfinityDb;
}

double LevelStats::peakDb() const
{
    return gainToDb(peak);
}

double LevelStats::crestDb() const
{
    return samples > 0 ? peakDb() - rmsDb() : 0.0;
}

double LevelStats::lastBlockRmsDb() const
{
    return powerToDb(lastBlockMeanSquare);
}

double suggestTrimDb(double measuredRmsDb, double targetRmsDb, double maxTrimDb)
{
    if (!std::isfinite(measuredRmsDb) || measuredRmsDb < -90.0)
        return 0.0;
    return std::clamp(targetRmsDb - measuredRmsDb, -maxTrimDb, maxTrimDb);
}

} // namespace ma
