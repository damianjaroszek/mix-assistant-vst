#pragma once

#include "ChannelChain.h"
#include "Resonance.h"
#include "Stereo.h"
#include "ChannelEq.h"
#include "Ducker.h"
#include "DynamicEq.h"
#include "Pan.h"
#include "Depth.h"
#include "Alignment.h"
#include "Harmonics.h"
#include "Saturator.h"
#include "Transient.h"
#include "Unmask.h"

#include <atomic>
#include <cstdint>

// The data channel between Agents and the Brain.
//
// One fixed-size region lives in shared memory (a memory-mapped file the plugins open),
// so it works whether FL runs the plugins in its own process or bridged in separate ones.
// Each Agent owns one slot and is its only writer; the Brain only reads. Slots use a
// sequence lock: the writer makes the sequence odd while writing, readers retry if they
// saw an odd or changed sequence. Only slow data goes through here (levels, spectra,
// settings, a few times a second), never audio.
//
// The other direction, Brain to Agent, is one command per slot (apply corrections,
// undo them) with its own sequence lock; the Brain is its only writer.
namespace ma::bus
{

constexpr std::uint32_t kMagic = 0x4D414255; // "MABU"
constexpr std::uint32_t kVersion = 24; // a session per project: Brain entries in the header, AgentData::sessionId
constexpr int kMaxSlots = 128;
constexpr int kNameLength = 48;
constexpr int kBands = 40;            // ERB bands, as SpectrumAnalyzer with BandScale::erb
constexpr float kSilentDb = -200.0f;  // stands in for -inf in the shared data
constexpr std::uint64_t kAliveTimeoutMs = 2000;

static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "shared-memory atomics must be lock-free");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "shared-memory atomics must be lock-free");
static_assert(std::atomic<float>::is_always_lock_free, "shared-memory atomics must be lock-free");

// What an Agent publishes. Plain data, copied whole.
struct AgentData
{
    char name[kNameLength];
    std::int32_t role;
    std::int32_t priority;
    std::int32_t measureLufs; // 0 = RMS, 1 = LUFS
    std::int32_t hasProfile;
    std::int32_t trimLimited; // last learning pass hit the peak ceiling
    float trimDb;
    float targetDb;
    float inputRmsDb;
    float inputLufs;
    float outputPeakDb;
    float learnedDb;
    float learnedTruePeakDb;
    float liveDb[kBands];    // raw ERB band levels (dBFS), about the last second, pre-trim
    float profileDb[kBands]; // raw ERB band levels of the learned passage, pre-trim
    std::int32_t liveValid;
    EqBand eq[kEqBands];        // the Brain's corrections currently set on the Agent
    std::int32_t eqBypass;
    std::int32_t hasBackup;     // the last applied command can be undone
    std::uint32_t lastCommand;  // serial of the last command the Agent carried out
    DuckSettings duck;          // dynamic give-way driven by the sidechain input
    std::int32_t duckSync;      // release synced to tempo: 0 = off, else a note length (see duckSyncFraction)
    std::int32_t polarityInverted;
    std::int32_t sidechainConnected; // signal arrived on the sidechain input in the last seconds
    float sidechainDb;          // sidechain peak level, about the last second
    float duckReductionDb;      // most the duck took off, about the last second
    float sidechainLowCorrelation; // low band (< 120 Hz) correlation of this channel with the sidechain
    std::int32_t correlationValid;
    HarmonicSettings harmonics;  // overtones for small speakers
    TransientSettings transients; // attack emphasis
    float outputDb[kBands];      // ERB band levels after the whole chain (what the mix gets), live only
    SaturationSettings saturation;
    std::int32_t intentionalDrive;  // the channel is meant to distort: no clipping warnings
    std::int32_t detectedRole;      // ma::Role recognised on the last Learn pass (0 = none)
    float detectedConfidence;
    // Phase 7: the engineer's chain on the channel, and what the Learn pass measured for it.
    CleanupSettings cleanup;
    CompressorSettings compressor;
    ClipperSettings clipper;
    BassEvenerSettings evener;
    std::int32_t statsValid;
    float levelP10, levelP50, levelP95; // 100 ms RMS while playing, pre-trim (dBFS)
    float lowNoteHz;                    // usual bass note, 0 = none
    float tailPitchHz;                  // phase 14: the pitch hits settle on (a kick's tuning), 0 = none
    float compressorReductionDb;        // meters, about the last second
    float clipperDb;
    float evenerGainDb;
    Resonance resonances[kMaxResonances]; // found on the last Learn pass, strongest first
    // Phase 8: mid-side EQ on the channel, and its live mid and side per ERB band (pre-trim, before the stereo EQ).
    StereoSettings stereo;
    std::int32_t stereoValid;
    float midDb[kBands];
    float sideDb[kBands];
    // Phase 23: which tools the Brain set and still are as it left them, and which the user changed
    // since (bits of ToolBit). A tool both off is in neither.
    std::int32_t brainTools;
    std::int32_t changedTools;
    // Phase 26: the width slider (percent, 100 = off) and what the shared width plan made of it.
    float widthRequest;
    float widthFromHz;
    WidthSettings width;
    float widthReach;           // 0..1 of the full widening that was safe for the channel
    std::int32_t widthLimitedBySum;
    float widthLimitHz;
    float widthWorstCoherence;  // its own worst audible band after the width
    float widthWorstHz;
    // Phase 30: the id other Agents know this channel by (kept in the project, unlike the slot's
    // owner), when it took it (a clone that loads the same id while the original runs takes a new
    // one), and its dynamic dips for other channels.
    std::uint64_t keyId;
    std::uint64_t keySinceMs;
    UnmaskSettings unmask[kMaxUnmask];
    std::uint64_t unmaskKey[kMaxUnmask];   // keyId of the channel each dip gives way to
    std::int32_t unmaskKeyFound[kMaxUnmask]; // that channel's Agent is running and publishing
    float unmaskReductionDb[kMaxUnmask];   // most each dip took, about the last second
    std::uint64_t duckKey;                 // the duck's key without a cable (the kick's keyId), 0 = cable only
    std::int32_t duckKeyFound;             // that Agent runs and the duck listens to it (no cable signal)
    // Phase 32: dynamic bands and the rider, the Learn probes (DynamicProbe order, band minus the whole
    // part while it plays, dB) and the meters (about the last second).
    DynamicEqSettings dynamicEq;
    std::int32_t probesValid;
    float probeP50[kDynamicBands];
    float probeP95[kDynamicBands];
    float dynamicReductionDb[kDynamicBands];
    float riderGainDb;
    // Phase 33: the Agent's pan, and how its input leans (left minus right above 150 Hz, dB).
    PanSettings pan;
    std::int32_t balanceValid;
    float inputBalanceDb;
    DepthSettings depth; // phase 34
    // Phase 35: its own delay, and its low end against the sidechain's in time (with a cable).
    AlignSettings align;
    std::int32_t alignValid;
    float alignLagMs;  // > 0: this channel later than the sidechain
    float alignBest;   // low-band correlation at that lag
    float alignZero;   // and as it plays now
    std::int32_t sendDuckTo; // phase 37: a send's choice (SendDuckTo), the user's
    std::int32_t reservedSession;
    // The Brain this Agent belongs to (saved in the project): two projects open at once share the
    // file, and each Brain only takes its own Agents. 0 = not joined yet, seen by every Brain.
    std::uint64_t sessionId;
};

