// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "bbport_copy.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <boost/container/small_vector.hpp>

#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {
class GraphicsPipeline;
struct SubmitInfo;
class Runtime;
class StagingBufferPool;
} // namespace Vulkan

namespace VideoCore {

class TextureCache;
class MemoryTracker;
class PageManager;

class BufferCache {
    static constexpr u64 ADDRESS_SPACE_BITS = 40;
    // bbport: arena pages are 4 GiB unless the driver's buffer size limit is lower (AMD's
    // Windows driver: 2 GiB); an arena spans up to two pages, so they shrink to half the limit.
    static constexpr u64 MAX_ARENA_PAGE_BITS = 32;
    static constexpr u64 MIN_ARENA_PAGE_BITS = 30;
    static constexpr u64 MAX_ARENA_PAGES = u64{1} << (ADDRESS_SPACE_BITS - MIN_ARENA_PAGE_BITS);
    static constexpr u64 MIN_BLOCK_SIZE = 16_KB;

public:
    /// Read-only bindings up to this size are copied into a stream buffer (bbport: public for
    /// the draw pipeline's constant ring).
    static constexpr u64 STREAM_THRESHOLD = 16_KB;

    explicit BufferCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool,
                         TextureCache& texture_cache, PageManager& tracker);
    ~BufferCache();

    /// Returns a pointer to GDS device local buffer.
    [[nodiscard]] const Buffer* GetGdsBuffer() const noexcept {
        return &gds_buffer;
    }

    /// Retrieves the device local DBA page table buffer.
    [[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept {
        return bda_pagetable_buffer.get();
    }

    /// Retrieves the fault buffer.
    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return fault_manager->GetFaultBuffer();
    }

    /// Retrieves the stream buffer.
    StreamBuffer& GetStreamBuffer() noexcept {
        return stream_buffer;
    }

    /// Returns minimum granularity of a sparse memory bind.
    u32 GetSparsePageShift() const noexcept {
        return block_shift;
    }

    /// Invalidates any buffer in the logical page range.
    void InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks = false);

    /// bbport diagnosis (BB_FORCE_VB_UPLOAD): the next ObtainBuffer copies this range from guest
    /// memory again (regions the GPU did not write).
    void ForceUpload(VAddr device_addr, u64 size);

    /// bbport: a guest write fault at `device_addr` was handled; unprotect its neighbourhood.
    void ExtendWriteFault(VAddr device_addr);

    /// Flushes any GPU modified buffer in the logical page range back to CPU memory.
    void ReadMemory(VAddr device_addr, u64 size, bool is_write = false, bool assume_locks = false);

    /// Finds a buffer for the specified region.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBuffer(VAddr device_addr, u32 size,
                                                             bool is_written,
                                                             bool is_texel_buffer = false);

    /// bbport: the 64-dword copy of the game's mesh raycasters moves GPU results into a record
    /// array the guest CPU reads. Instead of waiting for the GPU at the dispatch, the guest part
    /// of the copy is made now and the GPU-written bytes of the source land on a worker thread
    /// once the GPU work that wrote them has completed; the labels the guest learns of the GPU's
    /// progress from wait for it (landing_gate.h). False when not deferred (no side queue,
    /// BB_SCALAR_COPY64_DEFER=0): the caller reads back as before.
    bool DeferGuestCopy(VAddr src, VAddr dst, u64 size);
    u64 PendingLanding() const;
    bool LandingGateActive() const;
    void WaitLanding(u64 seq);
    void RunAfterLanding(u64 seq, std::function<void()> fn);
    /// Ends the landing thread: nothing more is deferred, every wait on it lets go, what is
    /// pending is dropped (~BufferCache; idempotent).
    void StopLanding();

    /// bbport diagnosis (BB_STABLE_CHECK=1): at an end-of-pipe fence, reports read-only
    /// bindings since the last one whose guest data changed meanwhile.
    void CheckStableBindings();

    /// Attempts to obtain a buffer without modifying the cache contents.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBufferForImage(VAddr device_addr, u32 size);

    /// Return true when a region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr addr, size_t size);

    /// Return true when a region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size);

    /// bbport: whether buffers the GPU wrote cover any byte of the range. The page tracker
    /// (IsRegionGpuModified) flags whole 4 KiB pages, and so the bytes next to GPU data.
    [[nodiscard]] bool IsRangeGpuWritten(VAddr addr, size_t size) const {
        return gpu_modified_ranges.Intersects(addr, size);
    }

    /// bbport: writes the image that starts at `addr` (its current GPU contents, tiled as in
    /// guest memory) into the buffer of that range, before a shader copies the range as plain
    /// memory. False when no image starts there.
    bool SyncImageToBuffer(VAddr addr, u32 size);

    /// Processes the fault buffer.
    void ProcessFaultBuffer();

    /// Synchronizes all buffers needed for DMA.
    void SynchronizeDmaBuffers();

    /// Commits pending sparse buffer memory binds. Must be called before every scheduler submit.
    void SubmitPendingArenaBinds(Vulkan::SubmitInfo& info);

    /// bbport: the arenas' guest and GPU address ranges and the resident block ranges (GPU
    /// crash report).
    void DumpArenas() const;

