#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

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

} // namespace ma::ui
