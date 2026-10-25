// Unit tests for the DSP core. Plain asserts, no framework, so they build anywhere.

#include "ma/ChannelAnalyzer.h"
#include "ma/ChannelEq.h"
#include "ma/Solver.h"
#include "ma/TonalBalance.h"
#include "ma/Decibels.h"
#include "ma/Ducker.h"
#include "ma/EqualLoudness.h"
#include "ma/GainStaging.h"
#include "ma/LevelStats.h"
#include "ma/Masking.h"
#include "ma/MixBus.h"
#include "ma/CorrelationMeter.h"
#include "ma/LoudnessMeter.h"
#include "ma/SpectrumAnalyzer.h"
#include "ma/TruePeakMeter.h"
#include "ma/Wav.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void check(bool ok, const std::string& what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok)
        ++failures;
}

void checkNear(double actual, double expected, double tolerance, const std::string& what)
{
    char buf[128];
    std::snprintf(buf, sizeof buf, " (got %.3f, want %.3f +/- %.3f)", actual, expected, tolerance);
    check(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, what + buf);
}

constexpr double kPi = 3.14159265358979323846;

ma::AudioData sine(double sampleRate, double seconds, int channels, double hz, double amplitude, double phase = 0.0)
{
    ma::AudioData a;
    a.sampleRate = sampleRate;
    const auto n = static_cast<size_t>(sampleRate * seconds);
    a.channels.assign(static_cast<size_t>(channels), std::vector<float>(n));
    for (size_t i = 0; i < n; ++i)
    {
        const auto v = static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * i / sampleRate + phase));
        for (auto& ch : a.channels)
            ch[i] = v;
    }
    return a;
}

// Feeds the audio in host-sized blocks.
void run(const ma::AudioData& a, const std::function<void(const float* const*, int, int)>& process)
{
    std::vector<const float*> p(a.channels.size());
    for (int start = 0; start < a.numSamples(); start += 512)
    {
        const int n = std::min(512, a.numSamples() - start);
        for (size_t c = 0; c < p.size(); ++c)
            p[c] = a.channels[c].data() + start;
        process(p.data(), a.numChannels(), n);
    }
}

void testLoudness()
{
    // EBU Tech 3341 case 1: stereo 1 kHz sine at -23 dBFS reads -23.0 LUFS.
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        const auto a = sine(rate, 20.0, 2, 1000.0, ma::dbToGain(-23.0));
        ma::LoudnessMeter m;
        m.prepare(rate, 2);
        run(a, [&](const float* const* c, int nc, int n) { m.process(c, nc, n); });
        const std::string at = " @ " + std::to_string(static_cast<int>(rate)) + " Hz";
        checkNear(m.integratedLufs(), -23.0, 0.1, "integrated LUFS of -23 dBFS stereo sine" + at);
        checkNear(m.shortTermLufs(), -23.0, 0.1, "short-term LUFS of -23 dBFS stereo sine" + at);
        checkNear(m.momentaryLufs(), -23.0, 0.1, "momentary LUFS of -23 dBFS stereo sine" + at);
    }

    // EBU Tech 3341 case 3 style: -36 / -23 / -36 dBFS sections, relative gate drops the quiet parts.
    {
        const double rate = 48000.0;
        ma::AudioData a;
        a.sampleRate = rate;
        a.channels.assign(2, {});
        for (auto [seconds, db] : { std::pair { 10.0, -36.0 }, std::pair { 60.0, -23.0 }, std::pair { 10.0, -36.0 } })
        {
            const auto part = sine(rate, seconds, 2, 1000.0, ma::dbToGain(db));
            for (size_t c = 0; c < 2; ++c)
                a.channels[c].insert(a.channels[c].end(), part.channels[c].begin(), part.channels[c].end());
        }
        ma::LoudnessMeter m;
        m.prepare(rate, 2);
        run(a, [&](const float* const* c, int nc, int n) { m.process(c, nc, n); });
        checkNear(m.integratedLufs(), -23.0, 0.1, "relative gate ignores quiet sections");
    }

    {
        ma::LoudnessMeter m;
        m.prepare(48000.0, 2);
        const auto silence = sine(48000.0, 5.0, 2, 1000.0, 0.0);
        run(silence, [&](const float* const* c, int nc, int n) { m.process(c, nc, n); });
        check(!std::isfinite(m.integratedLufs()), "silence has no integrated loudness");
    }
}

