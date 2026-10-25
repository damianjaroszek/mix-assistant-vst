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
        region.header.brainHeartbeatMs.store(0);
        region.header.spectraWantedUntil.store(0);
        for (auto& slot : region.slots)
        {
            slot.owner.store(0);
            slot.heartbeatMs.store(0);
            slot.sequence.store(0);
            std::memset(&slot.data, 0, sizeof(slot.data));
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

} // namespace ma::bus