// Phase 23: the tools of an Agent, for AgentData::brainTools / changedTools.
enum ToolBit : std::int32_t
{
    toolEq = 1 << 0,
    toolCleanup = 1 << 1,
    toolCompressor = 1 << 2,
    toolClipper = 1 << 3,
    toolEvener = 1 << 4,
    toolStereo = 1 << 5,
    toolSaturation = 1 << 6,
    toolDuck = 1 << 7,
    toolHarmonics = 1 << 8,
    toolTransients = 1 << 9,
    toolUnmask = 1 << 10, // phase 30
    toolDynamicEq = 1 << 11, // phase 32: dynamic bands and the rider
    toolPan = 1 << 12,       // phase 33
    toolDepth = 1 << 13,     // phase 34: a send's high cut
    toolAlign = 1 << 14      // phase 35: the bass's delay against the kick
};

// Phase 37: what a send (reverb, delay) gives way to, chosen in its Agent.
enum SendDuckTo : std::int32_t
{
    sendDuckAuto = 0,   // the Brain decides from what feeds it
    sendDuckVoice = 1,  // the main voice (the loudest vocal, else lead)
    sendDuckSource = 2, // its own source: quiet on the hit, the tail blooms after it
    sendDuckNone = 3
};

// Note lengths for a tempo-synced duck release, indexed by AgentData::duckSync.
constexpr int kDuckSyncChoices = 5;
inline double duckSyncFraction(int index)
{
    constexpr double fractions[kDuckSyncChoices] = {0.0, 1.0 / 32.0, 1.0 / 16.0, 1.0 / 8.0, 1.0 / 4.0};
    return index > 0 && index < kDuckSyncChoices ? fractions[index] : 0.0;
}

enum class CommandKind : std::int32_t
{
    none = 0,
    apply = 1, // remember the current settings, then set trim, EQ, duck and polarity
    undo = 2,  // go back to the remembered settings
    adapt = 3, // adaptive mode: set the EQ only, keep the remembered settings as they are
    refine = 4 // phase 30: like apply, but an existing backup stays (one undo for the whole refining)
};