void testTruePeak()
{
    {
        const auto a = sine(48000.0, 1.0, 2, 997.0, 0.5);
        ma::TruePeakMeter m;
        m.prepare(2);
        run(a, [&](const float* const* c, int nc, int n) { m.process(c, nc, n); });
        checkNear(m.truePeakDb(), -6.02, 0.1, "true peak of a 0.5 amplitude sine");
    }
    {
        // Sine at fs/4 with 45 degree phase: samples hit only 0.707, the waveform reaches 1.0.
        const auto a = sine(48000.0, 1.0, 1, 12000.0, 1.0, kPi / 4.0);
        ma::TruePeakMeter m;
        m.prepare(1);
        run(a, [&](const float* const* c, int nc, int n) { m.process(c, nc, n); });
        checkNear(m.samplePeakDb(), -3.01, 0.05, "sample peak misses the inter-sample peak");
        checkNear(m.truePeakDb(), 0.0, 0.5, "true peak finds the inter-sample peak");
    }
}

void testSpectrum()
{
    const auto a = sine(48000.0, 5.0, 2, 1000.0, 0.5);
    ma::SpectrumAnalyzer s;
    s.prepare(48000.0);
    run(a, [&](const float* const* c, int nc, int n) { s.process(c, nc, n); });
    const auto bands = s.averageDb();

    checkNear(ma::SpectrumAnalyzer::bandCentreHz(17), 1000.0, 0.01, "band 17 is centred on 1 kHz");
    checkNear(ma::SpectrumAnalyzer::bandCentreHz(0), 19.7, 0.1, "band 0 is centred near 20 Hz");
    checkNear(bands[17], ma::gainToDb(0.5 / std::sqrt(2.0)), 0.5, "1 kHz sine energy lands in the 1 kHz band");
    check(bands[14] < bands[17] - 60.0 && bands[20] < bands[17] - 60.0, "one octave away the band is 60 dB lower");
}

void testLevelsAndTrim()
{
    const auto a = sine(48000.0, 2.0, 2, 440.0, ma::dbToGain(-12.0));
    ma::LevelStats l;
    run(a, [&](const float* const* c, int nc, int n) { l.process(c, nc, n); });
    checkNear(l.rmsDb(), -15.01, 0.05, "RMS of a -12 dBFS peak sine");
    checkNear(l.crestDb(), 3.01, 0.05, "crest factor of a sine");
    checkNear(ma::suggestTrimDb(l.rmsDb(), -18.0), -2.99, 0.05, "trim to -18 dBFS RMS");
    checkNear(ma::suggestTrimDb(-60.0, -18.0), 24.0, 1e-9, "trim is limited to +24 dB");
    checkNear(ma::suggestTrimDb(ma::kMinusInfinityDb, -18.0), 0.0, 1e-9, "no trim for silence");

    ma::LevelStats gated;
    const auto quiet = sine(48000.0, 2.0, 2, 440.0, ma::dbToGain(-80.0));
    run(quiet, [&](const float* const* c, int nc, int n) { gated.process(c, nc, n); });
    run(a, [&](const float* const* c, int nc, int n) { gated.process(c, nc, n); });
    checkNear(gated.rmsDb(), -15.01, 0.05, "gate ignores near-silent passages");
}

