#include "ma/TonalBalance.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace ma
{
namespace
{
constexpr double kNormLowHz = 60.0;
constexpr double kNormHighHz = 8000.0;
constexpr double kMinRegionDeviationDb = 1.0;
constexpr double kFloorDb = -150.0;

int numBands()
{
    return SpectrumAnalyzer::numBandsFor(kTonalScale);
}

double hzOf(int band)
{
    return SpectrumAnalyzer::bandCentreHz(kTonalScale, band);
}

// Least spread a corridor gets: references disagree more at the extremes of the spectrum.
double minimumHalfWidthDb(double hz, int references)
{
    const double base = references > 1 ? 1.5 : 2.0;
    if (hz < 60.0 || hz > 10000.0)
        return base + 1.5;
    if (hz < 120.0 || hz > 6000.0)
        return base + 0.5;
    return base;
}

double quantile(std::vector<double> values, double q)
{
    std::sort(values.begin(), values.end());
    const double position = q * static_cast<double>(values.size() - 1);
    const auto below = static_cast<size_t>(std::floor(position));
    const auto above = std::min(below + 1, values.size() - 1);
    const double t = position - static_cast<double>(below);
    return values[below] * (1.0 - t) + values[above] * t;
}
} // namespace

Curve normaliseCurve(const Curve& levelsDb)
{
    double sum = 0.0;
    int count = 0;
    for (int b = 0; b < numBands(); ++b)
    {
        const double hz = hzOf(b);
        if (hz >= kNormLowHz && hz <= kNormHighHz && std::isfinite(levelsDb[static_cast<size_t>(b)]))
        {
            sum += levelsDb[static_cast<size_t>(b)];
            ++count;
        }
    }
    const double offset = count > 0 ? sum / count : 0.0;
    Curve out {};
    for (int b = 0; b < numBands(); ++b)
    {
        const double v = levelsDb[static_cast<size_t>(b)];
        out[static_cast<size_t>(b)] = std::isfinite(v) && v > kFloorDb ? v - offset : kFloorDb;
    }
    return out;
}

void TonalTarget::rebuild()
{
    centre = low = high = Curve {};
    if (references.empty())
        return;
    const int n = static_cast<int>(references.size());
    for (int b = 0; b < numBands(); ++b)
    {
        std::vector<double> values;
        for (const auto& r : references)
            values.push_back(r[static_cast<size_t>(b)]);
        const double median = quantile(values, 0.5);
        const double halfWidth = minimumHalfWidthDb(hzOf(b), n);
        centre[static_cast<size_t>(b)] = median;
        low[static_cast<size_t>(b)] = std::min(median - halfWidth, n > 2 ? quantile(values, 0.1) : median - halfWidth);
        high[static_cast<size_t>(b)] = std::max(median + halfWidth, n > 2 ? quantile(values, 0.9) : median + halfWidth);
    }
}

TonalComparison compareToTarget(const Curve& mixLevelsDb, const TonalTarget& target)
{
    TonalComparison result;
    if (!target.valid())
        return result;

    // Align the mix to the target over the normalisation range, so a mix that is louder
    // overall does not read as "too much everywhere".
    const auto mix = normaliseCurve(mixLevelsDb);
    double offset = 0.0;
    int count = 0;
    for (int b = 0; b < numBands(); ++b)
    {
        const double hz = hzOf(b);
        if (hz >= kNormLowHz && hz <= kNormHighHz && mix[static_cast<size_t>(b)] > kFloorDb + 1.0)
        {
            offset += target.centre[static_cast<size_t>(b)] - mix[static_cast<size_t>(b)];
            ++count;
        }
    }
    if (count == 0)
        return result;
    offset /= count;

    for (int b = 0; b < numBands(); ++b)
    {
        const auto i = static_cast<size_t>(b);
        const double v = mix[i] > kFloorDb + 1.0 ? mix[i] + offset : kFloorDb;
        result.mixDb[i] = v;
    }
    // Judged smoothed over a band each side (about a third of an octave), mix and corridor alike,
    // the way Tonal Balance Control draws them: one note of the bass line or a chord standing out
    // of a single band is the music, not the tone, and must not get a cut of its own.
    const auto smoothed = [&](const Curve& curve, int b) {
        double sum = 2.0 * curve[static_cast<size_t>(b)], weight = 2.0;
        for (int n : { b - 1, b + 1 })
            if (n >= 0 && n < numBands() && result.mixDb[static_cast<size_t>(n)] > kFloorDb)
            {
                sum += curve[static_cast<size_t>(n)];
                weight += 1.0;
            }
        return sum / weight;
    };
    for (int b = 0; b < numBands(); ++b)
    {
        const auto i = static_cast<size_t>(b);
        if (result.mixDb[i] <= kFloorDb)
        {
            result.deviationDb[i] = result.mixDb[i] - target.low[i];
            continue;
        }
        const double v = smoothed(result.mixDb, b), low = smoothed(target.low, b), high = smoothed(target.high, b);
        result.deviationDb[i] = v < low ? v - low : v > high ? v - high : 0.0;
    }

    // Contiguous runs outside the corridor on the same side.
    for (int b = 0; b < numBands();)
    {
        const double d = result.deviationDb[static_cast<size_t>(b)];
        if (d == 0.0)
        {
            ++b;
            continue;
        }
        const bool below = d < 0.0;
        TonalRegion region;
        region.firstBand = b;
        double sum = 0.0;
        while (b < numBands() && result.deviationDb[static_cast<size_t>(b)] != 0.0
               && (result.deviationDb[static_cast<size_t>(b)] < 0.0) == below)
            sum += result.deviationDb[static_cast<size_t>(b++)];
        region.lastBand = b - 1;
        region.deviationDb = sum / (region.lastBand - region.firstBand + 1);
        region.lowHz = SpectrumAnalyzer::bandEdgeHz(kTonalScale, region.firstBand);
        region.highHz = SpectrumAnalyzer::bandEdgeHz(kTonalScale, region.lastBand + 1);
        if (std::abs(region.deviationDb) >= kMinRegionDeviationDb)
            result.regions.push_back(region);
    }
    std::sort(result.regions.begin(), result.regions.end(), [](const TonalRegion& a, const TonalRegion& b) {
        return std::abs(a.deviationDb) * (a.lastBand - a.firstBand + 1) > std::abs(b.deviationDb) * (b.lastBand - b.firstBand + 1);
    });
    result.valid = true;
    return result;
}

double tonalZoneEdgeHz(int edge)
{
    // Where TBC draws its range lines; the extremes stop where references disagree most.
    static constexpr double edges[kTonalZones + 1] { 40.0, 250.0, 2000.0, 9000.0, 16000.0 };
    return edges[std::clamp(edge, 0, kTonalZones)];
}

std::array<TonalZone, kTonalZones> zoneOffsets(const TonalComparison& comparison, const TonalTarget& target)
{
    std::array<TonalZone, kTonalZones> zones {};
    if (!comparison.valid || !target.valid())
        return zones;
    for (int z = 0; z < kTonalZones; ++z)
    {
        auto& zone = zones[static_cast<size_t>(z)];
        zone.lowHz = tonalZoneEdgeHz(z);
        zone.highHz = tonalZoneEdgeHz(z + 1);
        zone.firstBand = numBands();
        int bands = 0, playing = 0;
        double sum = 0.0;
        for (int b = 0; b < numBands(); ++b)
        {
            const double hz = hzOf(b);
            if (hz < zone.lowHz || hz >= zone.highHz)
                continue;
            zone.firstBand = std::min(zone.firstBand, b);
            zone.lastBand = std::max(zone.lastBand, b);
            ++bands;
            const auto i = static_cast<size_t>(b);
            if (comparison.mixDb[i] > kFloorDb + 1.0)
            {
                sum += comparison.mixDb[i] - target.centre[i];
                ++playing;
            }
        }
        zone.valid = playing >= 2 && 2 * playing > bands;
        zone.offsetDb = playing > 0 ? sum / playing : 0.0;
    }
    return zones;
}

double outsideCorridorDb(const TonalComparison& comparison)
{
    double sum = 0.0;
    for (int b = 0; b < numBands(); ++b)
        sum += std::abs(comparison.deviationDb[static_cast<size_t>(b)]);
    return sum;
}

std::string serialiseTarget(const TonalTarget& target)
{
    std::ostringstream out;
    out << "ma-target 1 erb " << numBands() << "\n" << target.name << "\n";
    out.setf(std::ios::fixed);
    out.precision(2);
    for (const auto& r : target.references)
    {
        for (int b = 0; b < numBands(); ++b)
            out << (b ? " " : "") << r[static_cast<size_t>(b)];
        out << "\n";
    }
    return out.str();
}

bool parseTarget(const std::string& text, TonalTarget& target)
{
    std::istringstream in(text);
    std::string magic, scale;
    int version = 0, bands = 0;
    if (!(in >> magic >> version >> scale >> bands) || magic != "ma-target" || version != 1 || scale != "erb" || bands != numBands())
        return false;
    std::string line;
    std::getline(in, line);
    TonalTarget parsed;
    if (!std::getline(in, parsed.name))
        return false;
    // Files edited or written on Windows may end lines with CR LF.
    while (!parsed.name.empty() && (parsed.name.back() == '\r' || parsed.name.back() == ' '))
        parsed.name.pop_back();
    while (std::getline(in, line))
    {
        std::istringstream values(line);
        Curve c {};
        int b = 0;
        for (; b < numBands() && values >> c[static_cast<size_t>(b)]; ++b)
        {
        }
        if (b == numBands())
            parsed.references.push_back(c);
    }
    parsed.rebuild();
    target = parsed;
    return true;
}

TonalTarget builtinHouseTarget()
{
    // The drops (loudest 32 s) of thirteen club masters the owner picked as the sound to aim for (house, tech house,
    // progressive, trance; 2026-11-14), measured with ma-lab --make-target. Two came from lossy sources cut at
    // 16 kHz: their two top bands carry the others' median.
    static const double kReferences[13][40] = {
        { 7.3, 12.7, 9.5, 10.1, 9.6, 6.9, 5.3, 0.8, 1.6, 2.8, 2.6, -0.1, -1.8, -3.0, -0.9, -0.4, -2.8, -0.2, -3.1, -4.7, -3.3, -3.5, -0.9, -3.2, -2.9, -4.5, -1.5, -4.1, -4.6, -5.2, -5.5, -5.7, -6.4, -6.5, -7.5, -9.2, -10.7, -12.1, -14.9, -18.8 },
        { 13.4, 17.5, 13.2, 7.7, 5.1, 2.7, 2.6, -1.8, -0.9, 0.2, 2.7, 0.2, 2.1, 2.1, -0.3, 1.2, 1.2, -0.7, -0.4, -3.1, -2.5, -3.4, -3.0, -4.6, -2.9, -4.3, -4.8, -5.7, -5.5, -5.0, -4.6, -5.0, -5.1, -4.9, -5.9, -7.2, -8.9, -10.8, -15.1, -18.8 },
        { 9.4, 13.9, 13.6, 7.2, 10.5, 3.1, 2.5, 0.4, -0.9, -1.4, -0.6, 1.3, 3.3, 5.3, 0.1, 2.7, 0.2, 1.2, -0.1, -2.2, -1.9, -5.6, -4.2, -5.6, -5.0, -3.5, -6.2, -6.3, -6.1, -5.2, -5.0, -5.5, -5.6, -5.4, -5.1, -6.2, -8.6, -11.7, -14.8, -20.5 },
        { 16.8, 21.1, 16.4, 11.6, 7.8, 5.8, 0.8, 1.4, 2.6, 0.5, 0.1, -4.1, -1.9, -2.0, -2.3, -2.6, -1.9, -3.2, -2.8, -1.4, -3.7, -4.8, -4.8, -4.8, -5.9, -5.4, -3.6, -4.1, -3.1, -3.0, -1.0, -1.6, -1.1, -1.6, -1.3, -1.7, -6.0, -9.0, -12.7, -17.0 },
        { 15.9, 17.5, 16.0, 9.5, 8.2, 4.8, 2.6, -0.2, -3.1, -1.2, 2.6, -2.9, -1.1, 7.7, -0.1, 2.4, -3.5, -2.9, 2.5, -3.7, -4.7, -6.2, -5.8, -8.3, -5.9, -4.5, -2.9, -2.6, -4.7, -3.4, -3.9, -2.1, -0.7, -1.9, -3.1, -6.3, -8.5, -11.9, -13.7, -17.5 },
        { 17.3, 17.5, 12.5, 9.4, 9.4, 4.3, 1.6, 2.1, 2.0, -0.2, -1.7, -0.1, -0.5, -0.6, -0.5, -3.4, -2.7, -2.4, -3.5, -4.5, -4.5, -4.7, -4.5, -4.0, -5.0, -3.0, -3.0, -2.5, -1.9, -2.1, -1.8, -1.7, -3.0, -4.3, -5.2, -7.6, -10.5, -15.0, -20.3, -27.1 },
        { 17.2, 19.8, 17.8, 7.3, 6.4, 1.6, 0.3, -2.6, 0.4, -3.4, -6.0, -7.6, -7.4, -7.7, -5.0, -3.9, -3.3, -2.5, -1.4, -2.9, -2.9, -1.0, -1.7, -0.8, -1.1, -0.7, 0.4, 1.2, 1.8, 1.8, 2.0, 1.0, -0.6, -1.2, -3.2, -3.6, -6.6, -10.8, -19.3, -24.8 },
        { 21.4, 23.1, 14.1, 13.2, 5.8, 7.7, 4.4, 5.7, 2.6, 8.0, 4.6, 0.0, 4.5, -0.9, 2.3, -2.5, -2.0, 5.2, -3.3, -0.1, 0.7, 1.8, -4.0, -6.0, -9.6, -10.1, -10.3, -11.4, -11.2, -11.3, -10.5, -10.7, -10.6, -9.8, -9.3, -9.8, -11.0, -11.7, -16.2, -23.9 },
        { 12.3, 16.2, 10.5, 9.8, -0.3, -1.3, 2.2, -0.1, 0.2, -2.1, 0.0, -4.2, -1.2, -0.7, -1.9, 2.1, -0.8, 1.4, -1.1, 0.7, -0.7, -2.4, -2.6, -2.8, -4.9, -3.8, -5.1, -3.3, -2.2, -2.3, 1.0, -0.3, -1.5, -2.4, -2.2, -3.9, -5.8, -10.9, -14.9, -20.5 },
        { 24.8, 21.0, 16.4, 13.6, 6.4, 6.5, 7.5, 3.0, 2.4, 3.2, 0.2, 1.8, -1.9, 3.0, -1.4, -1.4, -3.4, -3.8, -5.7, -7.3, -6.9, -8.2, -7.2, -7.7, -6.0, -6.4, -4.7, -3.2, -4.0, -2.6, -2.2, -1.0, -0.3, 0.0, -3.3, -5.9, -8.3, -12.0, -15.0, -24.8 },
        { 10.4, 15.4, 11.1, 11.6, 3.2, 2.9, 2.6, 3.2, 0.9, 5.5, 3.7, 3.3, 3.2, 3.2, -0.7, -2.5, -2.8, -0.8, -3.2, -2.4, -2.6, -2.2, -2.4, -2.1, -3.0, -4.6, -5.8, -6.5, -7.4, -7.0, -7.6, -6.2, -6.5, -4.1, -2.3, -3.8, -3.2, -4.6, -7.7, -8.8 },
        { 13.6, 18.4, 12.3, 8.8, 4.8, 3.4, 4.2, 2.9, -0.2, 1.9, 2.9, 1.3, -1.5, -0.2, -1.8, -2.3, -1.6, -3.4, -0.8, -3.6, -3.0, -5.3, -4.3, -4.8, -4.8, -3.0, -4.4, -3.5, -4.5, -1.9, -3.7, -2.5, -3.5, -2.5, -4.1, -5.2, -6.5, -10.9, -14.9, -21.5 },
        { 15.6, 18.5, 9.8, 6.6, 4.3, 2.9, 5.1, 4.5, 6.4, -0.7, 2.2, 1.8, -1.4, 1.1, 1.7, -2.2, -2.0, -2.8, -1.7, -3.1, -4.1, -4.4, -5.0, -4.2, -3.7, -6.3, -5.4, -4.4, -3.9, -4.1, -2.6, -3.0, -3.6, -3.7, -3.2, -4.1, -7.6, -11.4, -14.9, -20.5 }
    };
    TonalTarget target;
    target.name = kBuiltinTargetName;
    for (const auto& reference : kReferences)
    {
        Curve curve {};
        for (int b = 0; b < numBands() && b < 40; ++b)
            curve[static_cast<size_t>(b)] = reference[b];
        target.references.push_back(normaliseCurve(curve));
    }
    target.rebuild();
    return target;
}

} // namespace ma
