#pragma once

#include <cmath>
#include <limits>

namespace ma
{

constexpr double kMinusInfinityDb = -std::numeric_limits<double>::infinity();

inline double gainToDb(double gain)
{
    return gain > 0.0 ? 20.0 * std::log10(gain) : kMinusInfinityDb;
}

inline double powerToDb(double meanSquare)
{
    return meanSquare > 0.0 ? 10.0 * std::log10(meanSquare) : kMinusInfinityDb;
}

inline double dbToGain(double db)
{
    return std::pow(10.0, db / 20.0);
}

} // namespace ma