void testErbSpectrum()
{
    checkNear(ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, 0), 33.5, 2.0, "lowest ERB band sits just above 20 Hz");
    check(ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, 39) < 20000.0, "highest ERB band stays below 20 kHz");
    checkNear(ma::SpectrumAnalyzer::bandEdgeHz(ma::BandScale::erb, 0), 20.0, 0.01, "ERB bands start at 20 Hz");
    checkNear(ma::SpectrumAnalyzer::bandEdgeHz(ma::BandScale::erb, 40), 20000.0, 0.5, "ERB bands end at 20 kHz");
    checkNear(ma::SpectrumAnalyzer::bandEdgeHz(ma::BandScale::thirdOctave, 17) * std::pow(2.0, 1.0 / 6.0), 1000.0, 0.01,
              "third-octave edges sit a sixth of an octave from the centre");

    // ERB bands are narrow in Hz at the bottom and wide at the top, like the ear.
    const double lowWidth = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, 1) - ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, 0);
    const double highWidth = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, 39) - ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, 38);
    check(highWidth > 20.0 * lowWidth, "ERB bands widen with frequency");

    const auto a = sine(48000.0, 5.0, 2, 1000.0, 0.5);
    ma::SpectrumAnalyzer s;
    s.prepare(48000.0, ma::BandScale::erb);
    run(a, [&](const float* const* c, int nc, int n) { s.process(c, nc, n); });
    const auto bands = s.averageDb();
    double total = 0.0;
    int peakBand = 0;
    for (int b = 0; b < s.numBands(); ++b)
    {
        total += std::pow(10.0, bands[static_cast<size_t>(b)] / 10.0);
        if (bands[static_cast<size_t>(b)] > bands[static_cast<size_t>(peakBand)])
            peakBand = b;
    }
    check(s.centreHz(peakBand) > 850.0 && s.centreHz(peakBand) < 1150.0, "1 kHz sine peaks in the ERB band around 1 kHz");
    checkNear(10.0 * std::log10(total), ma::gainToDb(0.5 / std::sqrt(2.0)), 0.2, "ERB bands add up to the RMS level");
}

void testEqualLoudness()
{
    checkNear(ma::equalLoudnessSpl(1000.0, 40.0), 40.0, 0.2, "1 kHz at 40 phon is 40 dB SPL");
    checkNear(ma::equalLoudnessSpl(1000.0, 80.0), 80.0, 0.2, "1 kHz at 80 phon is 80 dB SPL");
    checkNear(ma::equalLoudnessWeightDb(1000.0), 0.0, 1e-9, "no weighting at 1 kHz");
    check(ma::equalLoudnessWeightDb(50.0) > 15.0, "the ear hears 50 Hz much quieter than 1 kHz");
    check(ma::equalLoudnessWeightDb(3150.0) < 0.0, "the ear is most sensitive around 3 kHz");
    check(ma::equalLoudnessWeightDb(50.0, 40.0) > ma::equalLoudnessWeightDb(50.0, 80.0), "bass drops away more at low listening levels");
}

void testTrimPlan()
{
    auto plan = ma::planTrim(-24.0, -10.0, -18.0, -1.0);
    check(plan.valid && !plan.limitedByPeak, "plain trim is not limited");
    checkNear(plan.trimDb, 6.0, 1e-9, "trim brings -24 to -18");

    plan = ma::planTrim(-24.0, -4.0, -18.0, -1.0);
    check(plan.limitedByPeak, "trim is limited when the peak would cross the ceiling");
    checkNear(plan.trimDb, 3.0, 1e-9, "limited trim leaves the true peak at the ceiling");

    plan = ma::planTrim(ma::kMinusInfinityDb, ma::kMinusInfinityDb, -18.0, -1.0);
    check(!plan.valid && plan.trimDb == 0.0, "silence gives no trim");
}

void testChannelAnalyzerSpeed()
{
    // The Agent's whole measurement chain on 60 s of stereo noise at 48 kHz.
    const double rate = 48000.0;
    ma::AudioData a;
    a.sampleRate = rate;
    a.channels.assign(2, std::vector<float>(static_cast<size_t>(rate * 60.0)));
    unsigned state = 12345u;
    for (auto& ch : a.channels)
        for (auto& v : ch)
        {
            state = state * 1664525u + 1013904223u;
            v = static_cast<float>(state >> 8) / 16777216.0f - 0.5f;
        }

    ma::ChannelAnalyzer analyzer;
    analyzer.prepare(rate, 2);
    const auto start = std::chrono::steady_clock::now();
    run(a, [&](const float* const* c, int nc, int n) { analyzer.process(c, nc, n); });
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double realtime = 60.0 / seconds;
    std::printf("INFO  Agent analysis runs %.0fx faster than real time (40 Agents learning at once use about %.1f%% of one core)\n",
                realtime, 4000.0 / realtime);
#ifdef NDEBUG
    check(realtime > 100.0, "Agent analysis is fast enough for 40 channels");
#endif
    check(std::isfinite(analyzer.integratedLufs()) && analyzer.gatedSeconds() > 59.0, "analyser measured the whole passage");
}