struct Command
{
    std::uint64_t target; // owner id the command is for
    std::uint32_t serial; // changes with every command
    CommandKind kind;
    float trimDb;
    EqBand eq[kEqBands];
    DuckSettings duck;
    std::int32_t duckSync;
    std::int32_t polarityInverted;
    HarmonicSettings harmonics;
    TransientSettings transients;
    CleanupSettings cleanup;
    CompressorSettings compressor;
    ClipperSettings clipper;
    BassEvenerSettings evener;
    StereoSettings stereo;
    SaturationSettings saturation; // phase 13
    // Phase 15: how far a bus's sources moved (dB) with this command; its learned levels follow,
    // so a second plan sees what now reaches it. 0 for every other channel.
    float inputShiftDb;
    // Phase 30: the dynamic dips (keys by AgentData::keyId).
    UnmaskSettings unmask[kMaxUnmask];
    std::uint64_t unmaskKey[kMaxUnmask];
    std::uint64_t duckKey; // the duck listens to this Agent when no cable brings a sidechain
    DynamicEqSettings dynamicEq; // phase 32
    PanSettings pan;             // phase 33
    DepthSettings depth;         // phase 34
    AlignSettings align;         // phase 35
};

// Phase 30: an Agent's output envelope per octave band, written from its audio thread every block
// while another Agent listens to it (outside the sequence lock: single floats, read as they come).
struct KeyFeed
{
    std::atomic<std::uint64_t> wantedUntilMs; // a listener asked for it until then (any Agent writes)
    std::atomic<std::uint64_t> stampMs;       // when the owner last wrote the levels
    std::atomic<std::uint64_t> keyId;         // the owner's AgentData::keyId, so listeners find it cheaply
    std::atomic<std::uint64_t> keySinceMs;    // and since when it has it
    std::atomic<float> levelDb[kKeyBands];
};

// Phase 31: what the Brain decided about each of an Agent's tools (ToolBit order) at its last
// analysis, so the Agent can say "the Brain looked at this and left it off, because ..." instead of
// leaving the user to guess whether it was considered at all. Written by the Brain under its own
// sequence lock, read by the Agent's timer; the text is the reason, UTF-8, the Agent adds the verdict.
constexpr int kToolCount = 15;
constexpr int kNoteLength = 384;
enum class ToolDecision : std::int32_t
{
    none = 0,       // no word on it (yet)
    proposed = 1,   // the last analysis puts it in the proposal (set, changed or switched off)
    kept = 2,       // checked: it stays as the Agent has it
    notNeeded = 3,  // checked: not needed, the reason says why
    notChecked = 4, // could not be judged (no data, not in this analysis' scope): the reason says what is missing
    yours = 5       // the user's setting, the Brain leaves it alone on purpose
};

struct ToolNotes
{
    std::uint64_t target; // owner id the notes are for
    std::int64_t madeMs;  // when the analysis ran (ms since 1970, so a saved note can tell its age)
    std::int32_t wholeMix; // 1 = "Analizuj" on the whole chain, 0 = EQ and give-way only
    std::int32_t reserved;
    std::int32_t decision[kToolCount]; // ToolDecision
    std::int32_t reserved2;
    char text[kToolCount][kNoteLength];
};

struct Slot
{
    std::atomic<std::uint64_t> owner;       // 0 = free, else the owning instance id
    std::atomic<std::uint64_t> heartbeatMs; // last publish time
    std::atomic<std::uint32_t> sequence;
    std::uint32_t reserved;
    AgentData data;
    std::atomic<std::uint32_t> commandSequence;
    std::uint32_t reserved2;
    Command command;
    KeyFeed key; // phase 30
    std::atomic<std::uint32_t> notesSequence; // phase 31
    std::uint32_t reserved3;
    ToolNotes notes;
};

// "Kontrola mastera": the plugin after the mastering chain writes what it measures into its Brain's entry
// (own sequence lock). Everything is since the Brain's last reset (`BrainEntry::resetEpoch`), like the
// Brain's own master measurements, so the two compare.
struct MasterReport
{
    std::atomic<std::uint32_t> sequence;
    std::uint32_t reserved;
    std::atomic<std::uint64_t> heartbeatMs;
    float seconds;          // analysed since the reset
    float integratedLufs;
    float maxShortTermLufs; // the loudest 3 s
    float truePeakDb;
    float sideDb;           // side minus mid, 150 Hz up (dB)
    float lowSideDb;        // side minus mid below 120 Hz
    float bandsDb[kBands];  // average ERB spectrum, like the Brain's master analyser
    float reserved2[2];
};

