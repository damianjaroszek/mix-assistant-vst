#include "ma/MasterCheck.h"

#include <algorithm>
#include <cmath>

namespace ma
{

MasterCheck compareMaster(const MasterSide& before, const MasterSide& after)
{
    MasterCheck check;
    if (!before.valid || !after.valid || before.seconds < kMasterMinimumSeconds || after.seconds < kMasterMinimumSeconds
        || before.integratedLufs < -100.0 || after.integratedLufs < -100.0)
        return check;
    // Both sides must have heard about the same stretch, or the difference is the music's, not mastering's.
    if (std::abs(before.seconds - after.seconds) > 0.25 * std::max(before.seconds, after.seconds) + 2.0)
        return check;
    check.valid = true;
    check.gainDb = after.integratedLufs - before.integratedLufs;
    check.plrBeforeDb = before.truePeakDb - before.integratedLufs;
    check.plrAfterDb = after.truePeakDb - after.integratedLufs;
    check.densityDb = check.plrBeforeDb - check.plrAfterDb;
    check.overs = after.truePeakDb > kMasterPeakLimitDb;
    check.lowSideWide = after.lowSideDb > kMasterLowSideDb;

    const int bands = SpectrumAnalyzer::numBandsFor(kTonalScale);
    double mean = 0.0;
    int count = 0;
    for (int b = 0; b < bands; ++b)
    {
        const auto i = static_cast<size_t>(b);
        const bool heard = before.bandsDb[i] > -150.0 && after.bandsDb[i] > -150.0;
        check.tiltDb[i] = heard ? after.bandsDb[i] - before.bandsDb[i] : 0.0;
        const double hz = SpectrumAnalyzer::bandCentreHz(kTonalScale, b);
        if (heard && hz >= 60.0 && hz <= 8000.0)
        {
            mean += check.tiltDb[i];
            ++count;
        }
    }
    mean = count > 0 ? mean / count : 0.0;
    for (int b = 0; b < bands; ++b)
        if (before.bandsDb[static_cast<size_t>(b)] > -150.0 && after.bandsDb[static_cast<size_t>(b)] > -150.0)
            check.tiltDb[static_cast<size_t>(b)] -= mean;

    double largest = 0.0;
    for (int z = 0; z < kTonalZones; ++z)
    {
        double sum = 0.0;
        int n = 0;
        for (int b = 0; b < bands; ++b)
        {
            const double hz = SpectrumAnalyzer::bandCentreHz(kTonalScale, b);
            if (hz >= tonalZoneEdgeHz(z) && hz < tonalZoneEdgeHz(z + 1) && before.bandsDb[static_cast<size_t>(b)] > -150.0)
            {
                sum += check.tiltDb[static_cast<size_t>(b)];
                ++n;
            }
        }
        check.zoneChangeDb[static_cast<size_t>(z)] = n > 0 ? sum / n : 0.0;
        if (std::abs(check.zoneChangeDb[static_cast<size_t>(z)]) >= kMasterToneChangeDb
            && std::abs(check.zoneChangeDb[static_cast<size_t>(z)]) > largest)
        {
            largest = std::abs(check.zoneChangeDb[static_cast<size_t>(z)]);
            check.largestZone = z;
        }
    }
    return check;
}

Curve cappedMasterTilt(const MasterCheck& check)
{
    Curve tilt {};
    if (!check.valid)
        return tilt;
    for (size_t b = 0; b < tilt.size(); ++b)
        tilt[b] = std::clamp(check.tiltDb[b], -kMasterTiltCapDb, kMasterTiltCapDb);
    return tilt;
}

} // namespace ma