void testMixBus()
{
    auto region = std::make_unique<ma::bus::Region>();
    std::memset(static_cast<void*>(region.get()), 0, sizeof(ma::bus::Region));
    ma::bus::initialise(*region);
    check(region->header.magic.load() == ma::bus::kMagic, "shared region initialises");

    const int a = ma::bus::claimSlot(*region, 111, 1000);
    const int b = ma::bus::claimSlot(*region, 222, 1000);
    check(a >= 0 && b >= 0 && a != b, "two Agents get two slots");
    check(ma::bus::claimSlot(*region, 111, 1100) == a, "an Agent gets its own slot back");

    ma::bus::AgentData data {};
    std::strncpy(data.name, "Kick", sizeof(data.name) - 1);
    data.priority = 1;
    data.liveDb[3] = -12.5f;
    ma::bus::publish(region->slots[a], data, 1200);

    ma::bus::AgentData out {};
    check(ma::bus::read(region->slots[a], out, 1300), "Brain reads a published slot");
    check(std::string(out.name) == "Kick" && out.priority == 1 && out.liveDb[3] == -12.5f, "slot data arrives intact");
    check(!ma::bus::read(region->slots[a], out, 1200 + ma::bus::kAliveTimeoutMs + 1), "a silent Agent drops out");

    // An abandoned slot (crashed instance) is reused after the timeout.
    const int c = ma::bus::claimSlot(*region, 333, 1200 + ma::bus::kAliveTimeoutMs + 500);
    check(c == a || c == b, "abandoned slots are reclaimed");

    ma::bus::releaseSlot(*region, b, 222);
    check(region->slots[b].owner.load() == 0 || c == b, "released slot is free");

    ma::bus::Command command {};
    check(!ma::bus::readCommand(region->slots[a], command), "no command before the Brain sends one");
    command.target = 333;
    command.serial = 7;
    command.kind = ma::bus::CommandKind::apply;
    command.trimDb = -2.5f;
    command.eq[1] = { 250.0f, -3.0f, 1.4f };
    ma::bus::sendCommand(region->slots[a], command);
    ma::bus::Command received {};
    check(ma::bus::readCommand(region->slots[a], received), "Agent reads the Brain's command");
    check(received.target == 333 && received.serial == 7 && received.kind == ma::bus::CommandKind::apply
              && received.trimDb == -2.5f && received.eq[1].hz == 250.0f && received.eq[1].gainDb == -3.0f,
          "command arrives intact");
}

ma::MaskingChannel band(double lowHz, double highHz, double db, int priority)
{
    ma::MaskingChannel ch;
    ch.priority = priority;
    ch.valid = true;
    ch.rawDb.fill(-120.0);
    for (int b = 0; b < ma::SpectrumAnalyzer::numBandsFor(ma::BandScale::erb); ++b)
    {
        const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
        if (hz >= lowHz && hz <= highHz)
            ch.rawDb[static_cast<size_t>(b)] = db;
    }
    return ch;
}

void testMasking()
{
    // Kick (priority 1) at 45-90 Hz, a louder bass (priority 2) at 45-250 Hz, hi-hats up top.
    const std::vector<ma::MaskingChannel> channels { band(45.0, 90.0, -14.0, 1), band(45.0, 250.0, -6.0, 2),
                                                     band(8000.0, 14000.0, -24.0, 3) };
    const auto r = ma::analyseMasking(channels);

    check(r.amount[1][0] > 0.6, "loud bass masks the kick");
    check(r.amount[0][1] < r.amount[1][0], "the kick masks the bass less than the other way round");
    check(r.amount[2][0] < 0.05 && r.amount[0][2] < 0.05, "hi-hats and kick do not mask each other");
    check(!r.issues.empty() && r.issues.front().masker == 1 && r.issues.front().masked == 0, "top issue: bass covers kick");
    if (!r.issues.empty())
        check(r.issues.front().lowHz < 60.0 && r.issues.front().highHz > 70.0, "issue names the kick's low band");

    bool reverse = false;
    for (const auto& issue : r.issues)
        reverse = reverse || (issue.masker == 0 && issue.masked == 1);
    check(!reverse, "a more important channel masking a less important one is not an issue");
}

