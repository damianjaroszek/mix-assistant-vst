#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cmath>

namespace ma::ui
{

// UI text is Polish; JUCE needs non-ASCII literals decoded explicitly.
inline juce::String pl(const char* utf8)
{
    return juce::String::fromUTF8(utf8);
}

inline juce::String formatDb(double db, const char* unit, int decimals = 1)
{
    if (!std::isfinite(db))
        return "-inf " + juce::String(unit);
    return juce::String(db, decimals) + " " + unit;
}

namespace colours
{
const juce::Colour background { 0xff1d2026 };
const juce::Colour panel { 0xff272b33 };
const juce::Colour text { 0xffe6e8eb };
const juce::Colour quiet { 0xff9aa0a8 };
const juce::Colour accent { 0xff4c9be8 };
const juce::Colour good { 0xff5cbf73 };
const juce::Colour warning { 0xffe8b04c };
const juce::Colour critical { 0xffe8584c };
} // namespace colours

// Horizontal level bar from -60 dB to 0 dB with an optional target marker.
class LevelBar : public juce::Component
{
public:
    void setLevel(double newDb)
    {
        const bool changed = std::isfinite(newDb) != std::isfinite(db) || std::abs(newDb - db) > 0.05;
        if (changed)
        {
            db = newDb;
            repaint();
        }
    }

    void setTarget(double newTargetDb)
    {
        targetDb = newTargetDb;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour(colours::panel);
        g.fillRoundedRectangle(area, 3.0f);

        const auto proportion = [](double value) {
            return std::isfinite(value) ? juce::jlimit(0.0, 1.0, (value + 60.0) / 60.0) : 0.0;
        };

        const auto fill = area.withWidth(static_cast<float>(area.getWidth() * proportion(db)));
        g.setColour(db > -1.0 ? colours::critical : db > -6.0 ? colours::warning : colours::accent);
        g.fillRoundedRectangle(fill, 3.0f);

        if (std::isfinite(targetDb))
        {
            const auto x = area.getX() + static_cast<float>(area.getWidth() * proportion(targetDb));
            g.setColour(colours::text);
            g.drawLine(x, area.getY(), x, area.getBottom(), 2.0f);
        }
    }

private:
    double db = -INFINITY;
    double targetDb = -INFINITY;
};

// Bar spectrum on a log-frequency axis from 20 Hz to 20 kHz, so third-octave and
// ERB layouts read the same way. Levels from floorDb (bottom) to 0 dB (top).
class SpectrumView : public juce::Component
{
public:
    static constexpr int kMaxBands = 40;

    explicit SpectrumView(double floor = -72.0) : floorDb(floor) {}

    void setBands(const double* levelsDb, const double* centresHz, int count)
    {
        numBands = juce::jmin(count, kMaxBands);
        for (int b = 0; b < numBands; ++b)
        {
            levels[static_cast<size_t>(b)] = levelsDb[b];
            centres[static_cast<size_t>(b)] = centresHz[b];
        }
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour(colours::panel);
        g.fillRoundedRectangle(area, 4.0f);

        auto plot = area.reduced(8.0f, 6.0f);
        const auto labelArea = plot.removeFromBottom(14.0f);

        const auto xOf = [&plot](double hz) {
            return plot.getX() + static_cast<float>(plot.getWidth() * std::log(hz / 20.0) / std::log(1000.0));
        };

        g.setColour(colours::quiet.withAlpha(0.2f));
        for (double db = 0.0; db >= floorDb; db -= 12.0)
        {
            const auto y = plot.getY() + static_cast<float>(plot.getHeight() * (db / floorDb));
            g.drawHorizontalLine(juce::roundToInt(y), plot.getX(), plot.getRight());
        }

        g.setColour(colours::accent);
        for (int b = 0; b < numBands; ++b)
        {
            // Each bar spans halfway to its neighbours on the log axis.
            const double hz = centres[static_cast<size_t>(b)];
            const double lowHz = b > 0 ? std::sqrt(hz * centres[static_cast<size_t>(b - 1)]) : hz * hz / std::sqrt(hz * centres[1]);
            const double highHz = b + 1 < numBands ? std::sqrt(hz * centres[static_cast<size_t>(b + 1)]) : hz * hz / lowHz;
            const double db = levels[static_cast<size_t>(b)];
            const double proportion = std::isfinite(db) ? juce::jlimit(0.0, 1.0, 1.0 - db / floorDb) : 0.0;
            const float h = static_cast<float>(plot.getHeight() * proportion);
            const float x0 = juce::jmax(plot.getX(), xOf(lowHz)) + 0.5f;
            const float x1 = juce::jmin(plot.getRight(), xOf(highHz)) - 0.5f;
            g.fillRect(x0, plot.getBottom() - h, juce::jmax(1.0f, x1 - x0), h);
        }

        g.setColour(colours::quiet);
        g.setFont(juce::FontOptions(10.0f));
        for (double hz : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        {
            const auto text = hz >= 1000.0 ? juce::String(juce::roundToInt(hz / 1000.0)) + "k" : juce::String(juce::roundToInt(hz));
            g.drawText(text, juce::Rectangle<float>(xOf(hz) - 16.0f, labelArea.getY(), 32.0f, 14.0f), juce::Justification::centred);
        }
    }

private:
    double floorDb;
    int numBands = 0;
    std::array<double, kMaxBands> levels {};
    std::array<double, kMaxBands> centres {};
};

} // namespace ma::ui
