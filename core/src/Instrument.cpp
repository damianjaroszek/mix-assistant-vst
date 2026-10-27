#include "ma/Instrument.h"

#include <algorithm>
#include <cmath>

namespace ma
{
namespace
{
constexpr double kGateDb = -60.0;
constexpr double kOnsetRiseDb = 6.0;   // hop over the slow envelope that counts as an onset
constexpr int kMinOnsetGapHops = 12;   // 60 ms
constexpr int kDecayHops = 30;         // level measured 150 ms after the onset
constexpr int kErbBands = 40;
constexpr int kTailStartHops = 30, kTailEndHops = 60; // 150-300 ms after an onset: past a kick's pitch sweep
constexpr double kTailLowHz = 30.0, kTailHighHz = 130.0;
constexpr long long kMinTailHops = 40;              // about two seconds of hits

double powerToDb(double p)
{
    return 10.0 * std::log10(std::max(p, 1e-20));
}

// 0 below lo, 1 above hi, linear between.
double ramp(double x, double lo, double hi)
{
    return std::clamp((x - lo) / (hi - lo), 0.0, 1.0);
}

// 1 inside [lo, hi], falling to 0 an octave outside.
double within(double hz, double lo, double hi)
{
    if (hz < lo)
        return ramp(std::log2(hz / lo), -1.0, 0.0);
    if (hz > hi)
        return 1.0 - ramp(std::log2(hz / hi), 0.0, 1.0);
    return 1.0;
}
} // namespace

void InstrumentFeatures::prepare(double newSampleRate)
{
    sampleRate = newSampleRate;
    hopLength = std::max(1, static_cast<int>(std::lround(0.005 * sampleRate)));
    slowCoeff = std::exp(-1.0 / (0.1 / 0.005)); // 100 ms, per hop
    pitchTracker.prepare(sampleRate);
    reset();
}

void InstrumentFeatures::reset()
{
    hopFill = 0;
    hopMid = hopSide = 0.0;
    lastHops.fill(0.0);
    hopIndex = 0;
    slowPower = 0.0;
    recentPeakDb = -200.0;
    hops = activeHops = onsets = decays = 0;
    sinceOnset = 1 << 20;
    onsetPeakDb = -200.0;
    onsetLowDb = 200.0;
    decaySum = 0.0;
    midSum = sideSum = 0.0;
    havePrevious = false;
    movementSum = 0.0;
    movementFrames = 0;
    recentHops.fill(0.0);
    recentIndex = 0;
    levelHistogram.fill(0);
    pitchTracker.reset();
    noteHistogram.fill(0);
    noteHops = 0;
    tailHistogram.fill(0);
    tailHops = 0;
}

void InstrumentFeatures::addSpectrumFrame(const SpectrumAnalyzer::Bands& power, int numBands)
{
    double total = 0.0;
    for (int b = 0; b < numBands; ++b)
        total += power[static_cast<size_t>(b)];
    if (total < 1e-10) // under -100 dB: nothing playing
    {
        havePrevious = false;
        return;
    }
    // Distance between consecutive shapes (each normalised to 1): 0 = same, 1 = nothing shared.
    SpectrumAnalyzer::Bands shape {};
    double distance = 0.0;
    for (int b = 0; b < numBands; ++b)
    {
        shape[static_cast<size_t>(b)] = power[static_cast<size_t>(b)] / total;
        distance += std::abs(shape[static_cast<size_t>(b)] - previousShape[static_cast<size_t>(b)]);
    }
    if (havePrevious)
    {
        movementSum += 0.5 * distance;
        ++movementFrames;
    }
    previousShape = shape;
    havePrevious = true;
}

void InstrumentFeatures::process(const float* const* channels, int numChannels, int numSamples)
{
    if (numChannels <= 0)
        return;
    for (int i = 0; i < numSamples; ++i)
    {
        const double l = channels[0][i];
        const double r = numChannels > 1 ? channels[1][i] : l;
        const double mid = 0.5 * (l + r), side = 0.5 * (l - r);
        hopMid += mid * mid;
        hopSide += side * side;
        pitchTracker.process(mid);
        if (++hopFill == hopLength)
            endHop();
    }
}

void InstrumentFeatures::endHop()
{
    // 20 ms windows, so a bass note's own cycles do not read as level changes.
    lastHops[static_cast<size_t>(hopIndex)] = (hopMid + hopSide) / hopLength;
    hopIndex = (hopIndex + 1) % static_cast<int>(lastHops.size());
    const double power = (lastHops[0] + lastHops[1] + lastHops[2] + lastHops[3]) / 4.0;
    const double db = powerToDb(power);
    ++hops;
    recentPeakDb = std::max(db, recentPeakDb - 0.005); // about 1 dB a second
    const bool active = db > kGateDb && db > recentPeakDb - 40.0;
    recentHops[static_cast<size_t>(recentIndex)] = lastHops[static_cast<size_t>((hopIndex + 3) % 4)];
    recentIndex = (recentIndex + 1) % static_cast<int>(recentHops.size());
    if (active)
    {
        ++activeHops;
        midSum += hopMid;
        sideSum += hopSide;
        double sum = 0.0;
        for (double p : recentHops)
            sum += p;
        const double level = powerToDb(sum / static_cast<double>(recentHops.size()));
        const int bin = static_cast<int>(std::floor((level + 120.0) * 2.0));
        if (bin >= 0)
            ++levelHistogram[static_cast<size_t>(std::min(bin, static_cast<int>(levelHistogram.size()) - 1))];
        const double note = pitchTracker.pitchHz();
        if (note > 0.0)
        {
            ++noteHistogram[static_cast<size_t>(std::clamp(std::lround(note), 0L, 160L))];
            ++noteHops;
        }
    }

    // Onset: a jump over the slow envelope, at least 60 ms after the last one. How far the
    // level falls after it (lowest point before 150 ms or the next onset) says hit or note.
    ++sinceOnset;
    const bool onset = active && db - powerToDb(slowPower) > kOnsetRiseDb && sinceOnset >= kMinOnsetGapHops;
    if (onset && sinceOnset < kDecayHops && onsetLowDb < 199.0)
    {
        decaySum += std::min(onsetPeakDb - onsetLowDb, 40.0);
        ++decays;
    }
    if (onset)
    {
        ++onsets;
        sinceOnset = 0;
        onsetPeakDb = db;
        onsetLowDb = 200.0;
    }
    if (sinceOnset > 0 && sinceOnset <= 4)
        onsetPeakDb = std::max(onsetPeakDb, db); // the hit's peak, within 20 ms
    else if (sinceOnset > 4 && sinceOnset <= kDecayHops)
        onsetLowDb = std::min(onsetLowDb, db);
    if (active && sinceOnset >= kTailStartHops && sinceOnset <= kTailEndHops)
    {
        const double tail = pitchTracker.pitchHz();
        if (tail >= kTailLowHz && tail <= kTailHighHz)
        {
            const auto bin = std::lround(120.0 * std::log2(tail / kTailLowHz));
            ++tailHistogram[static_cast<size_t>(std::clamp(bin, 0L, static_cast<long>(tailHistogram.size()) - 1))];
            ++tailHops;
        }
    }
    if (sinceOnset == kDecayHops)
    {
        decaySum += std::min(onsetPeakDb - onsetLowDb, 40.0);
        ++decays;
        onsetLowDb = 200.0;
    }

    slowPower = slowCoeff * slowPower + (1.0 - slowCoeff) * power;
    hopFill = 0;
    hopMid = hopSide = 0.0;
}

InstrumentFeatures::Summary InstrumentFeatures::summary() const
{
    Summary s;
    const double hopSeconds = static_cast<double>(hopLength) / sampleRate;
    s.seconds = activeHops * hopSeconds;
    s.valid = s.seconds >= 2.0;
    if (hops > 0)
        s.activeShare = static_cast<double>(activeHops) / static_cast<double>(hops);
    if (activeHops > 0)
        s.onsetsPerSecond = onsets / (static_cast<double>(hops) * hopSeconds);
    s.decayDb = decays > 0 ? decaySum / static_cast<double>(decays) : 0.0;
    s.width = midSum > 1e-20 ? sideSum / midSum : 0.0;
    s.movement = movementFrames > 0 ? movementSum / static_cast<double>(movementFrames) : 0.0;

    long long total = 0;
    for (auto n : levelHistogram)
        total += n;
    const auto percentile = [&](double share) {
        long long seen = 0;
        for (size_t b = 0; b < levelHistogram.size(); ++b)
        {
            seen += levelHistogram[b];
            if (static_cast<double>(seen) >= share * static_cast<double>(total))
                return -120.0 + (static_cast<double>(b) + 0.5) / 2.0;
        }
        return 0.0;
    };
    if (total > 0)
    {
        s.levelP10 = percentile(0.10);
        s.levelP50 = percentile(0.50);
        s.levelP95 = percentile(0.95);
    }
    // A bass line plays clear low notes most of the time; a pad or a kick does not.
    if (activeHops > 0 && static_cast<double>(noteHops) > 0.3 * static_cast<double>(activeHops))
    {
        long long seen = 0;
        for (size_t hz = 0; hz < noteHistogram.size(); ++hz)
        {
            seen += noteHistogram[hz];
            if (2 * seen >= noteHops)
            {
                s.lowNoteHz = static_cast<double>(hz);
                break;
            }
        }
    }
    if (tailHops >= kMinTailHops && onsets >= 4)
    {
        long long seen = 0;
        for (size_t b = 0; b < tailHistogram.size(); ++b)
        {
            seen += tailHistogram[b];
            if (2 * seen >= tailHops)
            {
                s.tailPitchHz = kTailLowHz * std::pow(2.0, static_cast<double>(b) / 120.0);
                break;
            }
        }
    }
    return s;
}

RoleGuess recogniseRole(const InstrumentFeatures::Summary& f, const SpectrumAnalyzer::Bands& bandsDb)
{
    RoleGuess guess;
    if (!f.valid)
        return guess;

    // Where the energy sits.
    double total = 0.0, sub = 0.0, low = 0.0, high = 0.0, air = 0.0, logCentre = 0.0;
    for (int b = 0; b < kErbBands; ++b)
    {
        const double hz = SpectrumAnalyzer::bandCentreHz(BandScale::erb, b);
        const double p = std::pow(10.0, bandsDb[static_cast<size_t>(b)] / 10.0);
        total += p;
        logCentre += p * std::log2(hz);
        if (hz < 120.0)
            sub += p;
        if (hz < 250.0)
            low += p;
        if (hz > 2000.0)
            high += p;
        if (hz > 6000.0)
            air += p;
    }
    if (total < 1e-15)
        return guess;
    sub /= total;
    low /= total;
    high /= total;
    air /= total;
    const double centroid = std::pow(2.0, logCentre / total);
    guess.centroidHz = centroid;

    // How it moves: hits that die away, or notes that hold.
    const double hits = ramp(f.decayDb, 5.0, 10.0) * ramp(f.onsetsPerSecond, 0.4, 1.2);
    const double holds = 1.0 - ramp(f.decayDb, 3.0, 8.0);
    const double busy = std::max(ramp(f.onsetsPerSecond, 0.8, 3.0), ramp(f.movement, 0.04, 0.12));
    const double pauses = ramp(1.0 - f.activeShare, 0.08, 0.3);
    const double wide = ramp(f.width, 0.08, 0.4);

    // Pads are held, dark and wide; leads are brighter and sit in the middle. Without a
    // trained model that is the most reliable split between them.
    auto& s = guess.scores;
    s[static_cast<size_t>(Role::kick)] = hits * ramp(sub, 0.3, 0.6) * (1.0 - ramp(centroid, 250.0, 700.0));
    s[static_cast<size_t>(Role::bass)] = ramp(low, 0.45, 0.75) * (1.0 - 0.8 * hits) * (1.0 - ramp(centroid, 350.0, 900.0));
    s[static_cast<size_t>(Role::percussion)] = hits * (1.0 - ramp(low, 0.15, 0.4)) * ramp(centroid, 600.0, 2000.0);
    s[static_cast<size_t>(Role::pad)] = holds * (1.0 - ramp(f.onsetsPerSecond, 0.8, 3.0)) * within(centroid, 150.0, 1200.0)
                                        * (0.4 + 0.6 * wide) * (1.0 - pauses) * (1.0 - ramp(low, 0.3, 0.6));
    s[static_cast<size_t>(Role::lead)] = (0.3 + 0.7 * holds) * within(centroid, 500.0, 5000.0) * (1.0 - 0.7 * wide)
                                         * (1.0 - ramp(low, 0.15, 0.35)) * (1.0 - 0.5 * pauses) * (1.0 - 0.6 * hits)
                                         * (0.6 + 0.4 * busy);
    s[static_cast<size_t>(Role::vocal)] = pauses * within(centroid, 400.0, 3000.0) * (1.0 - ramp(sub, 0.03, 0.15))
                                          * (1.0 - ramp(air, 0.25, 0.5)) * (1.0 - 0.7 * hits) * (1.0 - 0.5 * wide);
    s[static_cast<size_t>(Role::effects)] = 0.5 * ramp(high, 0.5, 0.85) * holds * (1.0 - busy);

    int best = 0, second = 0;
    for (int r = 1; r < kRoles; ++r)
    {
        if (s[static_cast<size_t>(r)] > s[static_cast<size_t>(best)])
        {
            second = best;
            best = r;
        }
        else if (s[static_cast<size_t>(r)] > s[static_cast<size_t>(second)])
        {
            second = r;
        }
    }
    const double top = s[static_cast<size_t>(best)], next = s[static_cast<size_t>(second)];
    if (best == 0 || top <= 0.0)
        return guess;
    guess.role = static_cast<Role>(best);
    guess.runnerUp = static_cast<Role>(second);
    // Clear when the best rule fits well and the next one is far behind.
    guess.confidence = std::clamp(top / 0.5, 0.0, 1.0) * top / (top + next);
    return guess;
}

int defaultPriority(Role role)
{
    switch (role)
    {
    case Role::kick: return 1;
    case Role::bass: return 2;
    case Role::vocal: return 2;
    case Role::lead: return 3;
    case Role::percussion: return 4;
    case Role::pad: return 6;
    case Role::effects: return 8;
    case Role::send: return 9;
    case Role::other:
    case Role::bus: break;
    }
    return 5;
}

} // namespace ma
