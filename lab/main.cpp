// ma-lab: offline analysis of WAV stems.
//
//   ma-lab [--target-rms -18] [--json] stem1.wav stem2.wav ...
//   ma-lab --make-target "House" --out House.matarget ref1.wav ref2.wav ...
//   ma-lab --add-stems "Tech house" --out "Tech house.mastems" drums.wav bass.wav vocals.wav other.wav [mix.wav]
//
// Prints loudness, peaks, RMS, crest factor and the gain-staging trim for each
// file. With more than one file it also analyses their sum, i.e. the mix.
// --make-target builds a tonal-balance target from reference tracks, the same file
// the Brain writes when it records references; copy it to the Brain's targets folder.
// --add-stems measures one reference track from its stems (the Brain's "Wczytaj stemy")
// and adds it to a stem reference set, creating the file if needed.

#include "ma/Decibels.h"
#include "ma/LevelStats.h"
#include "ma/LoudnessMeter.h"
#include "ma/SpectrumAnalyzer.h"
#include "ma/StemReference.h"
#include "ma/TonalBalance.h"
#include "ma/TruePeakMeter.h"
#include "ma/Wav.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
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
    std::fprintf(stderr, "usage: ma-lab [--target-rms dBFS] [--json] file.wav [file2.wav ...]\n"
                         "       ma-lab --make-target NAME --out FILE.matarget ref.wav [ref2.wav ...]\n"
                         "       ma-lab --add-stems SET --out FILE.mastems drums.wav bass.wav vocals.wav other.wav [mix.wav]\n");
    return 2;
}

ma::Curve tonalCurve(const ma::AudioData& audio)
{
    constexpr int blockSize = 512;
    ma::SpectrumAnalyzer spectrum;
    spectrum.prepare(audio.sampleRate, ma::kTonalScale);
    std::vector<const float*> pointers(static_cast<size_t>(audio.numChannels()));
    for (int start = 0; start < audio.numSamples(); start += blockSize)
    {
        const int n = std::min(blockSize, audio.numSamples() - start);
        for (int c = 0; c < audio.numChannels(); ++c)
            pointers[static_cast<size_t>(c)] = audio.channels[static_cast<size_t>(c)].data() + start;
        spectrum.process(pointers.data(), audio.numChannels(), n);
    }
    return ma::normaliseCurve(spectrum.averageDb());
}

int makeTarget(const std::string& name, const std::string& out, const std::vector<std::string>& paths)
{
    ma::TonalTarget target;
    target.name = name;
    for (const auto& path : paths)
    {
        target.references.push_back(tonalCurve(ma::readWav(path)));
        std::printf("reference: %s\n", path.c_str());
    }
    target.rebuild();
    std::ofstream file(out);
    file << ma::serialiseTarget(target);
    if (!file)
    {
        std::fprintf(stderr, "error: cannot write %s\n", out.c_str());
        return 1;
    }
    std::printf("target \"%s\" with %zu references written to %s\n", name.c_str(), target.references.size(), out.c_str());
    return 0;
}

