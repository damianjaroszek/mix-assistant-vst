#pragma once

#include "BrainProcessor.h"
#include "MaUi.h"
#include "ma/ChannelAnalyzer.h"
#include "ma/EqualLoudness.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

// Components of the Brain's window. They all draw from the processor's MixSnapshot,
// refreshed by the editor's timer.
namespace brain
{

inline juce::Colour channelColour(int index)
{
    const float hue = std::fmod(0.58f + 0.618034f * static_cast<float>(index), 1.0f);
    return juce::Colour::fromHSV(hue, 0.55f, 0.95f, 1.0f);
}

inline juce::Colour statusColour(MixSnapshot::Status status)
{
    namespace colours = ma::ui::colours;
    switch (status)
    {
        case MixSnapshot::Status::good: return colours::good;
        case MixSnapshot::Status::warning: return colours::warning;
        case MixSnapshot::Status::problem: return colours::critical;
        case MixSnapshot::Status::unknown: break;
    }
    return colours::quiet;
}

// The correctness report: one line per check, with a coloured status dot.
class ReportView : public juce::Component
{
public:
    void setItems(const std::vector<MixSnapshot::ReportItem>& newItems)
    {
        items = newItems;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        namespace colours = ma::ui::colours;
        auto area = getLocalBounds().toFloat();
        g.setColour(colours::panel);
        g.fillRoundedRectangle(area, 4.0f);
        area.reduce(10.0f, 8.0f);

        for (const auto& item : items)
        {
            juce::AttributedString text;
            text.append(item.detail, juce::FontOptions(12.5f), colours::quiet);
            juce::TextLayout layout;
            layout.createLayout(text, area.getWidth() - 16.0f);

            auto row = area.removeFromTop(19.0f + layout.getHeight());
            if (row.getBottom() > static_cast<float>(getHeight()))
                break;

            g.setColour(statusColour(item.status));
            g.fillEllipse(row.getX(), row.getY() + 5.0f, 9.0f, 9.0f);
            row.removeFromLeft(16.0f);

            g.setColour(colours::text);
            g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
            g.drawText(item.title, row.removeFromTop(19.0f), juce::Justification::centredLeft);
            layout.draw(g, row);
            area.removeFromTop(10.0f);
        }
    }

private:
    std::vector<MixSnapshot::ReportItem> items;
};

// Table of connected Agents. Clicking a row highlights that channel everywhere.
class ChannelTable : public juce::Component
{
public:
    std::function<void(std::uint64_t)> onSelect;

    void setChannels(const std::vector<MixSnapshot::Channel>& newChannels, std::uint64_t newSelected)
    {
        channels = newChannels;
        selected = newSelected;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        namespace colours = ma::ui::colours;
        using ma::ui::pl;
        auto area = getLocalBounds().toFloat();
        g.setColour(colours::panel);
        g.fillRoundedRectangle(area, 4.0f);
        area.reduce(8.0f, 4.0f);

        const auto columns = [](juce::Rectangle<float> r) {
            std::array<juce::Rectangle<float>, 7> c;
            c[0] = r.removeFromLeft(r.getWidth() * 0.30f);
            c[1] = r.removeFromLeft(r.getWidth() * 0.22f);
            for (size_t i = 2; i < c.size(); ++i)
                c[i] = r.removeFromLeft(r.getWidth() / static_cast<float>(c.size() - i));
            return c;
        };

        g.setFont(juce::FontOptions(12.0f));
        g.setColour(colours::quiet);
        const juce::String headings[] = { pl("Kanał"), pl("Rola"), pl("Prio"), pl("Poziom"), pl("Szczyt"), "Trim", pl("Nauka") };
        auto header = columns(area.removeFromTop(rowHeight));
        for (size_t i = 0; i < header.size(); ++i)
            g.drawText(headings[i], header[i], juce::Justification::centredLeft);

        if (channels.empty())
        {
            g.drawText(pl("Brak Agentów. Wstaw Mix Assistant Agent na kanały miksera FL."), area,
                       juce::Justification::centred);
            return;
        }

        const auto roles = ma::ui::roleNames();
        firstRowY = area.getY();
        g.setFont(juce::FontOptions(13.0f));
        for (size_t index = 0; index < channels.size(); ++index)
        {
            auto row = area.removeFromTop(rowHeight);
            if (row.getBottom() > getHeight())
                break;
            const auto& channel = channels[index];
            const auto& d = channel.data;
            if (channel.owner == selected)
            {
                g.setColour(colours::accent.withAlpha(0.18f));
                g.fillRect(row);
            }

            auto c = columns(row);
            g.setColour(channelColour(static_cast<int>(index)));
            g.fillRect(c[0].removeFromLeft(4.0f).reduced(0.0f, 3.0f));
            c[0].removeFromLeft(6.0f);

            const bool lufs = d.measureLufs != 0;
            const double level = lufs ? d.inputLufs : d.inputRmsDb;
            g.setColour(colours::text);
            g.drawText(channel.name, c[0], juce::Justification::centredLeft, true);
            g.drawText(roles[juce::jlimit(0, roles.size() - 1, static_cast<int>(d.role))], c[1], juce::Justification::centredLeft, true);
            g.drawText(juce::String(d.priority), c[2], juce::Justification::centredLeft);
            g.drawText(level > -150.0 ? juce::String(level, 1) + (lufs ? " LUFS" : " dB") : "-", c[3], juce::Justification::centredLeft);
            g.setColour(d.outputPeakDb > 0.0f ? colours::critical : colours::text);
            g.drawText(d.outputPeakDb > -150.0f ? juce::String(d.outputPeakDb, 1) : "-", c[4], juce::Justification::centredLeft);
            g.setColour(colours::text);
            g.drawText((d.trimDb >= 0.0f ? "+" : "") + juce::String(d.trimDb, 1), c[5], juce::Justification::centredLeft);
            g.setColour(d.hasProfile == 0 ? colours::quiet : d.trimLimited != 0 ? colours::warning : colours::good);
            g.drawText(d.hasProfile == 0 ? pl("brak") : d.trimLimited != 0 ? pl("limit") : "OK", c[6], juce::Justification::centredLeft);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const int index = static_cast<int>((static_cast<float>(e.y) - firstRowY) / rowHeight);
        if (e.y < firstRowY || index >= static_cast<int>(channels.size()) || onSelect == nullptr)
            return;
        const auto owner = channels[static_cast<size_t>(index)].owner;
        onSelect(owner == selected ? 0 : owner);
    }

private:
    static constexpr float rowHeight = 20.0f;
    std::vector<MixSnapshot::Channel> channels;
    std::uint64_t selected = 0;
    float firstRowY = 0.0f;
};

// Every channel's spectrum as the ear hears it, over the master's, plus a strip that
// shows how many channels fight for each band.
class OverlayView : public juce::Component
{
public:
    static constexpr double floorDb = -84.0;