void testEq()
{
    const auto bell = ma::Biquad::peaking(48000.0, 1000.0, -6.0, 1.0);
    checkNear(bell.magnitudeDb(1000.0, 48000.0), -6.0, 0.01, "bell reaches its gain at the centre");
    check(std::abs(bell.magnitudeDb(100.0, 48000.0)) < 0.2, "bell leaves frequencies far below alone");

    ma::EqSettings eq {};
    eq[0] = { 1000.0f, -6.0f, 1.0f };
    checkNear(ma::eqResponseDb(eq, 1000.0), -6.0, 0.01, "EQ response sums its bands");
    checkNear(ma::eqResponseDb(ma::EqSettings {}, 1000.0), 0.0, 1e-9, "an empty EQ is flat");

    // Real-time EQ on a 1 kHz sine: -6 dB in, about half the amplitude out.
    auto s = sine(48000.0, 1.0, 2, 1000.0, 0.5);
    ma::ChannelEq realtime;
    realtime.prepare(48000.0);
    realtime.setBands(eq);
    float* channels[] = { s.channels[0].data(), s.channels[1].data() };
    realtime.process(channels, 2, s.numSamples());
    float peak = 0.0f;
    for (int i = s.numSamples() / 2; i < s.numSamples(); ++i)
        peak = std::max(peak, std::abs(s.channels[0][static_cast<size_t>(i)]));
    checkNear(peak, 0.5 * std::pow(10.0, -6.0 / 20.0), 0.005, "real-time EQ cuts the sine by 6 dB");
}

ma::SolverChannel solverBand(double lowHz, double highHz, double db, int priority)
{
    const auto m = band(lowHz, highHz, db, priority);
    ma::SolverChannel c;
    c.rawDb = m.rawDb;
    c.priority = m.priority;
    c.valid = true;
    return c;
}

void testSolver()
{
    // Kick (1) masked by a louder bass (2); vocal (3) masked by a pad (6); hats stay clear.
    const std::vector<ma::SolverChannel> channels { solverBand(45.0, 90.0, -14.0, 1), solverBand(45.0, 250.0, -8.0, 2),
                                                    solverBand(300.0, 3000.0, -20.0, 3), solverBand(200.0, 2500.0, -14.0, 6),
                                                    solverBand(8000.0, 14000.0, -24.0, 5) };
    const auto r = ma::solve(channels);

    bool bassCut = false, padCut = false, wrongWay = false;
    for (const auto& change : r.changes)
    {
        if (change.kind != ma::SolverChange::Kind::eqCut)
            continue;
        const auto& band = r.eq[static_cast<size_t>(change.channel)][static_cast<size_t>(change.eqSlot)];
        check(band.gainDb < 0.0f && band.gainDb >= -6.0f, "solver cuts within the limit");
        check(change.scoreAfter < change.scoreBefore, "every proposed cut reduces its collision");
        bassCut = bassCut || (change.channel == 1 && change.maskedChannel == 0 && band.hz < 120.0f);
        padCut = padCut || (change.channel == 3 && change.maskedChannel == 2 && band.hz > 250.0f && band.hz < 3000.0f);
        wrongWay = wrongWay || channels[static_cast<size_t>(change.channel)].priority < channels[static_cast<size_t>(change.maskedChannel)].priority;
    }
    check(bassCut, "solver cuts the bass under the kick");
    check(padCut, "solver cuts the pad where the vocal lives");
    check(!wrongWay, "solver never cuts the more important channel");
    check(r.crowdingAfter < r.crowdingBefore, "collisions go down overall");
    for (const auto& band : r.eq[4])
        check(band.gainDb == 0.0f, "a channel without collisions is left alone");

    const auto again = ma::solve(channels);
    bool same = again.changes.size() == r.changes.size();
    for (size_t c = 0; c < channels.size() && same; ++c)
        for (int b = 0; b < ma::kEqBands; ++b)
            same = same && again.eq[c][static_cast<size_t>(b)].gainDb == r.eq[c][static_cast<size_t>(b)].gainDb;
    check(same, "solving again gives the same proposal");

    ma::SolverOptions headroom;
    headroom.headroomGainDb = -2.5;
    const auto withGain = ma::solve(channels, headroom);
    check(!withGain.changes.empty() && withGain.changes.front().kind == ma::SolverChange::Kind::globalGain
              && withGain.changes.front().gainDb == -2.5,
          "missing headroom becomes one gain change for all channels");
}

