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
        result.deviationDb[i] = v < target.low[i] ? v - target.low[i] : v > target.high[i] ? v - target.high[i] : 0.0;
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

} // namespace ma
