// Unit tests for the DSP core. Plain asserts, no framework, so they build anywhere.

#include "ma/ChannelAnalyzer.h"
#include "ma/Decibels.h"
#include "ma/EqualLoudness.h"
#include "ma/GainStaging.h"
#include "ma/LevelStats.h"
#include "ma/LoudnessMeter.h"
#include "ma/SpectrumAnalyzer.h"
#include "ma/TruePeakMeter.h"
#include "ma/Wav.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
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
    testWavRoundTrip();

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "OK" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