ma::Curve mixOf(const std::vector<ma::SolverChannel>& channels)
{
    ma::Curve mix {};
    for (int b = 0; b < ma::SpectrumAnalyzer::numBandsFor(ma::BandScale::erb); ++b)
    {
        double power = 0.0;
        for (const auto& c : channels)
            power += std::pow(10.0, c.rawDb[static_cast<size_t>(b)] / 10.0);
        mix[static_cast<size_t>(b)] = 10.0 * std::log10(power);
    }
    return mix;
}

void testTonalBalance()
{
    const int n = ma::SpectrumAnalyzer::numBandsFor(ma::BandScale::erb);
    ma::Curve slope {};
    for (int b = 0; b < n; ++b)
        slope[static_cast<size_t>(b)] = -20.0 - 4.0 * std::log2(ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b) / 100.0);

    const auto norm = ma::normaliseCurve(slope);
    double mean = 0.0;
    int count = 0;
    for (int b = 0; b < n; ++b)
    {
        const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
        if (hz >= 60.0 && hz <= 8000.0)
        {
            mean += norm[static_cast<size_t>(b)];
            ++count;
        }
    }
    checkNear(mean / count, 0.0, 1e-9, "normalised curve sits at 0 dB over 60 Hz..8 kHz");

    ma::TonalTarget target;
    target.name = "House";
    target.references.push_back(norm);
    target.rebuild();
    check(target.high[20] - target.low[20] >= 4.0 - 1e-9, "one reference gets a corridor of at least +-2 dB");

    // The same slope 12 dB quieter, with a 6 dB hole around 2-4 kHz.
    ma::Curve mix = slope;
    for (int b = 0; b < n; ++b)
    {
        mix[static_cast<size_t>(b)] -= 12.0;
        const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
        if (hz >= 2000.0 && hz <= 4000.0)
            mix[static_cast<size_t>(b)] -= 6.0;
    }
    const auto cmp = ma::compareToTarget(mix, target);
    check(cmp.valid && !cmp.regions.empty(), "a hole is found");
    if (!cmp.regions.empty())
    {
        check(cmp.regions.front().hole() && cmp.regions.front().lowHz >= 1500.0 && cmp.regions.front().highHz <= 5000.0,
              "the hole is reported where it is, regardless of overall level");
        check(cmp.regions.front().deviationDb < -2.0, "the hole is a few dB under the corridor");
    }
    const auto same = ma::compareToTarget(slope, target);
    check(same.valid && same.regions.empty(), "the reference itself fits its own target");

    ma::TonalTarget parsed;
    check(ma::parseTarget(ma::serialiseTarget(target), parsed), "target file parses");
    check(parsed.name == "House" && parsed.references.size() == 1 && std::abs(parsed.centre[10] - target.centre[10]) < 0.01,
          "target survives a save and load");

    std::string crlf;
    for (char ch : ma::serialiseTarget(target))
        crlf += ch == '\n' ? std::string("\r\n") : std::string(1, ch);
    ma::TonalTarget windows;
    check(ma::parseTarget(crlf, windows) && windows.name == "House" && windows.references.size() == 1,
          "a target file with Windows line endings loads too");
}