// One per Brain that is running: FL can have two projects open (two instances), and their
// plugins all map the same file.
constexpr int kMaxBrains = 4;
struct BrainEntry
{
    std::atomic<std::uint64_t> sessionId;          // 0 = free; saved in the Brain's state
    std::atomic<std::uint64_t> processId;          // the host process it runs in
    std::atomic<std::uint64_t> heartbeatMs;        // this Brain is alive
    std::atomic<std::uint64_t> spectraWantedUntil; // its Agents compute live spectra until this time
    std::atomic<std::uint32_t> compareBefore;      // A/B: its Agents play their settings from before the last apply
    std::atomic<std::uint32_t> resetEpoch;         // goes up when the Brain starts measuring from scratch
    MasterReport master;
};

struct Header
{
    std::atomic<std::uint32_t> magic;
    std::uint32_t version;
    BrainEntry brains[kMaxBrains];
};

struct Region
{
    Header header;
    Slot slots[kMaxSlots];
};

// Makes a zeroed or foreign region usable. Safe to call from every instance.
void initialise(Region& region);

// Brain side: its entry (the one with its session id, else a free or abandoned one), or null when
// four other Brains are alive.
BrainEntry* claimBrain(Region& region, std::uint64_t sessionId, std::uint64_t processId, std::uint64_t nowMs);
// Agent side: the alive Brain of `sessionId`; with 0 (not joined yet) any alive Brain. Null if none.
const BrainEntry* findBrain(const Region& region, std::uint64_t sessionId, std::uint64_t nowMs);
inline BrainEntry* findBrain(Region& region, std::uint64_t sessionId, std::uint64_t nowMs)
{
    return const_cast<BrainEntry*>(findBrain(static_cast<const Region&>(region), sessionId, nowMs));
}
// Master check side: writes its report into the entry; the Brain reads a consistent copy (false: none yet,
// stale, or it kept changing).
struct MasterValues
{
    float seconds = 0.0f, integratedLufs = -200.0f, maxShortTermLufs = -200.0f, truePeakDb = -200.0f, sideDb = -200.0f, lowSideDb = -200.0f;
    float bandsDb[kBands] {};
};
void publishMaster(BrainEntry& brain, const MasterValues& values, std::uint64_t nowMs);
bool readMaster(const BrainEntry& brain, MasterValues& out, std::uint64_t nowMs);
// Agent side: the session to belong to. One that is alive is kept. An Agent without one joins the
// Brain in its own process, else the only Brain alive. One whose Brain has been gone for
// `missingMs` >= kRejoinMs (the Brain was replaced; a project still loading has its Brain up long
// before) joins the Brain in its own process, or the only one alive.
constexpr std::uint64_t kRejoinMs = 10000;
std::uint64_t chooseSession(const Region& region, std::uint64_t current, std::uint64_t processId, std::uint64_t nowMs, std::uint64_t missingMs);

// Claims a free or abandoned slot for `ownerId`. Returns the slot index or -1 if all are taken.
int claimSlot(Region& region, std::uint64_t ownerId, std::uint64_t nowMs);
void releaseSlot(Region& region, int index, std::uint64_t ownerId);

void publish(Slot& slot, const AgentData& data, std::uint64_t nowMs);
// Consistent copy of a slot. Returns false if it is free, stale or kept changing.
bool read(const Slot& slot, AgentData& out, std::uint64_t nowMs);

// Brain side: write a command into an Agent's slot. Agent side: read it.
void sendCommand(Slot& slot, const Command& command);
bool readCommand(const Slot& slot, Command& out);

// Phase 31: the Brain's notes per tool. Brain writes, the Agent reads (false until written once).
void sendNotes(Slot& slot, const ToolNotes& notes);
bool readNotes(const Slot& slot, ToolNotes& out);

bool isAlive(std::uint64_t heartbeatMs, std::uint64_t nowMs);

// Phase 30: the key feed. A listener renews its wish every timer tick; the owner writes the levels
// from its audio thread while wished for; a reader treats levels older than kKeyStaleMs as silence
// (the key's Agent stopped, was bypassed or the host stopped processing it).
constexpr std::uint64_t kKeyWantedMs = 3000;
constexpr std::uint64_t kKeyStaleMs = 250;
void wantKey(Slot& slot, std::uint64_t nowMs);
bool keyWanted(const Slot& slot, std::uint64_t nowMs);
void writeKey(Slot& slot, const float* levelDb, std::uint64_t nowMs);
// The key's level in the given octave bands (keyLevelDb), kKeySilentDb when stale.
double readKey(const Slot& slot, std::int32_t bands, std::uint64_t nowMs);

} // namespace ma::bus
