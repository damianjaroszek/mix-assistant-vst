#pragma once

#include "ChannelEq.h"

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
constexpr std::uint32_t kVersion = 2;
constexpr int kMaxSlots = 128;
constexpr int kNameLength = 48;
constexpr int kBands = 40;            // ERB bands, as SpectrumAnalyzer with BandScale::erb
constexpr float kSilentDb = -200.0f;  // stands in for -inf in the shared data
constexpr std::uint64_t kAliveTimeoutMs = 2000;

static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "shared-memory atomics must be lock-free");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "shared-memory atomics must be lock-free");

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
};

enum class CommandKind : std::int32_t
{
    none = 0,
    apply = 1, // remember the current settings, then set trim and EQ
    undo = 2   // go back to the remembered settings
};

struct Command
{
    std::uint64_t target; // owner id the command is for
    std::uint32_t serial; // changes with every command
    CommandKind kind;
    float trimDb;
    EqBand eq[kEqBands];
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
};

struct Header
{
    std::atomic<std::uint32_t> magic;
    std::uint32_t version;
    std::atomic<std::uint64_t> brainHeartbeatMs;   // a Brain is alive
    std::atomic<std::uint64_t> spectraWantedUntil; // Agents compute live spectra until this time
    std::atomic<std::uint32_t> compareBefore;      // A/B: Agents play their settings from before the last apply
    std::uint32_t reserved;
};

struct Region
{
    Header header;
    Slot slots[kMaxSlots];
};

// Makes a zeroed or foreign region usable. Safe to call from every instance.
void initialise(Region& region);

// Claims a free or abandoned slot for `ownerId`. Returns the slot index or -1 if all are taken.
int claimSlot(Region& region, std::uint64_t ownerId, std::uint64_t nowMs);
void releaseSlot(Region& region, int index, std::uint64_t ownerId);

void publish(Slot& slot, const AgentData& data, std::uint64_t nowMs);
// Consistent copy of a slot. Returns false if it is free, stale or kept changing.
bool read(const Slot& slot, AgentData& out, std::uint64_t nowMs);

// Brain side: write a command into an Agent's slot. Agent side: read it.
void sendCommand(Slot& slot, const Command& command);
bool readCommand(const Slot& slot, Command& out);

bool isAlive(std::uint64_t heartbeatMs, std::uint64_t nowMs);

} // namespace ma::bus
