#pragma once

#include "Instrument.h"
#include "Key.h"
#include "LoudnessMeter.h"
#include "SpectrumAnalyzer.h"
#include "Stereo.h"

#include <array>
#include <string>
#include <vector>

namespace ma
{

// Phase 9: references from stems. The user separates a reference track (FL Studio's stem
// separator, Demucs, an online service) and loads the stems; each group's level, spectral
// shape and width relative to the whole is measured over the loudest part (the drop), and
// our mix is measured the same way, so they compare drop to drop. Everything is relative to
// the sum, so references of any loudness compare directly.
enum class StemGroup
{
    drums = 0,
    bass,
    vocals,
    music // everything else: leads, pads, chords, FX
};
constexpr int kStemGroups = 4;

const char* stemGroupKey(StemGroup group); // "drums", "bass", "vocals", "music" (file format)
constexpr int kStemFullMix = -1; // a file that is the whole track, for the separation check
constexpr int kStemUnknown = -2;

// Groups for a reference's stem files, from their names (Demucs, FL Studio, English and
// Polish words). The part every name shares (the track's title) is ignored first, so a
// title like "Bassline Dream" does not make every stem a bass.
std::vector<int> stemGroupsFromNames(const std::vector<std::string>& fileNames);
// The group a channel of ours counts in; -1 = left out (buses and sends would count twice).
int stemGroupForRole(Role role);

// Broad regions the shape is compared in: sub, bass, low mids, mids, presence, air.
constexpr int kStemRegions = 6;
double stemRegionEdgeHz(int edge); // edge 0..kStemRegions

// One group's numbers, relative to the sum of all groups.
struct StemGroupMeasure
{
    bool present = false;
    double levelDb = 0.0;                    // ear-weighted level against the sum
    std::array<double, kStemRegions> shapeDb {}; // its own level per region minus its average over the regions it plays in
    std::array<bool, kStemRegions> shapeValid {}; // the group plays there at all
    double width = 0.0;                      // side share of its power, 300 Hz..10 kHz (0 mono, 0.5 uncorrelated)
    bool widthValid = false;
};

struct StemMeasure
{
    std::array<StemGroupMeasure, kStemGroups> groups {};
    bool valid = false;
};

// From each group's level per ERB band as the analysers read it (the mid, (L+R)/2, dB;
// -200 = absent) and its side alone (width only).
StemMeasure measureStemGroups(const std::array<SpectrumAnalyzer::Bands, kStemGroups>& groupDb,
                              const std::array<SpectrumAnalyzer::Bands, kStemGroups>& sideDb,
                              const std::array<bool, kStemGroups>& present);

// One reference track.
struct StemProfile
{
    std::string name;
    StemMeasure measure;
    double dropSeconds = 0.0;
    double separationErrorDb = -1.0; // average distance of the stems' sum from the full mix, < 0 = not checked

    // Phase 10: its key (from the stems that have one; unknown when the guess was not sure)
    // and each group's spectrum over the drop, so the profile can move to another key.
    // Sets saved before phase 10 have no spectra and stay where they are.
    MusicalKey key;
    double keyCorrelation = 0.0;
    bool hasBands = false;
    std::array<SpectrumAnalyzer::Bands, kStemGroups> groupDb {}, sideDb {};
    int shiftSemitones = 0; // applied by keyedStemTarget, not saved

    // Phase 12: the break, the part without drums (the drums at least 15 dB under their drop level
    // while the rest still plays), measured the same way, and how much quieter it is than the drop.
    struct Section
    {
        bool valid = false;
        double seconds = 0.0;
        double belowDropDb = 0.0; // short-term loudness of the drop minus the break's
        StemMeasure measure;
        std::array<SpectrumAnalyzer::Bands, kStemGroups> groupDb {}, sideDb {};
    };
    Section breakdown;
};

// Stems that carry the key: bass, vocals and the rest. Drums do not (the kick's own pitch is
// a separate question).
bool stemGroupHasKey(int group);

// Two passes over the stems, block by block (100 ms): first the loudness of their sum, to
// find the drop, then the analysis of each group over the drop only. Allocates in prepare().
class StemProfiler
{
public:
    void prepare(double sampleRate, bool haveFullMix);
    int blockLength() const { return block; }

    // Pass 1: the stems' sum, one block at a time, and the sum of the stems with a key (bass,
    // vocals, the rest) for the key; nullptr = take the whole sum.
    void measureBlock(const float* left, const float* right, int numSamples, const float* keyLeft = nullptr,
                      const float* keyRight = nullptr);
    // Pass 1, phase 12: the drums stem's level of the same block, so the break can be found
    // (no drums stem, no break).
    void measureDrums(const float* left, const float* right, int numSamples);
    // Between the passes: the blocks within 3 dB of the loudest short-term loudness, and the
    // break (drums 15 dB under their drop level, the rest within 20 dB of the drop).
    void chooseDrop();
    bool inDrop(int blockIndex) const;
    bool inBreak(int blockIndex) const;
    bool needed(int blockIndex) const { return inDrop(blockIndex) || inBreak(blockIndex); }
    int blocks() const { return static_cast<int>(loudness.size()); }