void testTonalSolver()
{
    // Kick, a vocal across 300 Hz-5 kHz and hats at 5-7 kHz; nothing above 9 kHz.
    std::vector<ma::SolverChannel> channels { solverBand(45.0, 90.0, -14.0, 1), solverBand(300.0, 5000.0, -24.0, 2),
                                              solverBand(5000.0, 7000.0, -30.0, 5) };
    const auto mix = mixOf(channels);

    // Target: the same mix with 6 dB more at 2-4.5 kHz and real content up to 14 kHz.
    ma::Curve reference = mix;
    for (int b = 0; b < ma::SpectrumAnalyzer::numBandsFor(ma::BandScale::erb); ++b)
    {
        const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
        if (hz >= 2000.0 && hz <= 4500.0)
            reference[static_cast<size_t>(b)] += 6.0;
        if (hz >= 9000.0 && hz <= 14000.0)
            reference[static_cast<size_t>(b)] = -36.0;
    }
    ma::TonalTarget target;
    target.references.push_back(ma::normaliseCurve(reference));
    target.rebuild();
    const auto cmp = ma::compareToTarget(mix, target);

    ma::SolverOptions options;
    options.tonal = &cmp;
    const auto r = ma::solve(channels, options);

    bool boostVocal = false, advice = false;
    for (const auto& change : r.changes)
    {
        if (change.kind == ma::SolverChange::Kind::tonalBoost)
        {
            const auto& band = r.eq[static_cast<size_t>(change.channel)][static_cast<size_t>(change.eqSlot)];
            boostVocal = boostVocal || (change.channel == 1 && band.gainDb > 0.0f && band.gainDb <= 4.0f && band.hz > 1500.0f && band.hz < 5000.0f);
        }
        if (change.kind == ma::SolverChange::Kind::tonalAdvice)
            advice = advice || (change.lowHz >= 7000.0 && (change.sourceChannel == 1 || change.sourceChannel == 2));
    }
    check(boostVocal, "a hole in 2-4.5 kHz is filled by boosting the vocal, which has content there");
    check(advice, "a hole nothing can fill becomes advice, naming a channel with content an octave below");
}

void testCorrelation()
{
    const auto a = sine(48000.0, 3.0, 2, 60.0, 0.5);
    ma::CorrelationMeter mono;
    mono.prepare(48000.0);
    mono.process(a.channels[0].data(), a.channels[1].data(), a.numSamples());
    checkNear(mono.lowBand(), 1.0, 0.01, "identical channels are mono in the low band");
    checkNear(mono.fullBand(), 1.0, 0.01, "identical channels are mono full band");

    std::vector<float> inverted(a.channels[1]);
    for (auto& v : inverted)
        v = -v;
    ma::CorrelationMeter flipped;
    flipped.prepare(48000.0);
    flipped.process(a.channels[0].data(), inverted.data(), a.numSamples());
    checkNear(flipped.lowBand(), -1.0, 0.01, "polarity-flipped bass reads -1");

    // Wide highs, mono lows: the low band still reads mono.
    const auto lowPart = sine(48000.0, 3.0, 1, 50.0, 0.5);
    const auto highL = sine(48000.0, 3.0, 1, 5000.0, 0.3);
    const auto highR = sine(48000.0, 3.0, 1, 5000.0, 0.3, kPi / 2.0);
    std::vector<float> l(lowPart.channels[0]), rr(lowPart.channels[0]);
    for (size_t i = 0; i < l.size(); ++i)
    {
        l[i] += highL.channels[0][i];
        rr[i] += highR.channels[0][i];
    }
    ma::CorrelationMeter wide;
    wide.prepare(48000.0);
    wide.process(l.data(), rr.data(), static_cast<int>(l.size()));
    check(wide.lowBand() > 0.95, "wide highs do not spoil the low-band reading");
    check(wide.fullBand() < wide.lowBand(), "full band shows the width");
}

// RMS of a stretch of samples, in dBFS.
double rmsDb(const std::vector<float>& x, size_t from, size_t to)
{
    double sum = 0.0;
    for (size_t i = from; i < to; ++i)
        sum += static_cast<double>(x[i]) * x[i];
    return 10.0 * std::log10(std::max(sum / static_cast<double>(to - from), 1e-20));
}

