#pragma once

#include "ma/MixBus.h"

#include <juce_core/juce_core.h>

#include <chrono>
#include <memory>
#include <mutex>

namespace ma
{

// Maps the Agents' and Brain's shared region (ma::bus::Region) from a file in the
// user's temp folder. Every plugin instance on the machine maps the same file, so it
// works across processes too (FL's bridged mode). One mapping per process, shared.
class SharedSession
{
public:
    static std::shared_ptr<SharedSession> get()
    {
        static std::mutex mutex;
        static std::weak_ptr<SharedSession> instance;

        const std::lock_guard<std::mutex> lock(mutex);
        auto session = instance.lock();
        if (session == nullptr)
        {
            session = std::shared_ptr<SharedSession>(new SharedSession());
            instance = session;
        }
        return session;
    }

    // Null if the file could not be mapped; callers then simply work alone.
    bus::Region* region() const { return regionPtr; }

    // Milliseconds on a clock shared by all processes on the machine.
    static std::uint64_t nowMs()
    {
        using namespace std::chrono;
        return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    }

    static std::uint64_t newInstanceId()
    {
        std::uint64_t id = 0;
        while (id == 0)
            id = static_cast<std::uint64_t>(juce::Random::getSystemRandom().nextInt64());
        return id;
    }

private:
    SharedSession()
    {
        const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("MixAssistant");
        folder.createDirectory();
        const auto file = folder.getChildFile("session-v" + juce::String(static_cast<int>(bus::kVersion)) + ".bin");
        const auto size = static_cast<juce::int64>(sizeof(bus::Region));

        if (file.getSize() != size)
        {
            // Create (or resize) the file filled with zeros; initialise() clears it properly.
            juce::FileOutputStream out(file);
            if (out.openedOk())
            {
                out.setPosition(0);
                out.truncate();
                const juce::HeapBlock<char> zeros(static_cast<size_t>(size), true);
                out.write(zeros.get(), static_cast<size_t>(size));
                out.flush();
            }
        }

        mapping = std::make_unique<juce::MemoryMappedFile>(file, juce::MemoryMappedFile::readWrite, false);
        if (mapping->getData() != nullptr && mapping->getSize() >= sizeof(bus::Region))
        {
            regionPtr = static_cast<bus::Region*>(mapping->getData());
            bus::initialise(*regionPtr);
        }
    }

    std::unique_ptr<juce::MemoryMappedFile> mapping;
    bus::Region* regionPtr = nullptr;
};

} // namespace ma
