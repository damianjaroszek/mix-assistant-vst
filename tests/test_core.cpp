// Unit tests for the DSP core. Plain asserts, no framework, so they build anywhere.

#include "ma/Decibels.h"
#include "ma/LevelStats.h"
#include "ma/LoudnessMeter.h"
#include "ma/SpectrumAnalyzer.h"
#include "ma/TruePeakMeter.h"
#include "ma/Wav.h"

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
    testWavRoundTrip();

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "OK" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