private:
    struct ArenaBinds {
        const Buffer* arena;
        boost::container::small_vector<vk::SparseMemoryBind, 32> binds;
    };

    ArenaBinds* BindsForArena(const Buffer* arena) {
        auto it = std::ranges::find(pending_binds, arena, &ArenaBinds::arena);
        if (it != pending_binds.end()) {
            return std::addressof(*it);
        }
        return &pending_binds.emplace_back(arena);
    }

    const Buffer* GetArena(u64 first_block, u64 last_block);

    void EnsureResident(const Buffer* arena, u64 first_block, u64 last_block);

    void DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size);

    bool SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size, bool is_written,
                           bool is_texel_buffer);

    /// bbport: batched small copies on the copy threads (BbCopy::QueueCopy).
    static void RunGuestCopy(const BbCopy::Item& item);
    void SmallGuestCopy(const BbCopy::Item& item);

    const Buffer* UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                               size_t total_size_bytes);

    bool SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    // bbport: texel buffer ranges known not to alias an image, per image registry generation.
    struct ImageMiss {
        VAddr address = 0;
        u32 size = 0;
        u64 generation = ~0ULL;
    };
    std::array<ImageMiss, 256> image_miss_cache{};
    Vulkan::Runtime& runtime;
    Vulkan::StagingBufferPool& staging_pool;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    TextureCache& texture_cache;
    std::unique_ptr<MemoryTracker> memory_tracker;

    StreamBuffer stream_buffer;
    Buffer gds_buffer;
    RangeSet gpu_modified_ranges;
    /// bbport: pages the guest writes while they hold recent GPU data (InvalidateMemory): the
    /// guest bytes of their GPU-written ranges as the write began. An upload sends only the bytes
    /// the guest changed since (the rest keeps the GPU's data); a read-back keeps those bytes.
    struct ShadowRange {
        VAddr start;
        std::vector<u8> bytes;
    };
    std::mutex shadow_mutex;
    std::unordered_map<VAddr, std::vector<ShadowRange>> shadows;
    std::atomic<u32> shadow_pages{0};
    void ShadowGpuData(VAddr page, u64 length);

    /// bbport: GPU data a guest write lands on, written by GPU work that has completed: read
    /// back on a second queue of the graphics family, which waits for nothing queued since
    /// (Finish waits for the whole queue). False when unavailable.
    struct SideQueue;
    std::unique_ptr<SideQueue> side_queue;
    bool DownloadOnSideQueue(VAddr page, u64 length);

    /// bbport: deferred raycaster copies (DeferGuestCopy).
    struct Landing;
    struct GatedLabel {
        u64 seq;
        std::function<void()> fn;
    };
    std::mutex landing_mutex;
    std::condition_variable landing_cv, landing_done_cv;
    std::deque<std::shared_ptr<Landing>> landing_queue; ///< registered, not yet landed
    std::deque<GatedLabel> gated_labels;                 ///< labels waiting for a landing
    u64 landing_registered_seq{0}, landing_done_seq{0};
    std::atomic<u64> landing_registered{0}, landing_done{0};
    bool landing_stop{false}, gate_running{false};
    std::atomic<u32> gate_outstanding{0}; ///< labels queued or running on the landing thread
    std::thread landing_thread;
    void LandingWorker();
    bool ExecuteLanding(Landing& landing);
    bool WaitLandingTick(u64 tick);
    std::atomic<bool> landing_exited{false};

    std::unique_ptr<FaultManager> fault_manager;
    std::unique_ptr<Buffer> bda_pagetable_buffer;

    std::array<const Buffer*, MAX_ARENA_PAGES> address_space{};
    u64 arena_page_bits{MAX_ARENA_PAGE_BITS};
    u64 arena_page_size{u64{1} << MAX_ARENA_PAGE_BITS};
    u64 num_arena_pages{u64{1} << (ADDRESS_SPACE_BITS - MAX_ARENA_PAGE_BITS)};
    std::deque<Buffer> arenas;
    std::vector<ArenaBinds> pending_binds;
    Vulkan::Semaphore memory_semaphore;

    struct Backing : public Interval {
        vk::DeviceMemory memory;
        u64 offset;
        constexpr bool CanMergeWith(const Backing& other) const noexcept {
            return memory == other.memory && offset + (end - start) == other.offset;
        }
        constexpr Backing SubRange(u64 a, u64 b) const noexcept {
            return {{a, b}, memory, offset + (a - start)};
        }
    };
    IntervalList<Backing> resident_ranges;

    u32 arena_memory_type_index{};
    u32 block_size{};
    u32 block_shift{};
    u32 blocks_per_arena_page{};
    u32 blocks_per_arena_page_shift{};
};

} // namespace VideoCore
