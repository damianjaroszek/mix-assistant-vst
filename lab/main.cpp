// ma-lab: offline analysis of WAV stems.
//
//   ma-lab [--target-rms -18] [--json] stem1.wav stem2.wav ...
//
// Prints loudness, peaks, RMS, crest factor and the gain-staging trim for each
// file. With more than one file it also analyses their sum, i.e. the mix.

#include "ma/Decibels.h"
#include "ma/LevelStats.h"
#include "ma/LoudnessMeter.h"
#include "ma/SpectrumAnalyzer.h"
#include "ma/TruePeakMeter.h"
#include "ma/Wav.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

namespace
{

struct Report
{
    std::string name;
    double seconds = 0.0;
    double integratedLufs = 0.0;
    double maxShortTermLufs = 0.0;
    double truePeakDb = 0.0;
    double samplePeakDb = 0.0;
    double rmsDb = 0.0;
    double crestDb = 0.0;
    double trimDb = 0.0;
    ma::SpectrumAnalyzer::Bands bandsDb {};
};

Report analyse(const std::string& name, const ma::AudioData& audio, double targetRmsDb)
{
    constexpr int blockSize = 512;

    ma::LoudnessMeter loudness;
    ma::TruePeakMeter truePeak;
    ma::LevelStats levels;
    ma::SpectrumAnalyzer spectrum;
    loudness.prepare(audio.sampleRate, audio.numChannels());
    truePeak.prepare(audio.numChannels());
    spectrum.prepare(audio.sampleRate);

    std::vector<const float*> pointers(static_cast<size_t>(audio.numChannels()));
    double maxShortTerm = ma::kMinusInfinityDb;

    for (int start = 0; start < audio.numSamples(); start += blockSize)
    {
        const int n = std::min(blockSize, audio.numSamples() - start);
        for (int c = 0; c < audio.numChannels(); ++c)
            pointers[static_cast<size_t>(c)] = audio.channels[static_cast<size_t>(c)].data() + start;

        loudness.process(pointers.data(), audio.numChannels(), n);
        truePeak.process(pointers.data(), audio.numChannels(), n);
        levels.process(pointers.data(), audio.numChannels(), n);
        spectrum.process(pointers.data(), audio.numChannels(), n);
        maxShortTerm = std::max(maxShortTerm, loudness.shortTermLufs());
    }

    Report r;
    r.name = name;
    r.seconds = audio.numSamples() / audio.sampleRate;
    r.integratedLufs = loudness.integratedLufs();
    r.maxShortTermLufs = maxShortTerm;
    r.truePeakDb = truePeak.truePeakDb();
    r.samplePeakDb = truePeak.samplePeakDb();
    r.rmsDb = levels.rmsDb();
    r.crestDb = levels.crestDb();
    r.trimDb = ma::suggestTrimDb(r.rmsDb, targetRmsDb);
    r.bandsDb = spectrum.averageDb();
    return r;
}

ma::AudioData sumOf(const std::vector<ma::AudioData>& stems)
{
    ma::AudioData mix;
    mix.sampleRate = stems.front().sampleRate;
    int channels = 0, samples = 0;
    for (const auto& s : stems)
    {
        channels = std::max(channels, s.numChannels());
        samples = std::max(samples, s.numSamples());
    }
    mix.channels.assign(static_cast<size_t>(channels), std::vector<float>(static_cast<size_t>(samples), 0.0f));

    for (const auto& s : stems)
    {
        for (int c = 0; c < channels; ++c)
        {
            // A mono stem feeds every channel of the mix.
            const auto& src = s.channels[static_cast<size_t>(std::min(c, s.numChannels() - 1))];
            auto& dst = mix.channels[static_cast<size_t>(c)];
            for (size_t i = 0; i < src.size(); ++i)
                dst[i] += src[i];
        }
    }
    return mix;
}

std::string fmt(double v)
{
    if (!std::isfinite(v))
        return "-inf";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", v);
    return buf;
}

std::string json(double v)
{
    if (!std::isfinite(v))
        return "null";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    return buf;
}

std::string escape(const std::string& s)
{
    std::string out;
    for (char ch : s)
    {
        if (ch == '"' || ch == '\\')
            out += '\\';
        out += ch;
    }
    return out;
}

void printTable(const std::vector<Report>& reports, double targetRmsDb)
{
    std::printf("Target RMS for gain staging: %.1f dBFS\n\n", targetRmsDb);
    std::printf("%-28s %7s %8s %8s %8s %8s %7s %7s\n",
                "file", "sec", "LUFS-I", "LUFS-S", "dBTP", "RMS", "crest", "trim");
    for (const auto& r : reports)
    {
        std::string name = r.name.size() > 28 ? "..." + r.name.substr(r.name.size() - 25) : r.name;
        std::printf("%-28s %7.1f %8s %8s %8s %8s %7s %+7.1f\n",
                    name.c_str(), r.seconds, fmt(r.integratedLufs).c_str(), fmt(r.maxShortTermLufs).c_str(),
                    fmt(r.truePeakDb).c_str(), fmt(r.rmsDb).c_str(), fmt(r.crestDb).c_str(), r.trimDb);
    }
    std::printf("\nLUFS-S is the loudest 3 s window. trim = gain that brings the gated RMS to the target.\n");
}

void printJson(const std::vector<Report>& reports, double targetRmsDb)
{
    std::printf("{\n  \"targetRmsDb\": %s,\n  \"bandCentresHz\": [", json(targetRmsDb).c_str());
    for (int b = 0; b < ma::SpectrumAnalyzer::kNumBands; ++b)
        std::printf("%s%.1f", b ? ", " : "", ma::SpectrumAnalyzer::bandCentreHz(b));
    std::printf("],\n  \"files\": [\n");

    for (size_t i = 0; i < reports.size(); ++i)
    {
        const auto& r = reports[i];
        std::printf("    {\"name\": \"%s\", \"seconds\": %s, \"integratedLufs\": %s, \"maxShortTermLufs\": %s, "
                    "\"truePeakDb\": %s, \"samplePeakDb\": %s, \"rmsDb\": %s, \"crestDb\": %s, \"trimDb\": %s, \"bandsDb\": [",
                    escape(r.name).c_str(), json(r.seconds).c_str(), json(r.integratedLufs).c_str(),
                    json(r.maxShortTermLufs).c_str(), json(r.truePeakDb).c_str(), json(r.samplePeakDb).c_str(),
                    json(r.rmsDb).c_str(), json(r.crestDb).c_str(), json(r.trimDb).c_str());
        for (int b = 0; b < ma::SpectrumAnalyzer::kNumBands; ++b)
            std::printf("%s%s", b ? ", " : "", json(r.bandsDb[static_cast<size_t>(b)]).c_str());
        std::printf("]}%s\n", i + 1 < reports.size() ? "," : "");
    }
    std::printf("  ]\n}\n");
}

int usage()
{
    std::fprintf(stderr, "usage: ma-lab [--target-rms dBFS] [--json] file.wav [file2.wav ...]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    double targetRmsDb = -18.0;
    bool asJson = false;
    std::vector<std::string> paths;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--json")
            asJson = true;
        else if (arg == "--target-rms" && i + 1 < argc)
            targetRmsDb = std::atof(argv[++i]);
        else if (arg == "-h" || arg == "--help")
            return usage();
        else
            paths.push_back(arg);
    }
    if (paths.empty())
        return usage();

    try
    {
        std::vector<ma::AudioData> stems;
        std::vector<Report> reports;
        for (const auto& path : paths)
        {
            stems.push_back(ma::readWav(path));
            reports.push_back(analyse(path, stems.back(), targetRmsDb));
        }

        if (stems.size() > 1)
        {
            const bool sameRate = std::all_of(stems.begin(), stems.end(),
                                              [&](const ma::AudioData& s) { return s.sampleRate == stems.front().sampleRate; });
            if (sameRate)
                reports.push_back(analyse("SUM (mix)", sumOf(stems), targetRmsDb));
            else
                std::fprintf(stderr, "note: files have different sample rates, skipping the sum\n");
        }

        if (asJson)
            printJson(reports, targetRmsDb);
        else
            printTable(reports, targetRmsDb);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
