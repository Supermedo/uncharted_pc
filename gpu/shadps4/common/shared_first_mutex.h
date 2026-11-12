// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace Common {

// Like std::shared_mutex, but reader has priority over writer.
//
// bbport: readers enter with one atomic add instead of an inner std::mutex. The pipeline cache
// takes its locks shared a few times per draw from the GPU command thread and the draw
// preparation workers at once; with the inner mutex they queued on each other (U1 chapter 2:
// ~14% of the GPU command thread in lock_shared/unlock_shared). Same rules as before: a reader
// gets in whenever no writer holds the lock (also while one waits, so shared locks may nest), a
// writer waits for zero readers. BB_SHARED_FIRST_LEGACY=1 (read once): the old implementation.
class SharedFirstMutex {
public:
    void lock() {
        if (Legacy()) {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [this]() { return !writer_active && readers == 0; });
            writer_active = true;
            return;
        }
        std::scoped_lock writers{writer_mutex};
        writers_waiting.fetch_add(1, std::memory_order_seq_cst);
        for (u32 spins = 0;; ++spins) {
            u32 seen = state.load(std::memory_order_seq_cst);
            if (seen == 0 &&
                state.compare_exchange_weak(seen, Writer, std::memory_order_acquire)) {
                break;
            }
            if (spins < 64) {
                std::this_thread::yield();
            } else if (seen != 0) {
                state.wait(seen, std::memory_order_seq_cst);
            }
        }
        writers_waiting.fetch_sub(1, std::memory_order_relaxed);
    }

    bool try_lock() {
        if (Legacy()) {
            std::lock_guard<std::mutex> lock(mtx);
            if (writer_active || readers > 0) {
                return false;
            }
            writer_active = true;
            return true;
        }
        // writer_mutex only queues waiting writers; the Writer bit is the lock itself.
        u32 expected = 0;
        return state.compare_exchange_strong(expected, Writer, std::memory_order_acquire);
    }

    void unlock() {
        if (Legacy()) {
            std::lock_guard<std::mutex> lock(mtx);
            writer_active = false;
            cv.notify_all();
            return;
        }
        state.fetch_sub(Writer, std::memory_order_release);
        state.notify_all();
    }

    void lock_shared() {
        if (Legacy()) {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [this]() { return !writer_active; });
            ++readers;
            return;
        }
        for (;;) {
            const u32 before = state.fetch_add(1, std::memory_order_acquire);
            if (!(before & Writer)) {
                return;
            }
            UndoShared();
            u32 seen = state.load(std::memory_order_seq_cst);
            if (seen & Writer) {
                state.wait(seen, std::memory_order_seq_cst);
            }
        }
    }

    bool try_lock_shared() {
        if (Legacy()) {
            std::lock_guard<std::mutex> lock(mtx);
            if (writer_active) {
                return false;
            }
            ++readers;
            return true;
        }
        const u32 before = state.fetch_add(1, std::memory_order_acquire);
        if (!(before & Writer)) {
            return true;
        }
        UndoShared();
        return false;
    }

    void unlock_shared() {
        if (Legacy()) {
            std::lock_guard<std::mutex> lock(mtx);
            if (--readers == 0) {
                cv.notify_all();
            }
            return;
        }
        UndoShared();
    }

private:
    using u32 = std::uint32_t;
    static constexpr u32 Writer = 1u << 31;

    static bool Legacy() {
        static const bool legacy = [] {
            const char* env = std::getenv("BB_SHARED_FIRST_LEGACY");
            return env && env[0] == '1';
        }();
        return legacy;
    }

    /// Drops one reader; the last one wakes a waiting writer.
    void UndoShared() {
        const u32 before = state.fetch_sub(1, std::memory_order_release);
        if ((before & ~Writer) == 1 && writers_waiting.load(std::memory_order_seq_cst) != 0) {
            state.notify_all();
        }
    }

    // Legacy implementation.
    std::mutex mtx;
    std::condition_variable cv;
    int readers = 0;
    bool writer_active = false;

    // Atomic implementation: Writer bit | reader count.
    std::atomic<u32> state{0};
    std::atomic<u32> writers_waiting{0};
    std::mutex writer_mutex;
};

} // namespace Common
