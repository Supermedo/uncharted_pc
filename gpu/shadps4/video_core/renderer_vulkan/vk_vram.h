// bbport: video memory accounting shared by the allocators, and the test knob that makes a big
// card behave like a small one.
//   BB_TEST_VRAM_MB=N    the card has N MiB of device-local memory: the budget the port computes
//                        from, and a hard limit (an allocation over it fails with
//                        VK_ERROR_OUT_OF_DEVICE_MEMORY as on drivers that do not page to system
//                        memory).
//   BB_TEST_VRAM_SOFT=1  with it: report N but do not fail allocations (drivers that page).
//   BB_VRAM_LOG=1        a line with the memory split every 5 s.
#pragma once

#include <atomic>
#include <cstdlib>

#include "common/types.h"

namespace Vulkan::Vram {

inline u64 EnvMiB(const char* name) {
    const char* env = std::getenv(name);
    return env ? std::strtoull(env, nullptr, 10) : 0;
}

/// The simulated device-local size in bytes, 0 when not simulating.
inline u64 TestLimit() {
    static const u64 limit = EnvMiB("BB_TEST_VRAM_MB") << 20;
    return limit;
}

inline bool TestHard() {
    static const bool hard = [] {
        const char* env = std::getenv("BB_TEST_VRAM_SOFT");
        return !(env && env[0] == '1');
    }();
    return hard;
}

/// BB_VRAM_LEGACY=1: the behaviour before the low-memory work (A/B tests).
inline bool Legacy() {
    static const bool legacy = [] {
        const char* env = std::getenv("BB_VRAM_LEGACY");
        return env && env[0] == '1';
    }();
    return legacy;
}

/// BB_TEST_VRAM_STRICT=1: an image allocation that does not fit the simulated card fails for
/// good (a driver that cannot page); otherwise the over-budget fallback is let through (the
/// usual Windows behaviour: the driver pages other memory out).
inline bool Strict() {
    static const bool strict = [] {
        const char* env = std::getenv("BB_TEST_VRAM_STRICT");
        return env && env[0] == '1';
    }();
    return strict;
}

inline bool Log() {
    static const bool log = [] {
        const char* env = std::getenv("BB_VRAM_LOG");
        return env && env[0] == '1';
    }();
    return log;
}

// Bytes by owner (device-local memory the port allocated).
inline std::atomic<u64> image_bytes{0}, image_count{0};
inline std::atomic<u64> arena_bytes{0}, arena_count{0}, arena_host_bytes{0};
inline std::atomic<u64> buffer_bytes{0}; ///< device-local VMA buffers
// Set by the instance: whether `bytes` more device-local memory fit the (simulated) card.
inline bool (*allows)(u64 bytes) = nullptr;
inline bool Allows(u64 bytes) {
    return !allows || allows(bytes);
}

inline std::atomic<bool> urgent{false}; ///< an allocation went over the budget: collect hard
inline std::atomic<u64> failed_allocs{0}, recovered_allocs{0}, host_fallbacks{0};

} // namespace Vulkan::Vram