void testDucker()
{
    constexpr double sr = 48000.0;
    const size_t n = static_cast<size_t>(sr * 2.0);
    // Sidechain: a 55 Hz "kick" for 80 ms at the start of every 500 ms beat.
    std::vector<float> kick(n, 0.0f);
    for (size_t i = 0; i < n; ++i)
        if (i % 24000 < 3840)
            kick[i] = static_cast<float>(0.8 * std::sin(2.0 * kPi * 55.0 * i / sr));
    const auto bassSource = sine(sr, 2.0, 1, 55.0, 0.5);
    const auto hatSource = sine(sr, 2.0, 1, 4000.0, 0.5);

    auto duck = [&](const std::vector<float>& input, const ma::DuckSettings& s, bool withSidechain) {
        std::vector<float> out(input);
        ma::Ducker d;
        d.prepare(sr);
        d.setSettings(s);
        for (size_t start = 0; start < n; start += 512)
        {
            const int count = static_cast<int>(std::min<size_t>(512, n - start));
            float* main[] = {out.data() + start};
            const float* side[] = {kick.data() + start};
            d.process(main, 1, withSidechain ? side : nullptr, withSidechain ? 1 : 0, count);
        }
        return out;
    };

    ma::DuckSettings band;
    band.on = 1.0f;
    band.hz = 55.0f;
    band.depthDb = 9.0f;
    band.thresholdDb = -30.0f;
    band.releaseMs = 60.0f;

    const auto bass = duck(bassSource.channels[0], band, true);
    // Second beat: kick at 24000..27840, gap afterwards.
    const double during = rmsDb(bass, 25000, 27500) - rmsDb(bassSource.channels[0], 25000, 27500);
    const double between = rmsDb(bass, 40000, 47000) - rmsDb(bassSource.channels[0], 40000, 47000);
    checkNear(during, -9.0, 1.5, "band duck pulls the bass down by its depth while the kick plays");
    checkNear(between, 0.0, 0.5, "band duck lets the bass back between kicks");

    const auto hats = duck(hatSource.channels[0], band, true);
    checkNear(rmsDb(hats, 25000, 27500) - rmsDb(hatSource.channels[0], 25000, 27500), 0.0, 0.3,
              "band duck leaves content far from the band alone");

    auto whole = band;
    whole.wholeSignal = 1.0f;
    const auto wholeHats = duck(hatSource.channels[0], whole, true);
    checkNear(rmsDb(wholeHats, 25000, 27500) - rmsDb(hatSource.channels[0], 25000, 27500), -9.0, 1.5,
              "whole-signal duck turns everything down");

    const auto noSidechain = duck(bassSource.channels[0], band, false);
    check(noSidechain == bassSource.channels[0], "without a sidechain nothing is ducked");

    auto off = band;
    off.on = 0.0f;
    check(duck(bassSource.channels[0], off, true) == bassSource.channels[0], "duck switched off is bypassed");

    checkNear(ma::syncedReleaseMs(125.0, 1.0 / 16.0), 120.0, 0.01, "a sixteenth at 125 BPM is 120 ms");
}

void testWavRoundTrip()
{
    const auto a = sine(44100.0, 0.5, 2, 440.0, 0.25);
    ma::writeWav("roundtrip.wav", a);
    const auto b = ma::readWav("roundtrip.wav");
    check(b.sampleRate == 44100.0 && b.numChannels() == 2 && b.numSamples() == a.numSamples(), "WAV header round trip");
    check(b.channels[1][100] == a.channels[1][100], "WAV float samples round trip");
}

} // namespace

int main()
{
    testLoudness();
    testTruePeak();
    testSpectrum();
    testLevelsAndTrim();
    testErbSpectrum();
    testEqualLoudness();
    testTrimPlan();
    testChannelAnalyzerSpeed();
    testMixBus();
    testMasking();
    testEq();
    testSolver();
    testTonalBalance();
    testTonalSolver();
    testCorrelation();
    testDucker();
    testWavRoundTrip();

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "OK" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
