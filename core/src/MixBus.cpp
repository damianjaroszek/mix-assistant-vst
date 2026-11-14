#include "ma/MixBus.h"

#include <chrono>
#include <cstring>
#include <thread>

namespace ma::bus
{
namespace
{
constexpr std::uint32_t kInitialising = 0x494E4954; // "INIT"
}

void initialise(Region& region)
{
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        auto magic = region.header.magic.load();
        if (magic == kMagic && region.header.version == kVersion)
            return;
        if (magic != kInitialising)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); // another instance is clearing it
    }

    // First user (or an older layout): clear everything, then publish the magic last.
    auto magic = region.header.magic.load();
    const bool current = magic == kMagic && region.header.version == kVersion;
    if (!current && region.header.magic.compare_exchange_strong(magic, kInitialising))
    {
        region.header.version = kVersion;
        for (auto& brain : region.header.brains)
        {
            brain.sessionId.store(0);
            brain.processId.store(0);
            brain.heartbeatMs.store(0);
            brain.spectraWantedUntil.store(0);
            brain.compareBefore.store(0);
            brain.resetEpoch.store(0);
            brain.master.sequence.store(0);
            brain.master.heartbeatMs.store(0);
        }
        for (auto& slot : region.slots)
        {
            slot.owner.store(0);
            slot.heartbeatMs.store(0);
            slot.sequence.store(0);
            slot.data = AgentData {};
            slot.commandSequence.store(0);
            slot.command = Command {};
            slot.key.wantedUntilMs.store(0);
            slot.key.stampMs.store(0);
            slot.key.keyId.store(0);
            slot.key.keySinceMs.store(0);
            for (auto& level : slot.key.levelDb)
                level.store(kKeySilentDb);
            slot.notesSequence.store(0);
            slot.notes = ToolNotes {};
        }
        region.header.magic.store(kMagic);
    }
}

bool isAlive(std::uint64_t heartbeatMs, std::uint64_t nowMs)
{
    if (heartbeatMs == 0)
        return false;
    return nowMs < heartbeatMs || nowMs - heartbeatMs <= kAliveTimeoutMs;
}

BrainEntry* claimBrain(Region& region, std::uint64_t sessionId, std::uint64_t processId, std::uint64_t nowMs)
{
    if (sessionId == 0)
        return nullptr;
    for (auto& brain : region.header.brains)
        if (brain.sessionId.load() == sessionId)
        {
            brain.processId.store(processId);
            return &brain;
        }
    for (auto& brain : region.header.brains)
    {
        auto owner = brain.sessionId.load();
        if ((owner == 0 || !isAlive(brain.heartbeatMs.load(), nowMs)) && brain.sessionId.compare_exchange_strong(owner, sessionId))
        {
            brain.processId.store(processId);
            brain.spectraWantedUntil.store(0);
            brain.compareBefore.store(0);
            brain.resetEpoch.store(0);
            brain.master.sequence.store(0);
            brain.master.heartbeatMs.store(0);
            brain.heartbeatMs.store(nowMs);
            return &brain;
        }
    }
    return nullptr;
}

const BrainEntry* findBrain(const Region& region, std::uint64_t sessionId, std::uint64_t nowMs)
{
    for (const auto& brain : region.header.brains)
    {
        const auto id = brain.sessionId.load();
        if (id != 0 && (sessionId == 0 || id == sessionId) && isAlive(brain.heartbeatMs.load(), nowMs))
            return &brain;
    }
    return nullptr;
}

void publishMaster(BrainEntry& brain, const MasterValues& values, std::uint64_t nowMs)
{
    auto& m = brain.master;
    m.sequence.fetch_add(1); // odd: being written
    m.seconds = values.seconds;
    m.integratedLufs = values.integratedLufs;
    m.maxShortTermLufs = values.maxShortTermLufs;
    m.truePeakDb = values.truePeakDb;
    m.sideDb = values.sideDb;
    m.lowSideDb = values.lowSideDb;
    std::memcpy(m.bandsDb, values.bandsDb, sizeof(m.bandsDb));
    m.sequence.fetch_add(1);
    m.heartbeatMs.store(nowMs);
}

bool readMaster(const BrainEntry& brain, MasterValues& out, std::uint64_t nowMs)
{
    const auto& m = brain.master;
    if (!isAlive(m.heartbeatMs.load(), nowMs))
        return false;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const auto before = m.sequence.load();
        if (before & 1u)
            continue;
        out.seconds = m.seconds;
        out.integratedLufs = m.integratedLufs;
        out.maxShortTermLufs = m.maxShortTermLufs;
        out.truePeakDb = m.truePeakDb;
        out.sideDb = m.sideDb;
        out.lowSideDb = m.lowSideDb;
        std::memcpy(out.bandsDb, m.bandsDb, sizeof(out.bandsDb));
        if (m.sequence.load() == before)
            return true;
    }
    return false;
}

std::uint64_t chooseSession(const Region& region, std::uint64_t current, std::uint64_t processId, std::uint64_t nowMs, std::uint64_t missingMs)
{
    std::uint64_t sameProcess = 0, only = 0;
    int alive = 0, inProcess = 0;
    bool currentAlive = false, currentInProcess = false;
    for (const auto& brain : region.header.brains)
    {
        const auto id = brain.sessionId.load();
        if (id == 0 || !isAlive(brain.heartbeatMs.load(), nowMs))
            continue;
        ++alive;
        only = id;
        const bool here = brain.processId.load() == processId;
        if (here)
        {
            ++inProcess;
            sameProcess = id;
        }
        if (id == current)
        {
            currentAlive = true;
            currentInProcess = here;
        }
    }
    // An Agent that joined another project's Brain before its own was there comes home.
    if (current != 0 && currentAlive && !currentInProcess && inProcess == 1)
        return sameProcess;
    if (current != 0 && (currentAlive || missingMs < kRejoinMs))
        return current;
    if (inProcess == 1)
        return sameProcess;
    if (alive == 1 && inProcess == 0)
        return only;
    return current;
}

