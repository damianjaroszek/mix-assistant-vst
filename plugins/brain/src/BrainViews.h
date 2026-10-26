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
    std::array<double, ma::SpectrumAnalyzer::kMaxBands> master {}, masterCentres {};
    int numMaster = 0;
};

// The mix against the tonal target: corridor, its centre and the mix, holes in red,
// excesses in amber. Curves are normalised, so this shows shape, not level.
class TonalView : public juce::Component
{
public:
    void setData(const ma::TonalTarget& newTarget, const ma::TonalComparison& newComparison, const juce::String& newStatus)
    {
        target = newTarget;
        comparison = newComparison;
        status = newStatus;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        namespace colours = ma::ui::colours;
        auto area = getLocalBounds().toFloat();
        g.setColour(colours::panel);
        g.fillRoundedRectangle(area, 4.0f);
        auto plot = area.reduced(8.0f, 6.0f);
        const auto labels = plot.removeFromBottom(14.0f);

        const int n = ma::SpectrumAnalyzer::numBandsFor(ma::kTonalScale);
        const auto hz = [](int b) { return ma::SpectrumAnalyzer::bandCentreHz(ma::kTonalScale, b); };

        // Fit the curves: 6 dB grid, a little room above, at most 48 dB tall.
        double highest = 6.0, lowest = -12.0;
        for (int b = 0; b < n; ++b)
        {
            const auto i = static_cast<size_t>(b);
            if (target.valid())
            {
                highest = std::max(highest, target.high[i]);
                lowest = std::min(lowest, target.low[i]);
            }
            if (comparison.valid && comparison.mixDb[i] > -100.0)
            {
                highest = std::max(highest, comparison.mixDb[i]);
                lowest = std::min(lowest, comparison.mixDb[i]);
            }
        }
        const double top = std::ceil((highest + 4.0) / 6.0) * 6.0;
        const double bottom = std::max(top - 48.0, std::floor((lowest - 3.0) / 6.0) * 6.0);
        const auto xOf = [&plot](double f) {
            return plot.getX() + static_cast<float>(plot.getWidth() * std::log(f / 20.0) / std::log(1000.0));
        };
        const auto yOf = [&plot, top, bottom](double db) {
            return plot.getY() + static_cast<float>(plot.getHeight() * (top - juce::jlimit(bottom, top, db)) / (top - bottom));
        };
        g.setColour(colours::quiet.withAlpha(0.2f));
        for (double db = top - 6.0; db >= bottom; db -= 6.0)
            g.drawHorizontalLine(juce::roundToInt(yOf(db)), plot.getX(), plot.getRight());

        if (comparison.valid)
            for (const auto& region : comparison.regions)
            {
                g.setColour((region.hole() ? colours::critical : colours::warning).withAlpha(0.16f));
                g.fillRect(juce::Rectangle<float>::leftTopRightBottom(xOf(region.lowHz), plot.getY(), xOf(region.highHz), plot.getBottom()));
            }

        if (target.valid())
        {
            juce::Path corridor;
            corridor.startNewSubPath(xOf(hz(0)), yOf(target.high[0]));
            for (int b = 1; b < n; ++b)
                corridor.lineTo(xOf(hz(b)), yOf(target.high[static_cast<size_t>(b)]));
            for (int b = n - 1; b >= 0; --b)
                corridor.lineTo(xOf(hz(b)), yOf(target.low[static_cast<size_t>(b)]));
            corridor.closeSubPath();
            g.setColour(colours::accent.withAlpha(0.22f));
            g.fillPath(corridor);
            g.setColour(colours::accent.withAlpha(0.6f));
            g.strokePath(corridor, juce::PathStrokeType(1.0f));
            g.strokePath(line(target.centre, n, xOf, yOf, hz), juce::PathStrokeType(1.0f, juce::PathStrokeType::curved),
                         juce::AffineTransform());
        }
        if (comparison.valid)
        {
            g.setColour(colours::text);
            g.strokePath(line(comparison.mixDb, n, xOf, yOf, hz), juce::PathStrokeType(2.2f));
        }

        g.setColour(colours::quiet);
        g.setFont(juce::FontOptions(10.0f));
        for (double f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        {
            const auto text = f >= 1000.0 ? juce::String(juce::roundToInt(f / 1000.0)) + "k" : juce::String(juce::roundToInt(f));
            g.drawText(text, juce::Rectangle<float>(xOf(f) - 16.0f, labels.getY(), 32.0f, 14.0f), juce::Justification::centred);
        }
        g.setFont(juce::FontOptions(12.5f));
        g.drawText(status, plot.reduced(6.0f, 2.0f).removeFromTop(16.0f), juce::Justification::topLeft);
    }

private:
    template <typename X, typename Y, typename H>
    static juce::Path line(const ma::Curve& curve, int n, X xOf, Y yOf, H hz)
    {
        juce::Path p;
        bool started = false;
        for (int b = 0; b < n; ++b)
        {
            const double v = curve[static_cast<size_t>(b)];
            if (v < -100.0)
            {
                started = false;
                continue;
            }
            const juce::Point<float> point { xOf(hz(b)), yOf(v) };
            if (!started)
                p.startNewSubPath(point);
            else
                p.lineTo(point);
            started = true;
        }
        return p;
    }

    ma::TonalTarget target;
    ma::TonalComparison comparison;
    juce::String status;
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

// How each channel's share of the mix moves on other playback systems: one row per channel,
// one column per system. Around 0 dB the channel translates; far below it disappears there.
class TranslationView : public juce::Component
{
public:
    void setData(const MixSnapshot& mix)
    {
        rows.clear();
        for (size_t c = 0; c < mix.channels.size(); ++c)
        {
            Row row;
            row.name = mix.channels[c].name;
            row.valid = mix.translation.valid && c < mix.translation.shareChangeDb.size();
            if (row.valid)
                row.change = mix.translation.shareChangeDb[c];
            rows.push_back(row);
        }
        valid = mix.translation.valid;
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
        if (!valid)
        {
            g.setColour(colours::quiet);
            g.drawText(pl("Puść utwór: translacja liczy się z co najmniej dwóch grających kanałów."), area, juce::Justification::topLeft);
            return;
        }

        const float nameWidth = 150.0f;
        const int columns = ma::kPlaybackSystems - 1; // full range is the reference
        const float columnWidth = (area.getWidth() - nameWidth) / static_cast<float>(columns);
        auto header = area.removeFromTop(20.0f);
        g.setColour(colours::quiet);
        g.drawText(pl("Kanał"), header.removeFromLeft(nameWidth), juce::Justification::centredLeft);
        const juce::StringArray shortNames { {}, pl("Telefon"), pl("Laptop"), pl("Douszne"), pl("Samochód"), pl("Klub (mono)") };
        for (int s = 1; s <= columns; ++s)
            g.drawText(shortNames[s], header.removeFromLeft(columnWidth), juce::Justification::centred);
        area.removeFromTop(4.0f);

        for (size_t r = 0; r < rows.size(); ++r)
        {
            auto line = area.removeFromTop(22.0f);
            if (line.getBottom() > static_cast<float>(getHeight()))
                break;
            g.setColour(channelColour(static_cast<int>(r)));
            g.fillRect(line.getX(), line.getY() + 5.0f, 3.0f, 12.0f);
            g.setColour(colours::text);
            g.drawText(rows[r].name, line.removeFromLeft(nameWidth).withTrimmedLeft(8.0f), juce::Justification::centredLeft, true);
            for (int s = 1; s <= columns; ++s)
            {
                auto cell = line.removeFromLeft(columnWidth).reduced(4.0f, 2.0f);
                if (!rows[r].valid)
                    continue;
                const double db = rows[r].change[static_cast<size_t>(s)];
                const auto colour = db < -12.0 ? colours::critical : db < -6.0 ? colours::warning : db > 6.0 ? colours::warning : colours::good;
                g.setColour(colour.withAlpha(0.18f));
                g.fillRoundedRectangle(cell, 3.0f);
                g.setColour(colour);
                g.drawText((db >= 0.0 ? "+" : "") + juce::String(juce::roundToInt(db)) + " dB", cell, juce::Justification::centred);
            }
        }
        area.removeFromTop(6.0f);
        g.setColour(colours::quiet);
        g.drawFittedText(pl("Ile kanał traci albo zyskuje względem reszty miksu na danym systemie. Poniżej -12 dB prawie "
                            "znika. Posłuchaj sam: „Odsłuch” u góry."),
                         area.removeFromTop(36.0f).toNearestInt(), juce::Justification::topLeft, 2);
    }

private:
    struct Row
    {
        juce::String name;
        std::array<double, ma::kPlaybackSystems> change {};
        bool valid = false;
    };
    std::vector<Row> rows;
    bool valid = false;
};

// The solver's proposal: one entry per change with a checkbox, wrapped to the width. Click toggles it.
class ProposalView : public juce::Component
{
public:
    std::function<void(int)> onToggle;

    void setProposal(const Proposal& proposal)
    {
        items.clear();
        for (const auto& item : proposal.items)
            items.push_back({ item.text, item.selected, item.advisory, {} });
        made = proposal.made;
        layoutRows();
        repaint();
    }

    void resized() override { layoutRows(); }

    // Height that shows every entry, for the scrolling container.
    int contentHeight() const
    {
        return items.empty() ? 40 : juce::roundToInt(items.back().bounds.getBottom() + 6.0f);
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
        if (items.empty())
        {
            g.setColour(colours::quiet);
            g.drawText(made ? pl("Nic do poprawienia: brak kolizji, które warto ciąć, i zapas na mastering jest OK.")
                            : pl("Puść najgęstszy fragment utworu (drop) i kliknij „Zaproponuj”."),
                       area, juce::Justification::topLeft);
            return;
        }
        for (const auto& item : items)
        {
            if (item.bounds.getBottom() > static_cast<float>(getHeight()))
                break;
            const auto box = juce::Rectangle<float>(item.bounds.getX(), item.bounds.getY() + 3.0f, 12.0f, 12.0f);
            if (item.advisory)
            {
                g.setColour(colours::warning);
                g.drawText("i", box, juce::Justification::centred);
            }
            else
            {
                g.setColour(colours::quiet);
                g.drawRoundedRectangle(box, 2.0f, 1.2f);
                if (item.selected)
                {
                    g.setColour(colours::accent);
                    g.fillRoundedRectangle(box.reduced(2.5f), 1.5f);
                }
            }
            const auto colour = item.advisory ? colours::warning : item.selected ? colours::text : colours::quiet;
            layoutFor(item.text, colour, textWidth()).draw(g, item.bounds.withTrimmedLeft(textIndent));
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].bounds.contains(e.position) && onToggle != nullptr)
                onToggle(static_cast<int>(i));
    }

private:
    struct Line
    {
        juce::String text;
        bool selected = true;
        bool advisory = false;
        juce::Rectangle<float> bounds;
    };

    static constexpr float textIndent = 22.0f;
    static constexpr float gap = 5.0f;

    float textWidth() const { return juce::jmax(50.0f, static_cast<float>(getWidth()) - 20.0f - textIndent); }

    static juce::TextLayout layoutFor(const juce::String& text, juce::Colour colour, float width)
    {
        juce::AttributedString attributed;
        attributed.append(text, juce::FontOptions(13.0f), colour);
        attributed.setWordWrap(juce::AttributedString::byWord);
        juce::TextLayout layout;
        layout.createLayout(attributed, width);
        return layout;
    }

    void layoutRows()
    {
        float y = 6.0f;
        for (auto& item : items)
        {
            const float height = juce::jmax(18.0f, layoutFor(item.text, juce::Colours::white, textWidth()).getHeight());
            item.bounds = { 10.0f, y, static_cast<float>(getWidth()) - 20.0f, height };
            y += height + gap;
        }
    }

    std::vector<Line> items;
    bool made = false;
};

// The engineer's verdict per channel ("Ustaw cały miks"): a status dot, the one-line summary
// and why, wrapped to the width. Lives in a viewport, like the proposal.
class VerdictView : public juce::Component
{
public:
    std::function<void(std::uint64_t)> onSelect;

