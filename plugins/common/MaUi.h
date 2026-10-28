#pragma once

#include "ma/ChannelEq.h"
#include "ma/SpectrumAnalyzer.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cmath>
#include <functional>

namespace ma::ui
{

// UI text is Polish; JUCE needs non-ASCII literals decoded explicitly.
inline juce::String pl(const char* utf8)
{
    return juce::String::fromUTF8(utf8);
}

// Channel roles, in the order of the Agent's "role" parameter (the index travels to the Brain).
inline juce::StringArray roleNames()
{
    return { pl("Inny"), pl("Stopa"), pl("Bas"), pl("Perkusja"), pl("Wokal"), pl("Lead"),
             pl("Pad"), pl("Efekty"), pl("Bus"), pl("Wysyłka (pogłos, delay)") };
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

// One tab's content. The editor keeps the controls as its own members and says where they go.
class Page : public juce::Component
{
public:
    std::function<void(juce::Rectangle<int>)> layout;

    void resized() override
    {
        if (layout)
            layout(getLocalBounds());
    }
};

// Tabs in the plugins' colours, so the editors keep one fixed size that fits the screen. Pages stay owned
// by the editor.
class Tabs : public juce::TabbedComponent
{
public:
    Tabs() : juce::TabbedComponent(juce::TabbedButtonBar::TabsAtTop)
    {
        setTabBarDepth(30);
        setOutline(1);
        setIndent(0);
        setColour(juce::TabbedComponent::backgroundColourId, colours::background);
        setColour(juce::TabbedComponent::outlineColourId, colours::panel);
        setColour(juce::TabbedButtonBar::tabOutlineColourId, colours::panel);
        setColour(juce::TabbedButtonBar::frontOutlineColourId, colours::panel);
        setColour(juce::TabbedButtonBar::tabTextColourId, colours::quiet);
        setColour(juce::TabbedButtonBar::frontTextColourId, colours::text);
    }

    void add(const juce::String& name, Page& page, const juce::String& tooltip = {})
    {
        addTab(name, colours::panel, &page, false);
        getTabbedButtonBar().getTabButton(getNumTabs() - 1)->setTooltip(tooltip);
    }

    std::function<void(int)> onChange;

    void currentTabChanged(int index, const juce::String&) override
    {
        if (onChange)
            onChange(index);
    }
};

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

// Frequency response of the Brain's EQ on a channel, -12..+6 dB, log axis 20 Hz..20 kHz.
class EqCurveView : public juce::Component
{
public:
    void setEq(const ma::EqSettings& newEq, bool newBypassed)
    {
        bool changed = newBypassed != bypassed;
        for (int b = 0; b < ma::kEqBands; ++b)
        {
            const auto& a = newEq[static_cast<size_t>(b)];
            const auto& c = eq[static_cast<size_t>(b)];
            changed = changed || !ma::sameBand(a, c);
        }
        if (!changed)
            return;
        eq = newEq;
        bypassed = newBypassed;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour(colours::panel);
        g.fillRoundedRectangle(area, 4.0f);
        auto plot = area.reduced(8.0f, 6.0f);

        const auto yOf = [&plot](double db) {
            return plot.getY() + static_cast<float>(plot.getHeight() * (6.0 - juce::jlimit(-12.0, 6.0, db)) / 18.0);
        };
        g.setColour(colours::quiet.withAlpha(0.25f));
        for (double db : { 0.0, -6.0, -12.0 })
            g.drawHorizontalLine(juce::roundToInt(yOf(db)), plot.getX(), plot.getRight());

        juce::Path curve;
        const int steps = juce::jmax(2, static_cast<int>(plot.getWidth()));
        for (int i = 0; i < steps; ++i)
        {
            const double hz = 20.0 * std::pow(1000.0, static_cast<double>(i) / (steps - 1));
            const auto point = juce::Point<float>(plot.getX() + static_cast<float>(i), yOf(ma::eqResponseDb(eq, hz)));
            if (i == 0)
                curve.startNewSubPath(point);
            else
                curve.lineTo(point);
        }
        g.setColour(bypassed ? colours::quiet : colours::accent);
        g.strokePath(curve, juce::PathStrokeType(2.0f));

        g.setColour(colours::quiet);
        g.setFont(juce::FontOptions(10.0f));
        g.drawText("0 dB", plot.withHeight(12.0f).translated(0.0f, yOf(0.0) - plot.getY() - 12.0f), juce::Justification::left);
        g.drawText("-12", plot.withHeight(12.0f).translated(0.0f, plot.getHeight() - 12.0f), juce::Justification::left);
    }

private:
    ma::EqSettings eq {};
    bool bypassed = false;
};

// Phase per frequency band, like a per-band correlation meter: one bar per ERB band from
// 120 Hz up, +1 (mono) at the top, -1 (anti-phase) at the bottom. Red under 0, amber under
// 0.3. An optional second set (a selected channel) is drawn as dots over the bars.
class CoherenceView : public juce::Component
{
public:
    static constexpr int kBands = 40;

    // `audible` (optional) marks the bands the signal really plays in; the rest are drawn grey.
    void setBands(const double* coherence, bool isValid, const double* overlay = nullptr, const bool* audible = nullptr)
    {
        valid = isValid;
        hasOverlay = overlay != nullptr;
        for (size_t b = 0; b < values.size(); ++b)
        {
            values[b] = coherence != nullptr ? coherence[b] : 1.0;
            dots[b] = overlay != nullptr ? overlay[b] : 1.0;
            heard[b] = audible == nullptr || audible[b];
        }
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour(colours::panel);
        g.fillRoundedRectangle(area, 4.0f);
        area.reduce(8.0f, 6.0f);
        auto labels = area.removeFromBottom(14.0f);
        if (!valid)
        {
            g.setColour(colours::quiet);
            g.setFont(juce::FontOptions(13.0f));
            g.drawText(juce::String::fromUTF8("Czekam na sygnał stereo."), area, juce::Justification::centred);
            return;
        }
        int first = 0;
        while (first < kBands && SpectrumAnalyzer::bandCentreHz(BandScale::erb, first) < 120.0)
            ++first;
        const int count = kBands - first;
        const float width = area.getWidth() / static_cast<float>(count);
        const auto yOf = [&](double c) { return area.getY() + static_cast<float>((1.0 - juce::jlimit(-1.0, 1.0, c)) * 0.5) * area.getHeight(); };
        const float zero = yOf(0.0);
        g.setColour(colours::quiet.withAlpha(0.35f));
        g.drawHorizontalLine(juce::roundToInt(zero), area.getX(), area.getRight());
        g.drawHorizontalLine(juce::roundToInt(yOf(1.0)), area.getX(), area.getRight());
        for (int b = first; b < kBands; ++b)
        {
            const double c = values[static_cast<size_t>(b)];
            const float x = area.getX() + static_cast<float>(b - first) * width;
            const float y = yOf(c);
            g.setColour(!heard[static_cast<size_t>(b)] ? colours::quiet.withAlpha(0.25f)
                        : c < 0.0                    ? colours::critical
                        : c < 0.3                    ? colours::warning
                                                     : colours::good);
            g.fillRect(juce::Rectangle<float>(x + 1.0f, std::min(y, zero), width - 2.0f, std::max(1.5f, std::abs(zero - y))));
            if (hasOverlay)
            {
                g.setColour(colours::text);
                g.fillEllipse(x + width * 0.5f - 2.5f, yOf(dots[static_cast<size_t>(b)]) - 2.5f, 5.0f, 5.0f);
            }
        }
        g.setColour(colours::quiet);
        g.setFont(juce::FontOptions(11.0f));
        for (double hz : { 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        {
            int band = first;
            while (band < kBands - 1 && SpectrumAnalyzer::bandCentreHz(BandScale::erb, band) < hz)
                ++band;
            const float x = area.getX() + (static_cast<float>(band - first) + 0.5f) * width;
            g.drawText(hz >= 1000.0 ? juce::String(juce::roundToInt(hz / 1000.0)) + "k" : juce::String(juce::roundToInt(hz)),
                       juce::Rectangle<float>(x - 20.0f, labels.getY(), 40.0f, labels.getHeight()), juce::Justification::centred);
        }
    }

private:
    std::array<double, kBands> values {}, dots {};
    std::array<bool, kBands> heard {};
    bool valid = false, hasOverlay = false;
};

} // namespace ma::ui