int claimSlot(Region& region, std::uint64_t ownerId, std::uint64_t nowMs)
{
    for (int i = 0; i < kMaxSlots; ++i)
    {
        auto& slot = region.slots[i];
        auto owner = slot.owner.load();
        if (owner == ownerId)
            return i;

        // Free, or left behind by an instance that crashed or was removed without cleanup.
        const bool abandoned = owner != 0 && !isAlive(slot.heartbeatMs.load(), nowMs);
        if ((owner == 0 || abandoned) && slot.owner.compare_exchange_strong(owner, ownerId))
        {
            slot.heartbeatMs.store(nowMs);
            return i;
        }
    }
    return -1;
}

void releaseSlot(Region& region, int index, std::uint64_t ownerId)
{
    if (index < 0 || index >= kMaxSlots)
        return;
    auto& slot = region.slots[index];
    slot.heartbeatMs.store(0);
    auto expected = ownerId;
    slot.owner.compare_exchange_strong(expected, 0);
}

void publish(Slot& slot, const AgentData& data, std::uint64_t nowMs)
{
    const auto seq = slot.sequence.load(std::memory_order_relaxed);
    slot.sequence.store(seq + 1, std::memory_order_relaxed); // odd: writing
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(&slot.data, &data, sizeof(AgentData));
    std::atomic_thread_fence(std::memory_order_release);
    slot.sequence.store(seq + 2, std::memory_order_release); // even: done
    slot.heartbeatMs.store(nowMs, std::memory_order_release);
}

bool read(const Slot& slot, AgentData& out, std::uint64_t nowMs)
{
    if (slot.owner.load() == 0 || !isAlive(slot.heartbeatMs.load(), nowMs))
        return false;

    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const auto before = slot.sequence.load(std::memory_order_acquire);
        if (before & 1u)
            continue;
        std::memcpy(&out, &slot.data, sizeof(AgentData));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.sequence.load(std::memory_order_relaxed) == before)
        {
            out.name[kNameLength - 1] = '\0';
            return true;
        }
    }
    return false;
}

void sendCommand(Slot& slot, const Command& command)
{
    const auto seq = slot.commandSequence.load(std::memory_order_relaxed);
    slot.commandSequence.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(&slot.command, &command, sizeof(Command));
    std::atomic_thread_fence(std::memory_order_release);
    slot.commandSequence.store(seq + 2, std::memory_order_release);
}

bool readCommand(const Slot& slot, Command& out)
{
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const auto before = slot.commandSequence.load(std::memory_order_acquire);
        if (before & 1u)
            continue;
        std::memcpy(&out, &slot.command, sizeof(Command));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.commandSequence.load(std::memory_order_relaxed) == before)
            return before != 0;
    }
    return false;
}

void sendNotes(Slot& slot, const ToolNotes& notes)
{
    const auto seq = slot.notesSequence.load(std::memory_order_relaxed);
    slot.notesSequence.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(&slot.notes, &notes, sizeof(ToolNotes));
    std::atomic_thread_fence(std::memory_order_release);
    slot.notesSequence.store(seq + 2, std::memory_order_release);
}

bool readNotes(const Slot& slot, ToolNotes& out)
{
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const auto before = slot.notesSequence.load(std::memory_order_acquire);
        if (before & 1u)
            continue;
        std::memcpy(&out, &slot.notes, sizeof(ToolNotes));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.notesSequence.load(std::memory_order_relaxed) == before)
        {
            for (auto& line : out.text)
                line[kNoteLength - 1] = '\0';
            return before != 0;
        }
    }
    return false;
}

void wantKey(Slot& slot, std::uint64_t nowMs)
{
    const auto until = nowMs + kKeyWantedMs;
    auto current = slot.key.wantedUntilMs.load(std::memory_order_relaxed);
    while (current < until && !slot.key.wantedUntilMs.compare_exchange_weak(current, until, std::memory_order_relaxed))
    {
    }
}

bool keyWanted(const Slot& slot, std::uint64_t nowMs)
{
    return nowMs < slot.key.wantedUntilMs.load(std::memory_order_relaxed);
}

void writeKey(Slot& slot, const float* levelDb, std::uint64_t nowMs)
{
    for (int b = 0; b < kKeyBands; ++b)
        slot.key.levelDb[b].store(levelDb[b], std::memory_order_relaxed);
    slot.key.stampMs.store(nowMs, std::memory_order_release);
}

double readKey(const Slot& slot, std::int32_t bands, std::uint64_t nowMs)
{
    const auto stamp = slot.key.stampMs.load(std::memory_order_acquire);
    if (stamp == 0 || (nowMs > stamp && nowMs - stamp > kKeyStaleMs))
        return kKeySilentDb;
    float levels[kKeyBands];
    for (int b = 0; b < kKeyBands; ++b)
        levels[b] = slot.key.levelDb[b].load(std::memory_order_relaxed);
    return keyLevelDb(levels, bands);
}

} // namespace ma::bus