    void setVerdicts(const std::vector<Proposal::Verdict>& newVerdicts, bool made, std::uint64_t newSelected)
    {
        verdicts = newVerdicts;
        wholeMix = made;
        selected = newSelected;
        layoutRows();
        repaint();
    }

    void resized() override { layoutRows(); }
    int contentHeight() const { return rows.empty() ? 60 : juce::roundToInt(rows.back().getBottom() + 8.0f); }

    void paint(juce::Graphics& g) override
    {
        namespace colours = ma::ui::colours;
        using ma::ui::pl;
        auto area = getLocalBounds().toFloat();
        g.setColour(colours::panel);
        g.fillRoundedRectangle(area, 4.0f);
        area.reduce(10.0f, 6.0f);
        if (verdicts.empty())
        {
            g.setColour(colours::quiet);
            g.setFont(juce::FontOptions(13.0f));
            g.drawFittedText(wholeMix ? pl("Żaden kanał nie gra. Puść najgęstszy fragment i kliknij „Ustaw cały miks” jeszcze raz.")
                                      : pl("Kliknij „Ustaw cały miks”: Mózg ustawi porządki, kompresję, clipper i bas na każdym kanale, "
                                           "patrząc na cały miks, i powie tu, co i dlaczego. Najpierw naucz poziom w Agentach."),
                             area.toNearestInt(), juce::Justification::topLeft, 4);
            return;
        }
        for (size_t i = 0; i < verdicts.size(); ++i)
        {
            const auto& verdict = verdicts[i];
            auto row = rows[i];
            if (verdict.owner == selected)
            {
                g.setColour(colours::accent.withAlpha(0.12f));
                g.fillRoundedRectangle(row.expanded(4.0f, 2.0f), 3.0f);
            }
            g.setColour(statusColour(verdict.status));
            g.fillEllipse(row.getX(), row.getY() + 5.0f, 9.0f, 9.0f);
            row.removeFromLeft(16.0f);
            layoutFor(verdict, row.getWidth()).draw(g, row);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i].contains(e.position) && onSelect != nullptr)
                onSelect(verdicts[i].owner);
    }

private:
    static juce::TextLayout layoutFor(const Proposal::Verdict& verdict, float width)
    {
        namespace colours = ma::ui::colours;
        juce::AttributedString text;
        text.append(verdict.name, juce::FontOptions(14.0f, juce::Font::bold), colours::text);
        text.append("  " + verdict.role + "\n", juce::FontOptions(12.5f), colours::quiet);
        text.append(verdict.summary + "\n", juce::FontOptions(13.0f), statusColour(verdict.status));
        text.append(verdict.reasons.joinIntoString("\n"), juce::FontOptions(12.5f), colours::quiet);
        text.setWordWrap(juce::AttributedString::byWord);
        juce::TextLayout layout;
        layout.createLayout(text, juce::jmax(50.0f, width));
        return layout;
    }