    // Pass 2, the same blocks: each group's signal (nullptr = the group has no stem) and the
    // full mix if one was loaded. Blocks outside the drop and the break are skipped inside.
    void analyseBlock(int blockIndex, const std::array<const float*, kStemGroups>& left,
                      const std::array<const float*, kStemGroups>& right, const float* mixLeft, const float* mixRight,
                      int numSamples);
    StemProfile finish(const std::string& name) const;

private:
    double sampleRate = 48000.0;
    int block = 4800;
    bool fullMix = false;
    LoudnessMeter meter;
    KeyDetector keys;
    std::vector<double> loudness, drums;
    std::vector<char> drop, breakdown;
    std::array<StereoAnalyzer, kStemGroups> groups, breakGroups;
    std::array<bool, kStemGroups> seen {}, breakSeen {};
    int breakBlocks = 0;
    SpectrumAnalyzer sum, mix;
    std::vector<float> sumLeft, sumRight;
    int dropBlocks = 0;
};

// A set of references for one style: the corridor per number is their median, widened to
// their spread and never narrower than a fixed tolerance.
struct StemCorridor
{
    bool valid = false;
    double centre = 0.0, low = 0.0, high = 0.0;
    double outside(double value) const { return value < low ? value - low : (value > high ? value - high : 0.0); }
};

struct StemTarget
{
    std::string name;
    std::vector<StemProfile> profiles;

    std::array<StemCorridor, kStemGroups> level {};
    std::array<std::array<StemCorridor, kStemRegions>, kStemGroups> shape {};
    std::array<StemCorridor, kStemGroups> width {};
    StemCorridor breakContrast; // phase 12: how much quieter the references' break is than their drop (dB)

    void rebuild();
    bool valid() const { return !profiles.empty(); }
};

// Phase 12: the same set as its breaks (profiles without one left out), to compare with the
// break of the mix through the same functions.
StemTarget stemBreakTarget(const StemTarget& target);

// The set moved to the track's key: each profile with a known key and spectra is shifted (its
// groups with a key, the drums stay) and measured again; the corridors rebuilt. An unknown
// `trackKey` leaves it as it is.
StemTarget keyedStemTarget(const StemTarget& target, MusicalKey trackKey);

std::string serialiseStemTarget(const StemTarget& target);
bool parseStemTarget(const std::string& text, StemTarget& target);

// Our mix against the target, group by group.
struct StemComparison
{
    struct Group
    {
        bool compared = false;     // present in our mix and in the references
        bool missing = false;      // the references have it, our mix does not
        bool extra = false;        // our mix has it, the references do not (a break with the bass left in), at least -25 dB against the sum (ear-weighted, so a sub bass reads low)
        double levelDb = 0.0, levelDeviationDb = 0.0; // deviation = distance outside the corridor
        std::array<double, kStemRegions> shapeDeviationDb {};
        std::array<bool, kStemRegions> shapeCompared {};
        double width = 0.0, widthDeviation = 0.0;
        bool widthCompared = false;
    };
    std::array<Group, kStemGroups> groups {};
    bool valid = false;
};

StemComparison compareStems(const StemMeasure& mix, const StemTarget& target);

// Level per group to bring each one into its corridor (aiming half a dB inside it), the
// mix's overall loudness kept, at most 3 dB per pass. Works from the same group powers as
// measureStemGroups, so applying it and planning again gives no further change.
struct StemLevelPlan
{
    std::array<double, kStemGroups> gainDb {};
    std::array<double, kStemGroups> before {}, after {}; // level against the sum
    bool any = false;
};
StemLevelPlan planStemLevels(const std::array<SpectrumAnalyzer::Bands, kStemGroups>& groupDb,
                             const std::array<bool, kStemGroups>& present, const StemTarget& target);

// For the solver: our channels grouped, measured over the drop (pre Brain EQ, like its input).
struct StemShapeInput
{
    const StemTarget* target = nullptr;
    std::vector<int> groupOf;                       // per solver channel, -1 = no group
    std::vector<SpectrumAnalyzer::Bands> dropDb;    // per solver channel
    std::vector<SpectrumAnalyzer::Bands> dropSideDb; // per solver channel (width only, not changed by EQ)
    // Per solver channel: no boost in a region centred below this (the cleanup's high-pass
    // took that range out on purpose); 0 = anywhere.
    std::vector<double> lowestBoostHz;
};

} // namespace ma
