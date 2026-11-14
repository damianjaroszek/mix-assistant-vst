#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace ma::clock
{

// Milliseconds on a clock shared by all processes on the machine. An offline test (mixcheck) sets
// `testMs` and moves it with the audio it renders, so every timeout and refresh in the plugins
// follows the song instead of the wall clock and a run gives the same result every time.
inline std::atomic<std::int64_t> testMs { -1 };

inline std::uint64_t nowMs()
{
    const auto test = testMs.load(std::memory_order_relaxed);
    if (test >= 0)
        return static_cast<std::uint64_t>(test);
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

} // namespace ma::clock
