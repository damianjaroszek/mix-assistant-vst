#include "ma/LowEnd.h"

#include "ma/Decibels.h"
#include "ma/SpectrumAnalyzer.h"

#include <algorithm>
#include <cmath>

namespace ma
{

double curveRegionDb(const Curve& curve, double lowHz, double highHz)
{
    const int bands = SpectrumAnalyzer::numBandsFor(kTonalScale);
    double power = 0.0;
    bool any = false;
    for (int b = 0; b < bands; ++b)
    {
        const double hz = SpectrumAnalyzer::bandCentreHz(kTonalScale, b);
        if (hz < lowHz || hz >= highHz)
            continue;
        const double db = curve[static_cast<size_t>(b)];
        if (!std::isfinite(db))
            continue;
        power += std::pow(10.0, db / 10.0);
        any = true;
    }
    return any ? powerToDb(power) : -200.0;
}

double duckRecoveryMs(const DuckSettings& duck)
{
    if (duck.on < 0.5f || duck.releaseMs <= 0.0f)
        return 0.0;
    // One time constant lets about 63% back; 2.2 of them about 90%, which is where the ear
    // stops hearing the hole.
    return 2.2 * static_cast<double>(duck.releaseMs);
}

LowStatus LowEndReport::worst() const
{
    LowStatus worst = LowStatus::unknown;
    for (const auto& part : parts)
        if (static_cast<int>(part.status) > static_cast<int>(worst))
            worst = part.status;
    return worst;
}

namespace
{
LowStatus judge(double distance, double warnAt, double problemAt)
{
    if (distance >= problemAt)
        return LowStatus::problem;
    return distance >= warnAt ? LowStatus::warning : LowStatus::good;
}
} // namespace

LowEndReport analyseLowEnd(const LowEndInput& input)
{
    LowEndReport report;
    // The loudest bass channel carries the line; the others (a sub layer, a bass FX) follow it.
    int main = -1;
    for (size_t c = 0; c < input.channels.size(); ++c)
        if (input.channels[c].bass && (main < 0 || input.channels[c].levelDb > input.channels[static_cast<size_t>(main)].levelDb))
            main = static_cast<int>(c);

    if (input.tonalValid)
    {
        report.valid = true;
        const double sub = curveRegionDb(input.mixDb, 0.0, kSubTopHz);
        const double body = curveRegionDb(input.mixDb, kSubTopHz, kBassTopHz);
        const double subTarget = curveRegionDb(input.targetDb, 0.0, kSubTopHz);
        const double bodyTarget = curveRegionDb(input.targetDb, kSubTopHz, kBassTopHz);
        if (sub > -190.0 && body > -190.0)
        {
            auto& part = report.parts[static_cast<size_t>(LowPart::subVsBody)];
            part.measured = sub - body;
            part.target = subTarget - bodyTarget;
            part.delta = part.measured - part.target;
            part.status = judge(std::abs(part.delta), kSubBalanceWarnDb, kSubBalanceProblemDb);
        }
        const double lowMid = curveRegionDb(input.mixDb, kBassTopHz, kLowMidTopHz);
        const double lowMidTarget = curveRegionDb(input.targetDb, kBassTopHz, kLowMidTopHz);
        if (lowMid > -190.0)
        {
            auto& part = report.parts[static_cast<size_t>(LowPart::space)];
            part.measured = lowMid;
            part.target = lowMidTarget;
            part.delta = lowMid - lowMidTarget;
            part.status = judge(std::abs(part.delta), kSpaceWarnDb, kSpaceProblemDb);
        }
    }

    if (main >= 0)
    {
        const auto& bass = input.channels[static_cast<size_t>(main)];
        if (bass.notesKnown)
        {
            auto& part = report.parts[static_cast<size_t>(LowPart::notes)];
            report.valid = true;
            part.channel = main;
            // The spread is measured before the chain, so what the evener already takes off comes out
            // of it; otherwise the finding would stay red after it was fixed.
            const double left = bass.noteSpreadDb * std::clamp(1.0 - bass.evenerAmount, 0.0, 1.0);
            part.measured = left;
            part.extra = bass.noteSpreadDb;
            part.target = kNoteSpreadWarnDb;
            part.delta = left;
            part.status = judge(left, kNoteSpreadWarnDb, kNoteSpreadProblemDb);
        }
        const double recovery = duckRecoveryMs(bass.duck);
        if (recovery > 0.0 && input.bpm > 20.0)
        {
            auto& part = report.parts[static_cast<size_t>(LowPart::duckReturn)];
            report.valid = true;
            part.channel = main;
            const double beat = 60000.0 / input.bpm;
            part.measured = recovery;
            part.target = beat;
            part.extra = beat;
            part.delta = recovery - beat;
            // In the groove: back by the next beat, but not so fast that the dip only clicks.
            const double sixteenth = beat / 4.0;
            if (recovery > 2.0 * beat || recovery < 0.5 * sixteenth)
                part.status = LowStatus::problem;
            else if (recovery > beat || recovery < sixteenth)
                part.status = LowStatus::warning;
            else
                part.status = LowStatus::good;
        }
    }
    return report;
}

} // namespace ma