    void layoutRows()
    {
        rows.clear();
        float y = 8.0f;
        const float width = static_cast<float>(getWidth()) - 20.0f;
        for (const auto& verdict : verdicts)
        {
            const float height = layoutFor(verdict, width - 16.0f).getHeight();
            rows.push_back({ 10.0f, y, width, height });
            y += height + 12.0f;
        }
    }

    std::vector<Proposal::Verdict> verdicts;
    std::vector<juce::Rectangle<float>> rows;
    bool wholeMix = false;
    std::uint64_t selected = 0;
};

// Phase 9: drums, bass, vocals and the rest against the stem references, drop to drop:
// level against the whole mix (corridor and our dot), shape per region, width.
class StemView : public juce::Component
{
public:
    void setData(const ma::StemTarget& newTarget, const ma::StemComparison& newComparison, const juce::String& newStatus,
                 bool newWarning, const juce::String& newKeyStatus, bool newKeyWarning)
    {
        target = newTarget;
        comparison = newComparison;
        status = newStatus;
        warning = newWarning;
        keyStatus = newKeyStatus;
        keyWarning = newKeyWarning;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        namespace colours = ma::ui::colours;
        using ma::ui::pl;
        auto area = getLocalBounds().toFloat();
        g.setColour(colours::panel);
        g.fillRoundedRectangle(area, 4.0f);
        area = area.reduced(8.0f, 6.0f);

        g.setFont(juce::FontOptions(12.5f));
        g.setColour(warning ? colours::warning : colours::quiet);
        g.drawFittedText(status, area.removeFromTop(32.0f).toNearestInt(), juce::Justification::topLeft, 2);
        // Phase 10: the track's key and how far each reference was moved to it.
        if (keyStatus.isNotEmpty())
        {
            g.setColour(keyWarning ? colours::warning : colours::quiet);
            g.drawFittedText(keyStatus, area.removeFromTop(32.0f).toNearestInt(), juce::Justification::topLeft, 2);
        }
        if (!target.valid())
            return;

        constexpr float nameWidth = 90.0f, levelWidth = 190.0f, widthWidth = 92.0f;
        const float regionWidth = (area.getWidth() - nameWidth - levelWidth - widthWidth - 12.0f) / ma::kStemRegions;
        const char* const regions[ma::kStemRegions] { "sub", "bas", "dół-środek", "środek", "prezencja", "powietrze" };

        auto header = area.removeFromTop(16.0f);
        g.setFont(juce::FontOptions(10.5f));
        g.setColour(colours::quiet);
        header.removeFromLeft(nameWidth);
        g.drawText(pl("poziom wobec całości (dB)"), header.removeFromLeft(levelWidth), juce::Justification::centredLeft);
        header.removeFromLeft(6.0f);
        for (int r = 0; r < ma::kStemRegions; ++r)
            g.drawText(pl(regions[r]), header.removeFromLeft(regionWidth), juce::Justification::centred);
        header.removeFromLeft(6.0f);
        g.drawText(pl("szerokość"), header, juce::Justification::centred);

        const float rowHeight = std::min(34.0f, area.getHeight() / ma::kStemGroups);
        constexpr double lowest = -30.0, highest = 0.0;
        for (int group = 0; group < ma::kStemGroups; ++group)
        {
            auto row = area.removeFromTop(rowHeight).reduced(0.0f, 2.0f);
            const auto& c = comparison.groups[static_cast<size_t>(group)];
            const auto& corridor = target.level[static_cast<size_t>(group)];

            g.setFont(juce::FontOptions(12.5f));
            g.setColour(corridor.valid ? colours::text : colours::quiet);
            static const char* const names[ma::kStemGroups] { "Perkusja", "Bas", "Wokal", "Reszta" };
            g.drawText(pl(names[group]), row.removeFromLeft(nameWidth), juce::Justification::centredLeft);

            // Level: the references' corridor, our dot.
            auto level = row.removeFromLeft(levelWidth);
            auto bar = level.withSizeKeepingCentre(level.getWidth() - 40.0f, 8.0f).withX(level.getX());
            const auto xOf = [&bar, lowest, highest](double db) {
                return bar.getX() + static_cast<float>(bar.getWidth() * (juce::jlimit(lowest, highest, db) - lowest) / (highest - lowest));
            };
            g.setColour(colours::background);
            g.fillRoundedRectangle(bar, 3.0f);
            if (corridor.valid)
            {
                g.setColour(colours::accent.withAlpha(0.5f));
                g.fillRoundedRectangle(juce::Rectangle<float>::leftTopRightBottom(xOf(corridor.low), bar.getY(), xOf(corridor.high), bar.getBottom()), 3.0f);
            }
            g.setFont(juce::FontOptions(10.5f));
            if (c.compared)
            {
                const auto colour = std::abs(c.levelDeviationDb) < 0.5 ? colours::good
                                    : std::abs(c.levelDeviationDb) < 3.0 ? colours::warning : colours::critical;
                g.setColour(colour);
                g.fillEllipse(juce::Rectangle<float>(10.0f, 10.0f).withCentre({ xOf(c.levelDb), bar.getCentreY() }));
                g.drawText(juce::String(c.levelDb, 1), level.removeFromRight(36.0f), juce::Justification::centredRight);
            }
            else
            {
                g.setColour(colours::quiet);
                g.drawText(c.missing ? pl("brak") : "-", level.removeFromRight(36.0f), juce::Justification::centredRight);
            }
            row.removeFromLeft(6.0f);

            // Shape per region: green inside the references, amber or red outside.
            for (int r = 0; r < ma::kStemRegions; ++r)
            {
                auto cell = row.removeFromLeft(regionWidth).reduced(2.0f, 0.0f);
                const bool compared = c.compared && c.shapeCompared[static_cast<size_t>(r)];
                const double d = c.shapeDeviationDb[static_cast<size_t>(r)];
                auto colour = !compared ? colours::background
                              : std::abs(d) < 1.5 ? colours::good.withAlpha(0.35f)
                              : std::abs(d) < 3.0 ? colours::warning.withAlpha(0.45f) : colours::critical.withAlpha(0.55f);
                g.setColour(colour);
                g.fillRoundedRectangle(cell, 3.0f);
                g.setColour(compared ? colours::text : colours::quiet);
                g.drawText(!compared ? juce::String("-") : std::abs(d) < 1.5 ? juce::String("ok")
                                                                              : (d > 0.0 ? "+" : "") + juce::String(d, 1),
                           cell, juce::Justification::centred);
            }
            row.removeFromLeft(6.0f);

            // Width: side share, ours against the references.
            const auto& w = target.width[static_cast<size_t>(group)];
            g.setColour(c.widthCompared && std::abs(c.widthDeviation) >= 0.04 ? colours::warning : colours::quiet);
            const auto widthText = c.widthCompared ? juce::String(juce::roundToInt(c.width * 100.0)) + "%" : juce::String("-");
            const auto refText = w.valid ? " (" + juce::String(juce::roundToInt(w.low * 100.0)) + pl("–")
                                               + juce::String(juce::roundToInt(w.high * 100.0)) + "%)"
                                         : juce::String();
            g.drawText(widthText + refText, row, juce::Justification::centred);
        }
    }

private:
    ma::StemTarget target;
    ma::StemComparison comparison;
    juce::String status, keyStatus;
    bool warning = false, keyWarning = false;
};

} // namespace brain