    void setData(const MixSnapshot& mix, std::uint64_t newSelected, const double* masterDb, const double* masterHz, int masterBands)
    {
        channels = mix.channels;
        crowding = mix.masking.crowding;
        selected = newSelected;
        numMaster = juce::jmin(masterBands, static_cast<int>(master.size()));
        for (int b = 0; b < numMaster; ++b)
        {
            const double hz = masterHz[b];
            masterCentres[static_cast<size_t>(b)] = hz;
            master[static_cast<size_t>(b)] = masterDb[b] - ma::equalLoudnessWeightDb(hz);
        }
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        namespace colours = ma::ui::colours;
        using ma::ui::pl;
        auto area = getLocalBounds().toFloat();
        g.setColour(colours::panel);
        g.fillRoundedRectangle(area, 4.0f);

        auto plot = area.reduced(8.0f, 6.0f);
        const auto labels = plot.removeFromBottom(14.0f);
        auto strip = plot.removeFromBottom(12.0f);
        plot.removeFromBottom(4.0f);

        const auto xOf = [&plot](double hz) {
            return plot.getX() + static_cast<float>(plot.getWidth() * std::log(hz / 20.0) / std::log(1000.0));
        };
        const auto yOf = [&plot](double db) {
            const double proportion = std::isfinite(db) ? juce::jlimit(0.0, 1.0, db / floorDb) : 1.0;
            return plot.getY() + static_cast<float>(plot.getHeight() * proportion);
        };

        g.setColour(colours::quiet.withAlpha(0.2f));
        for (double db = 0.0; db >= floorDb; db -= 12.0)
            g.drawHorizontalLine(juce::roundToInt(yOf(db)), plot.getX(), plot.getRight());

        // Master, filled in the background.
        if (numMaster > 1)
        {
            juce::Path fill;
            fill.startNewSubPath(xOf(masterCentres[0]), plot.getBottom());
            for (int b = 0; b < numMaster; ++b)
                fill.lineTo(xOf(masterCentres[static_cast<size_t>(b)]), yOf(master[static_cast<size_t>(b)]));
            fill.lineTo(xOf(masterCentres[static_cast<size_t>(numMaster - 1)]), plot.getBottom());
            fill.closeSubPath();
            g.setColour(colours::quiet.withAlpha(0.18f));
            g.fillPath(fill);
        }

        const auto& weights = ma::erbEqualLoudnessWeights();
        const auto drawChannel = [&](size_t index, bool highlighted) {
            const auto& channel = channels[index];
            juce::Path line;
            for (int b = 0; b < ma::bus::kBands; ++b)
            {
                const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
                const juce::Point<float> p { xOf(hz), yOf(channel.bandsDb[static_cast<size_t>(b)] - weights[static_cast<size_t>(b)]) };
                if (b == 0)
                    line.startNewSubPath(p);
                else
                    line.lineTo(p);
            }
            const auto colour = channelColour(static_cast<int>(index));
            const bool dimmed = selected != 0 && !highlighted;
            g.setColour(dimmed ? colour.withAlpha(0.25f) : colour);
            g.strokePath(line, juce::PathStrokeType(highlighted ? 3.0f : 1.6f));
        };
        for (size_t c = 0; c < channels.size(); ++c)
            if (channels[c].spectrumValid && channels[c].owner != selected)
                drawChannel(c, false);
        for (size_t c = 0; c < channels.size(); ++c)
            if (channels[c].spectrumValid && channels[c].owner == selected)
                drawChannel(c, true);

        // Crowding: how many channels sit within 6 dB of the loudest one in each band.
        for (int b = 0; b < static_cast<int>(crowding.size()); ++b)
        {
            const int count = crowding[static_cast<size_t>(b)];
            const float x0 = xOf(ma::SpectrumAnalyzer::bandEdgeHz(ma::BandScale::erb, b));
            const float x1 = xOf(ma::SpectrumAnalyzer::bandEdgeHz(ma::BandScale::erb, b + 1));
            const auto colour = count <= 1 ? colours::panel.brighter(0.15f) : count == 2 ? colours::accent.withAlpha(0.5f)
                                : count == 3 ? colours::warning : colours::critical;
            g.setColour(colour);
            g.fillRect(juce::jmax(x0, plot.getX()), strip.getY(), juce::jmax(1.0f, juce::jmin(x1, plot.getRight()) - x0 - 1.0f), strip.getHeight());
        }

        g.setColour(colours::quiet);
        g.setFont(juce::FontOptions(10.0f));
        for (double hz : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        {
            const auto text = hz >= 1000.0 ? juce::String(juce::roundToInt(hz / 1000.0)) + "k" : juce::String(juce::roundToInt(hz));
            g.drawText(text, juce::Rectangle<float>(xOf(hz) - 16.0f, labels.getY(), 32.0f, 14.0f), juce::Justification::centred);
        }
        g.drawText(pl("pasek: ile kanałów walczy o pasmo"), plot.withTrimmedLeft(plot.getWidth() - 220.0f).removeFromTop(14.0f),
                   juce::Justification::centredRight);
    }

private:
    std::vector<MixSnapshot::Channel> channels;
    std::vector<int> crowding;
    std::uint64_t selected = 0;
    std::array<double, ma::SpectrumAnalyzer::kNumBands> master {}, masterCentres {};
    int numMaster = 0;
};

// Collisions in plain words, most important first. Clicking one highlights the masker.
class IssuesView : public juce::Component
{
public:
    std::function<void(std::uint64_t)> onSelect;