int addStems(const std::string& name, const std::string& out, const std::vector<std::string>& paths)
{
    const auto groups = ma::stemGroupsFromNames(paths);
    std::vector<ma::AudioData> audio;
    std::vector<int> kinds;
    for (size_t i = 0; i < paths.size(); ++i)
    {
        if (groups[i] == ma::kStemUnknown)
        {
            std::fprintf(stderr, "error: cannot tell what %s is (name it drums/bass/vocals/other, or mix)\n", paths[i].c_str());
            return 1;
        }
        audio.push_back(ma::readWav(paths[i]));
        kinds.push_back(groups[i]);
        if (audio.back().sampleRate != audio.front().sampleRate)
        {
            std::fprintf(stderr, "error: different sample rates\n");
            return 1;
        }
    }
    int length = audio.front().numSamples();
    bool haveMix = false;
    for (size_t i = 0; i < audio.size(); ++i)
    {
        length = std::min(length, audio[i].numSamples());
        haveMix = haveMix || kinds[i] == ma::kStemFullMix;
    }

    ma::StemProfiler profiler;
    profiler.prepare(audio.front().sampleRate, haveMix);
    const int n = profiler.blockLength();
    std::array<std::vector<float>, ma::kStemGroups> left, right;
    std::vector<float> sumL, sumR, mixL, mixR;
    std::array<bool, ma::kStemGroups> has {};
    auto fill = [&](int start, int len) {
        for (auto* v : { &sumL, &sumR, &mixL, &mixR })
            v->assign(static_cast<size_t>(len), 0.0f);
        for (int g = 0; g < ma::kStemGroups; ++g)
        {
            left[static_cast<size_t>(g)].assign(static_cast<size_t>(len), 0.0f);
            right[static_cast<size_t>(g)].assign(static_cast<size_t>(len), 0.0f);
        }
        for (size_t i = 0; i < audio.size(); ++i)
        {
            const auto& a = audio[i];
            const auto& l = a.channels.front();
            const auto& r = a.numChannels() > 1 ? a.channels[1] : a.channels.front();
            const bool mix = kinds[i] == ma::kStemFullMix;
            for (int k = 0; k < len; ++k)
            {
                const auto at = static_cast<size_t>(start + k), to = static_cast<size_t>(k);
                if (mix)
                {
                    mixL[to] += l[at];
                    mixR[to] += r[at];
                    continue;
                }
                left[static_cast<size_t>(kinds[i])][to] += l[at];
                right[static_cast<size_t>(kinds[i])][to] += r[at];
                sumL[to] += l[at];
                sumR[to] += r[at];
            }
            if (!mix)
                has[static_cast<size_t>(kinds[i])] = true;
        }
    };
    for (int start = 0; start < length; start += n)
    {
        const int len = std::min(n, length - start);
        fill(start, len);
        profiler.measureBlock(sumL.data(), sumR.data(), len);
    }
    profiler.chooseDrop();
    int block = 0;
    for (int start = 0; start < length; start += n, ++block)
    {
        if (!profiler.inDrop(block))
            continue;
        const int len = std::min(n, length - start);
        fill(start, len);
        std::array<const float*, ma::kStemGroups> l {}, r {};
        for (size_t g = 0; g < l.size(); ++g)
            if (has[g])
            {
                l[g] = left[g].data();
                r[g] = right[g].data();
            }
        profiler.analyseBlock(block, l, r, haveMix ? mixL.data() : nullptr, haveMix ? mixR.data() : nullptr, len);
    }

    // The reference's title: what the file names share.
    std::string title = paths.front();
    for (const auto& p : paths)
    {
        size_t same = 0;
        while (same < title.size() && same < p.size() && title[same] == p[same])
            ++same;
        title.resize(same);
    }
    const auto slash = title.find_last_of("/\\");
    if (slash != std::string::npos)
        title = title.substr(slash + 1);
    while (!title.empty() && std::string(" -_.(").find(title.back()) != std::string::npos)
        title.pop_back();
    const auto profile = profiler.finish(title.empty() ? "reference" : title);

    ma::StemTarget target;
    {
        std::ifstream existing(out);
        if (existing)
        {
            const std::string text((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
            if (!ma::parseStemTarget(text, target))
            {
                std::fprintf(stderr, "error: %s is not a stem reference set\n", out.c_str());
                return 1;
            }
        }
    }
    target.name = name;
    target.profiles.push_back(profile);
    target.rebuild();
    std::ofstream(out) << ma::serialiseStemTarget(target);

    std::printf("\"%s\": drop %.0f s, separation %s\n", profile.name.c_str(), profile.dropSeconds,
                profile.separationErrorDb < 0.0 ? "not checked" : (std::to_string(profile.separationErrorDb).substr(0, 4) + " dB").c_str());
    const char* names[ma::kStemGroups] { "drums", "bass", "vocals", "music" };
    for (int g = 0; g < ma::kStemGroups; ++g)
    {
        const auto& m = profile.measure.groups[static_cast<size_t>(g)];
        if (!m.present)
        {
            std::printf("  %-7s -\n", names[g]);
            continue;
        }
        std::printf("  %-7s level %6.1f dB  width %3.0f%%  shape", names[g], m.levelDb, m.width * 100.0);
        for (int r = 0; r < ma::kStemRegions; ++r)
            if (m.shapeValid[static_cast<size_t>(r)])
                std::printf(" %+5.1f", m.shapeDb[static_cast<size_t>(r)]);
            else
                std::printf("     -");
        std::printf("\n");
    }
    std::printf("set \"%s\" with %zu references written to %s\n", name.c_str(), target.profiles.size(), out.c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    double targetRmsDb = -18.0;
    bool asJson = false;
    std::vector<std::string> paths;
    std::string targetName, targetOut, stemsName;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--json")
            asJson = true;
        else if (arg == "--make-target" && i + 1 < argc)
            targetName = argv[++i];
        else if (arg == "--add-stems" && i + 1 < argc)
            stemsName = argv[++i];
        else if (arg == "--out" && i + 1 < argc)
            targetOut = argv[++i];
        else if (arg == "--target-rms" && i + 1 < argc)
            targetRmsDb = std::atof(argv[++i]);
        else if (arg == "-h" || arg == "--help")
            return usage();
        else
            paths.push_back(arg);
    }
    if (paths.empty() || ((!targetName.empty() || !stemsName.empty()) && targetOut.empty()))
        return usage();

    try
    {
        if (!targetName.empty())
            return makeTarget(targetName, targetOut, paths);
        if (!stemsName.empty())
            return addStems(stemsName, targetOut, paths);

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