    struct Line
    {
        juce::String text;
        float score = 0.0f;
        std::uint64_t masker = 0;
        bool involvesSelected = false;
    };

    void setLines(std::vector<Line> newLines)
    {
        lines = std::move(newLines);
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        namespace colours = ma::ui::colours;
        using ma::ui::pl;
        auto area = getLocalBounds().toFloat();
        g.setColour(colours::panel);
        g.fillRoundedRectangle(area, 4.0f);
        area.reduce(10.0f, 6.0f);

        g.setFont(juce::FontOptions(13.0f));
        if (lines.empty())
        {
            g.setColour(colours::quiet);
            g.drawText(pl("Brak kolizji, w których mniej ważny kanał zasłania ważniejszy."), area, juce::Justification::topLeft);
            return;
        }
        for (const auto& line : lines)
        {
            auto row = area.removeFromTop(rowHeight);
            if (row.getBottom() > getHeight())
                break;
            if (line.involvesSelected)
            {
                g.setColour(colours::accent.withAlpha(0.18f));
                g.fillRect(row);
            }
            g.setColour(line.score >= 0.5f ? colours::critical : colours::warning);
            g.fillEllipse(row.getX(), row.getCentreY() - 4.0f, 8.0f, 8.0f);
            g.setColour(colours::text);
            g.drawText(line.text, row.withTrimmedLeft(16.0f), juce::Justification::centredLeft, true);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const int index = static_cast<int>((static_cast<float>(e.y) - 6.0f) / rowHeight);
        if (index >= 0 && index < static_cast<int>(lines.size()) && onSelect != nullptr)
            onSelect(lines[static_cast<size_t>(index)].masker);
    }

private:
    static constexpr float rowHeight = 20.0f;
    std::vector<Line> lines;
};

} // namespace brain
