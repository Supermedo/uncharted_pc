// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <array>
#include <map>
#include <unordered_map>
#include <mutex>
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <magic_enum/magic_enum.hpp>
#include "bbport_copy.h"
#include "bbport_toggles.h"
#include "common/thread.h"
#include "video_core/landing_gate.h"
#include "common/alignment.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#endif

namespace Vulkan {
extern void (*g_gpu_crash_context)();
}

namespace VideoCore {

namespace {
// bbport: deferred raycaster copies (landing_gate.h). The instance is cleared and the stopping
// flag set first thing in ~BufferCache: every wait on the landing thread is released then.
std::atomic<BufferCache*> land_instance{nullptr};
std::atomic<bool> land_stopping{false};
} // namespace

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

struct BufferCache::SideQueue {
    static constexpr u64 StagingSize = 4_MB;

    SideQueue(const Vulkan::Instance& instance, vk::Queue queue_)
        : device{instance.GetDevice()}, queue{queue_},
          staging{instance, 0, StagingSize, MemoryType::HostCached, "Side queue download"} {
        pool = Vulkan::Check(device.createCommandPoolUnique({
            .flags = vk::CommandPoolCreateFlagBits::eTransient,
            .queueFamilyIndex = instance.GetSideQueueFamilyIndex(),
        }));
        cmdbuf = Vulkan::Check(device.allocateCommandBuffers({
                                   .commandPool = *pool,
                                   .level = vk::CommandBufferLevel::ePrimary,
                                   .commandBufferCount = 1,
                               }))
                     .front();
        fence = Vulkan::Check(device.createFenceUnique({}));
    }

    vk::Device device;
    vk::Queue queue;
    Buffer staging;
    vk::UniqueCommandPool pool;
    vk::CommandBuffer cmdbuf;
    vk::UniqueFence fence;
    std::mutex mutex;
};

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      stream_buffer{instance, scheduler, MemoryType::Stream, STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance} {
    while (arena_page_bits > MIN_ARENA_PAGE_BITS &&
           (u64{2} << arena_page_bits) > instance.GetMaxBufferSize()) {
        --arena_page_bits;
    }
    arena_page_size = u64{1} << arena_page_bits;
    num_arena_pages = u64{1} << (ADDRESS_SPACE_BITS - arena_page_bits);
    if (arena_page_bits != MAX_ARENA_PAGE_BITS) {
        LOG_INFO(Render_Vulkan, "Buffer arenas of {} MiB (driver buffer limit {} MiB)",
                 arena_page_size >> 20, instance.GetMaxBufferSize() >> 20);
    }
    const std::array families = {instance.GetGraphicsQueueFamilyIndex(),
                                 instance.GetSideQueueFamilyIndex()};
    const bool shared = families[0] != families[1];
    const vk::BufferCreateInfo probe_ci = {
        .flags =
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = arena_page_size,
        .usage = ARENA_USAGE,
        .sharingMode = shared ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive,
        .queueFamilyIndexCount = shared ? 2u : 0u,
        .pQueueFamilyIndices = shared ? families.data() : nullptr,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    block_size = Common::AlignUp(std::max<u64>(reqs.alignment, MIN_BLOCK_SIZE), reqs.alignment);
    ASSERT_MSG(std::popcount(block_size) == 1, "Sparse block size {} is not a power of 2",
               block_size);
    block_shift = std::bit_width(block_size) - 1;
    blocks_per_arena_page = arena_page_size / block_size;
    blocks_per_arena_page_shift = arena_page_bits - block_shift;
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();

    const u64 bda_pagetable_size =
        (blocks_per_arena_page * num_arena_pages) * sizeof(vk::DeviceAddress);
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, block_shift,
                                                   blocks_per_arena_page * num_arena_pages);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);
    static BufferCache* crash_report_cache;
    crash_report_cache = this;
    Vulkan::g_gpu_crash_context = [] { crash_report_cache->DumpArenas(); };

    // A GPU write marked now is in the command buffer being recorded or, if that is submitted
    // before the command lands, in the next one.
    static Vulkan::Scheduler* write_scheduler;
    write_scheduler = &scheduler;
    g_gpu_write_done_tick = [] { return write_scheduler->CurrentTick() + 1; };
    land_stopping.store(false, std::memory_order_release);
    land_instance.store(this, std::memory_order_release);
    const char* side_env = std::getenv("BB_SIDE_DOWNLOAD");
    if (instance.GetSideQueue() && !(side_env && side_env[0] == '0')) {
        side_queue = std::make_unique<SideQueue>(instance, instance.GetSideQueue());
    }
}

bool BufferCache::DownloadOnSideQueue(VAddr page, u64 length) {
    if (!side_queue) {
        return false;
    }
    const auto start_time = std::chrono::steady_clock::now();
    // The pages around the fault whose GPU data is complete come along: the guest fills its
    // rings page after page, and each would fault and wait for a round trip of its own.
    constexpr u64 Window = 64_KB;
    const u64 first_block = page >> block_shift;
    const u64 last_block = (page + length - 1) >> block_shift;
    const Buffer* arena = GetArena(first_block, last_block);
    const VAddr window_start = std::max(Common::AlignDown(page, Window), arena->cpu_addr);
    const VAddr window_end = std::min(
        std::max(Common::AlignDown(page, Window) + Window, page + length),
        arena->cpu_addr + arena->size_bytes);
    boost::container::small_vector<VAddr, 16> pages;
    u64 write_tick = 0;
    {
        std::scoped_lock lk{shadow_mutex};
        for (VAddr at = window_start; at < window_end; at += TRACKER_BYTES_PER_PAGE) {
            const bool faulting = at >= page && at < page + length;
            if (!memory_tracker->IsRegionGpuModified(at, TRACKER_BYTES_PER_PAGE)) {
                continue;
            }
            const u64 tick = memory_tracker->LatestGpuWriteTick(at, TRACKER_BYTES_PER_PAGE);
            // A page merged before keeps its guest bytes apart (DownloadMemory handles those);
            // a page the guest is writing is not ours to fill.
            const bool usable = !shadows.contains(at) && scheduler.IsFree(tick) &&
                                (faulting || !memory_tracker->IsRegionCpuModified(
                                                 at, TRACKER_BYTES_PER_PAGE));
            if (!usable) {
                if (faulting) {
                    return false;
                }
                continue;
            }
            pages.push_back(at);
            write_tick = std::max(write_tick, tick);
        }
    }
    boost::container::small_vector<vk::BufferCopy, 16> copies;
    u64 total = 0;
    for (const VAddr at : pages) {
        gpu_modified_ranges.ForEachInRange(at, TRACKER_BYTES_PER_PAGE, [&](VAddr start, VAddr end) {
            if (!copies.empty() &&
                copies.back().srcOffset + copies.back().size == start - arena->cpu_addr &&
                copies.back().dstOffset + copies.back().size == total) {
                copies.back().size += end - start;
                total += end - start;
                return;
            }
            total = Common::AlignUp(total, 64);
            copies.push_back(vk::BufferCopy{
                .srcOffset = start - arena->cpu_addr,
                .dstOffset = total,
                .size = end - start,
            });
            total += end - start;
        });
    }
    auto& side = *side_queue;
    if (total > SideQueue::StagingSize) {
        return false;
    }
    if (total != 0) {
        std::scoped_lock lk{side.mutex};
        Vulkan::Check(side.device.resetCommandPool(*side.pool));
        Vulkan::Check(side.cmdbuf.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}));
        side.cmdbuf.copyBuffer(arena->Handle(), side.staging.Handle(), copies);
        const vk::MemoryBarrier2 to_host = {
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        };
        side.cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_host});
        Vulkan::Check(side.cmdbuf.end());
        // The writing work signalled the timeline already; waiting on it orders its writes
        // before the copy.
        const vk::Semaphore timeline = scheduler.GetWorkSemaphore()->Handle();
        const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eTransfer;
        const vk::TimelineSemaphoreSubmitInfo timeline_info = {
            .waitSemaphoreValueCount = 1,
            .pWaitSemaphoreValues = &write_tick,
        };
        const vk::SubmitInfo submit = {
            .pNext = &timeline_info,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &timeline,
            .pWaitDstStageMask = &wait_stage,
            .commandBufferCount = 1,
            .pCommandBuffers = &side.cmdbuf,
        };
        Vulkan::Check(side.queue.submit(submit, *side.fence));
        Vulkan::Check(side.device.waitForFences(*side.fence, true, UINT64_MAX));
        Vulkan::Check(side.device.resetFences(*side.fence));
        side.staging.Invalidate(0, total);
        for (const auto& copy : copies) {
            memory->TryWriteBacking(std::bit_cast<u8*>(arena->cpu_addr + copy.srcOffset),
                                    side.staging.mapped_data.data() + copy.dstOffset, copy.size);
        }
    }
    for (const VAddr at : pages) {
        gpu_modified_ranges.Subtract(at, TRACKER_BYTES_PER_PAGE);
        memory_tracker->UnmarkRegionAsGpuModified(at, TRACKER_BYTES_PER_PAGE);
    }
    BbStats::gpu_side_download_ns.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                             start_time)
            .count(),
        std::memory_order_relaxed);
    return true;
}

// bbport: deferred raycaster copies (see landing_gate.h). The guest part of the copy is made at
// the dispatch; the GPU-written bytes of the source land in the destination on a worker thread
// once the GPU work that wrote them has completed, in registration order.
struct BufferCache::Landing {
    u64 seq{};
    u64 tick{};
    vk::Buffer buffer;
    boost::container::small_vector<vk::BufferCopy, 4> copies; ///< dstOffset: in the side staging
    boost::container::small_vector<VAddr, 4> targets;         ///< guest address of each copy
};

bool BufferCache::DeferGuestCopy(VAddr src, VAddr dst, u64 size) {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_SCALAR_COPY64_DEFER");
        return !(env && env[0] == '0');
    }();
    if (!enabled || !side_queue || land_stopping.load(std::memory_order_acquire) || size == 0 ||
        size > SideQueue::StagingSize / 2 || !memory->IsValidGpuMapping(src, size) ||
        !memory->IsValidGpuMapping(dst, size)) {
        return false;
    }
    const u64 first_block = src >> block_shift;
    const u64 last_block = (src + size - 1) >> block_shift;
    const Buffer* arena = GetArena(first_block, last_block);
    if (!arena || src < arena->cpu_addr || src + size > arena->cpu_addr + arena->size_bytes) {
        return false;
    }
    auto landing = std::make_shared<Landing>();
    landing->buffer = arena->Handle();
    u64 total = 0;
    gpu_modified_ranges.ForEachInRange(src, size, [&](VAddr start, VAddr end) {
        total = Common::AlignUp(total, 64);
        landing->copies.push_back(vk::BufferCopy{
            .srcOffset = start - arena->cpu_addr,
            .dstOffset = total,
            .size = end - start,
        });
        landing->targets.push_back(dst + (start - src));
        total += end - start;
    });
    if (landing->copies.empty() || total > SideQueue::StagingSize) {
        return false;
    }
    // The tick of the work that wrote the source. Everything is recorded by now (the dispatch
    // follows a drain of the draw pipe), so a write marked for the next tick is in the open
    // command buffer; that one is submitted here, so the tick completes without anybody waiting
    // for something else to submit it.
    u64 tick = memory_tracker->LatestGpuWriteTick(src, size);
    if (tick != 0) {
        tick = std::min(tick, scheduler.CurrentTick());
        if (tick >= scheduler.CurrentTick()) {
            scheduler.Flush();
        }
    }
    if (tick == 0) {
        return false; // page flags and written ranges disagree: nothing says when it lands
    }
    landing->tick = tick;
    // The guest part of the copy now; the GPU-written bytes follow when they have landed.
    memory->TryWriteBacking(reinterpret_cast<void*>(dst), reinterpret_cast<const void*>(src), size);
    {
        std::scoped_lock lk{landing_mutex};
        if (landing_stop) {
            return false;
        }
        landing->seq = ++landing_registered_seq;
        landing_registered.store(landing->seq, std::memory_order_release);
        landing_queue.push_back(std::move(landing));
        if (!landing_thread.joinable()) {
            landing_thread = std::thread([this] { LandingWorker(); });
        }
    }
    landing_cv.notify_one();
    return true;
}

/// Waits for the GPU to reach `tick`, in slices, so that the shutdown can cancel it. False when
/// cancelled or when the wait failed for a reason other than a lost device (which ends the game,
/// like the other GPU waits do).
bool BufferCache::WaitLandingTick(u64 tick) {
    auto& semaphore = *scheduler.GetWorkSemaphore();
    if (semaphore.IsFree(tick)) {
        return true;
    }
    semaphore.Refresh();
    if (semaphore.IsFree(tick)) {
        return true;
    }
    const vk::Device device = instance.GetDevice();
    const vk::Semaphore handle = semaphore.Handle();
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &handle,
        .pValues = &tick,
    };
    const auto begin = std::chrono::steady_clock::now();
    bool reported = false;
    while (!land_stopping.load(std::memory_order_acquire)) {
        const vk::Result result = device.waitSemaphores(&wait_info, 20'000'000ull);
        if (result == vk::Result::eSuccess) {
            semaphore.Refresh();
            return true;
        }
        if (result == vk::Result::eErrorDeviceLost) {
            Vulkan::DeviceLost(device);
        }
        if (result != vk::Result::eTimeout) {
            std::printf("Landing: waiting for tick %llu failed (%s); the copy lands without data\n",
                        static_cast<unsigned long long>(tick), vk::to_string(result).c_str());
            return false;
        }
        if (!reported && std::chrono::steady_clock::now() - begin > std::chrono::seconds(5)) {
            reported = true;
            std::printf("GPU stall: the landing thread waits 5 s for tick %llu\n",
                        static_cast<unsigned long long>(tick));
        }
    }
    return false;
}

bool BufferCache::ExecuteLanding(Landing& landing) {
    auto& side = *side_queue;
    if (!WaitLandingTick(landing.tick)) {
        return false;
    }
    u64 total = 0;
    for (const auto& copy : landing.copies) {
        total = std::max(total, copy.dstOffset + copy.size);
    }
    {
        std::scoped_lock lk{side.mutex};
        Vulkan::Check(side.device.resetCommandPool(*side.pool));
        Vulkan::Check(side.cmdbuf.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}));
        side.cmdbuf.copyBuffer(landing.buffer, side.staging.Handle(), landing.copies);
        const vk::MemoryBarrier2 to_host = {
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        };
        side.cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_host});
        Vulkan::Check(side.cmdbuf.end());
        const vk::Semaphore timeline = scheduler.GetWorkSemaphore()->Handle();
        const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eTransfer;
        const u64 wait_value = landing.tick;
        const vk::TimelineSemaphoreSubmitInfo timeline_info = {
            .waitSemaphoreValueCount = 1,
            .pWaitSemaphoreValues = &wait_value,
        };
        const vk::SubmitInfo submit = {
            .pNext = &timeline_info,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &timeline,
            .pWaitDstStageMask = &wait_stage,
            .commandBufferCount = 1,
            .pCommandBuffers = &side.cmdbuf,
        };
        Vulkan::Check(side.queue.submit(submit, *side.fence));
        // The tick has been reached, so the copy is short. In slices anyway: at shutdown it gets
        // half a second more.
        u32 slices_after_stop = 0;
        for (;;) {
            const vk::Result result = side.device.waitForFences(*side.fence, true, 20'000'000ull);
            if (result == vk::Result::eSuccess) {
                break;
            }
            if (result == vk::Result::eErrorDeviceLost) {
                Vulkan::DeviceLost(side.device);
            }
            if (result != vk::Result::eTimeout) {
                Vulkan::Check(result);
            }
            if (land_stopping.load(std::memory_order_acquire) && ++slices_after_stop > 25) {
                return false;
            }
        }
        Vulkan::Check(side.device.resetFences(*side.fence));
        side.staging.Invalidate(0, total);
        for (size_t i = 0; i < landing.copies.size(); ++i) {
            memory->TryWriteBacking(reinterpret_cast<void*>(landing.targets[i]),
                                    side.staging.mapped_data.data() + landing.copies[i].dstOffset,
                                    landing.copies[i].size);
        }
    }
    return true;
}

void BufferCache::LandingWorker() {
    Common::SetCurrentThreadName("bb:Landing");
    std::unique_lock lk{landing_mutex};
    for (;;) {
        landing_cv.wait(lk, [&] {
            return landing_stop || !landing_queue.empty() ||
                   (!gated_labels.empty() && gated_labels.front().seq <= landing_done_seq);
        });
        if (landing_stop) {
            break;
        }
        if (!gated_labels.empty() && gated_labels.front().seq <= landing_done_seq) {
            auto fn = std::move(gated_labels.front().fn);
            gated_labels.pop_front();
            gate_running = true;
            lk.unlock();
            fn();
            lk.lock();
            gate_running = false;
            gate_outstanding.fetch_sub(1, std::memory_order_release);
            continue;
        }
        const auto landing = std::move(landing_queue.front());
        landing_queue.pop_front();
        lk.unlock();
        // Lands without data when cancelled at shutdown or when the wait for the GPU failed:
        // the labels behind it go on either way.
        ExecuteLanding(*landing);
        lk.lock();
        landing_done_seq = landing->seq;
        landing_done.store(landing->seq, std::memory_order_release);
        landing_done_cv.notify_all();
    }
    // Shutdown: what is still pending is dropped, nobody waits for it any more.
    landing_queue.clear();
    gated_labels.clear();
    gate_outstanding.store(0, std::memory_order_release);
    landing_done_seq = landing_registered_seq;
    landing_done.store(landing_done_seq, std::memory_order_release);
    landing_done_cv.notify_all();
    landing_exited.store(true, std::memory_order_release);
}

BufferCache::~BufferCache() {
    StopLanding();
}

void BufferCache::StopLanding() {
    // Stop first: nothing new goes to the landing thread, and the waits on it (its wait for the
    // GPU, the guest-visible labels waiting behind a landing) let go.
    const auto stop_begin = std::chrono::steady_clock::now();
    land_stopping.store(true, std::memory_order_release);
    land_instance.store(nullptr, std::memory_order_release);
    u64 dropped_copies = 0;
    size_t dropped_labels = 0;
    {
        std::scoped_lock lk{landing_mutex};
        landing_stop = true;
        dropped_copies = landing_registered_seq - landing_done_seq;
        dropped_labels = gated_labels.size();
    }
    landing_cv.notify_all();
    landing_done_cv.notify_all();
    if (landing_thread.joinable()) {
        // The thread leaves its waits within a slice (20 ms). If it is stuck in the driver
        // anyway the process must still end: this runs from the exit of the game.
        for (int ms = 0; ms < 3000 && !landing_exited.load(std::memory_order_acquire); ++ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (landing_exited.load(std::memory_order_acquire)) {
            landing_thread.join();
            if (dropped_copies != 0 || dropped_labels != 0) {
                std::printf("Landing thread stopped: %llu copies and %zu labels were still pending "
                            "(%lld ms)\n",
                            static_cast<unsigned long long>(dropped_copies), dropped_labels,
                            static_cast<long long>(
                                std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - stop_begin)
                                    .count()));
            }
        } else {
            std::printf("Landing thread did not stop within 3 s: ending the process\n");
            std::fflush(nullptr);
            std::_Exit(0);
        }
    }
}

bool BufferCache::LandingGateActive() const {
    return PendingLanding() != 0 || gate_outstanding.load(std::memory_order_acquire) != 0;
}

u64 BufferCache::PendingLanding() const {
    const u64 registered = landing_registered.load(std::memory_order_acquire);
    return registered > landing_done.load(std::memory_order_acquire) ? registered : 0;
}

void BufferCache::WaitLanding(u64 seq) {
    if (seq == 0 || landing_done.load(std::memory_order_acquire) >= seq) {
        return;
    }
    std::unique_lock lk{landing_mutex};
    // Released at shutdown as well; a stall is reported, not ended (the landing thread waits for
    // the GPU, which ends the game when the device is lost).
    const auto done = [&] { return landing_stop || landing_done_seq >= seq; };
    if (!landing_done_cv.wait_for(lk, std::chrono::seconds(5), done)) {
        std::printf("GPU stall: a label waits 5 s for the landing of copy %llu\n",
                    static_cast<unsigned long long>(seq));
        landing_done_cv.wait(lk, done);
    }
}

void BufferCache::RunAfterLanding(u64 seq, std::function<void()> fn) {
    {
        std::unique_lock lk{landing_mutex};
        if (landing_stop || (seq <= landing_done_seq && gated_labels.empty() && !gate_running)) {
            lk.unlock();
            fn();
            return;
        }
        gated_labels.push_back({seq, std::move(fn)});
        gate_outstanding.fetch_add(1, std::memory_order_release);
    }
    landing_cv.notify_one();
}

namespace LandingGate {
u64 Pending() {
    auto* cache = land_instance.load(std::memory_order_acquire);
    return cache ? cache->PendingLanding() : 0;
}
bool Active() {
    auto* cache = land_instance.load(std::memory_order_acquire);
    return cache && cache->LandingGateActive();
}
void Wait(u64 seq) {
    if (auto* cache = land_instance.load(std::memory_order_acquire)) {
        cache->WaitLanding(seq);
    }
}
void Run(u64 seq, std::function<void()> fn) {
    if (auto* cache = land_instance.load(std::memory_order_acquire)) {
        cache->RunAfterLanding(seq, std::move(fn));
    } else {
        fn();
    }
}
bool Stopped() {
    return land_stopping.load(std::memory_order_acquire);
}
} // namespace LandingGate

void BufferCache::ForceUpload(VAddr device_addr, u64 size) {
    if (!IsRegionGpuModified(device_addr, size)) {
        memory_tracker->MarkRegionAsCpuModified(device_addr, size);
    }
}

namespace {
/// bbport: the guest frame (g_guest_frames) in which the GPU last bound each 4 KiB page of the
/// GPU heap window, read or written (BufferCache::ObtainBuffer). GPU data still in use, such as
/// buffers a compute shader made once and every frame draws from, is never taken for dead.
constexpr VAddr GpuUseLow = 0x1000000000ull, GpuUseHigh = 0x1200000000ull;
std::unique_ptr<u32[]> gpu_use_frame{new u32[(GpuUseHigh - GpuUseLow) >> 12]{}};

void NoteGpuUse(VAddr address, u64 size) {
    const VAddr begin = std::max(address, GpuUseLow);
    const VAddr end = std::min(address + size, GpuUseHigh);
    if (begin >= end) {
        return;
    }
    const u32 frame = g_guest_frames.load(std::memory_order_relaxed);
    for (u64 page = (begin - GpuUseLow) >> 12, last = (end - 1 - GpuUseLow) >> 12; page <= last;
         ++page) {
        gpu_use_frame[page] = frame;
    }
}

// bbport diagnosis: the latest resolutions of CPU writes to pages holding GPU data, printed on
// a GPU crash for the buffers the crashed command read (BB_GPU_CHECKPOINTS=1).
struct FaultEvent {
    VAddr page;
    u32 length;
    u32 frame;
    u32 gpu_write;
    u32 gpu_use;
    u32 kind; // 1 dropped, 2 merged, 3 read back
};
constexpr u64 FaultHistorySize = 1u << 16;
std::unique_ptr<FaultEvent[]> fault_history;
std::atomic<u64> fault_events{0};

void NoteFault(VAddr page, u64 length, u32 frame, u32 gpu_write, u32 gpu_use, u32 kind) {
    if (!fault_history) {
        return;
    }
    const u64 n = fault_events.fetch_add(1, std::memory_order_relaxed);
    fault_history[n % FaultHistorySize] = {page, static_cast<u32>(length), frame, gpu_write,
                                           gpu_use, kind};
}

/// The latest frame the GPU bound a page of the range; outside the window: now (in use).
u32 LastGpuUse(VAddr address, u64 size) {
    if (address < GpuUseLow || address + size > GpuUseHigh) {
        return g_guest_frames.load(std::memory_order_relaxed);
    }
    u32 latest = 0;
    for (u64 page = (address - GpuUseLow) >> 12, last = (address + size - 1 - GpuUseLow) >> 12;
         page <= last; ++page) {
        latest = std::max(latest, gpu_use_frame[page]);
    }
    return latest;
}
} // namespace

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks) {
    memory_tracker->InvalidateRegion(device_addr, size, [this, device_addr, size, assume_locks] {
        // bbport: a guest write to a page holding GPU data read that data back first, which
        // waits for the whole GPU queue (U3's Cartagena square: ~70 times a second, 5-20 ms each,
        // the writing game thread stopped; its cloth bones came out exploded). Now data from
        // GPU work that has completed is read back on a second queue, which waits for nothing
        // queued since (DownloadOnSideQueue, BB_SIDE_DOWNLOAD=0: off). Data the GPU is still
        // writing shares the page with the guest's new data (the same rings hand out
        // neighbouring blocks): the page becomes writable at once and the next upload merges
        // (ShadowGpuData, SynchronizeMemory). BB_GPU_MERGE=0: read back instead.
        // BB_DEAD_GPU_FRAMES=N (diagnosis): GPU data written and not bound for N guest frames is
        // dropped as dead. Wrong for state a solver keeps between runs and idles over a few
        // frames (U3's cloth at ~80 FPS: the GPU hung in its solver, device lost).
        static const u32 dead_frames = [] {
            const char* env = std::getenv("BB_DEAD_GPU_FRAMES");
            return env ? static_cast<u32>(std::strtoul(env, nullptr, 10)) : 0u;
        }();
        static const bool merge = [] {
            const char* env = std::getenv("BB_GPU_MERGE");
            return !(env && env[0] == '0');
        }();
        const VAddr page = Common::AlignDown(device_addr, TRACKER_BYTES_PER_PAGE);
        const u64 length = Common::AlignUp(device_addr + size, TRACKER_BYTES_PER_PAGE) - page;
        // Decided on the GPU thread, where GPU writes are recorded: none can come in between.
        auto resolve = [this, page, length, device_addr, size] {
            const u32 frames = g_guest_frames.load(std::memory_order_relaxed);
            const u32 gpu_write = memory_tracker->LatestGpuWriteFrame(page, length);
            const u32 gpu_use = LastGpuUse(page, length);
            if (DownloadOnSideQueue(page, length)) {
                memory_tracker->MarkRegionAsCpuModified(device_addr, size);
                BbStats::gpu_side_downloads.fetch_add(1, std::memory_order_relaxed);
                NoteFault(page, length, frames, gpu_write, gpu_use, 4);
                return;
            }
            if (dead_frames && frames >= dead_frames && gpu_write <= frames - dead_frames &&
                gpu_use <= frames - dead_frames) {
                gpu_modified_ranges.Subtract(page, length);
                memory_tracker->UnmarkRegionAsGpuModified(page, length);
                memory_tracker->MarkRegionAsCpuModified(device_addr, size);
                BbStats::dead_gpu_drops.fetch_add(1, std::memory_order_relaxed);
                NoteFault(page, length, frames, gpu_write, gpu_use, 1);
                return;
            }
            if (merge) {
                ShadowGpuData(page, length);
                memory_tracker->MarkRegionAsCpuModified(device_addr, size);
                BbStats::gpu_merges.fetch_add(1, std::memory_order_relaxed);
                NoteFault(page, length, frames, gpu_write, gpu_use, 2);
                return;
            }
            ReadMemory(device_addr, size, true, true);
            BbStats::gpu_read_backs.fetch_add(1, std::memory_order_relaxed);
            NoteFault(page, length, frames, gpu_write, gpu_use, 3);
        };
        if (assume_locks) {
            resolve();
        } else {
            liverpool->SendCommand<true>(std::move(resolve));
        }
    });
}

/// bbport diagnosis: starts the fault history (BB_GPU_CHECKPOINTS=1).
void EnableFaultHistory() {
    fault_history = std::make_unique<FaultEvent[]>(FaultHistorySize);
}

/// bbport diagnosis: the recorded fault resolutions on pages of [address, address + size),
/// newest first.
void PrintFaultHistory(VAddr address, u64 size) {
    if (!fault_history) {
        return;
    }
    static constexpr const char* kinds[] = {"?", "dropped", "merged", "read back",
                                            "read back on the side queue"};
    const u64 total = fault_events.load();
    const u64 oldest = total > FaultHistorySize ? total - FaultHistorySize : 0;
    const u32 now = g_guest_frames.load();
    u32 printed = 0;
    for (u64 n = total; n > oldest && printed < 12; --n) {
        const auto& e = fault_history[(n - 1) % FaultHistorySize];
        if (e.page < address + size && address < e.page + e.length) {
            std::printf("GPU crash:       page 0x%llx+0x%x %s %u frames ago (GPU write frame %u, "
                        "last bound %u, fault frame %u)\n",
                        static_cast<unsigned long long>(e.page), e.length, kinds[std::min<u32>(e.kind, 4)],
                        now - e.frame, e.gpu_write, e.gpu_use, e.frame);
            ++printed;
        }
    }
}

namespace {
/// bbport: who owns the bytes of a shadowed GPU-written range (ShadowGpuData) once the guest has
/// written the page. A byte the guest changed is its own; one it left as it was keeps the GPU's
/// data. Judged byte by byte, a guest store of the value already there (zero over zero, the
/// commonest) kept the GPU's bytes instead: U3's London pub reuses memory the GPU had filled
/// with 0xff, and a free-list link the game set to 0 read back as ffffffffffffffff (its
/// allocator faulted at 0xd9c59b), while the GPU read the same mixture and hung (device lost).
/// The guest writes whole objects, so a change anywhere in an aligned 64-byte block (one cache
/// line; GPU buffers are at least that aligned) makes the whole block the guest's.
/// fn(begin, end, guest_owned) gets the runs of shadow.bytes, as offsets.
constexpr u64 ShadowBlockBytes = 64;
std::atomic<u64> shadow_equal_bytes_claimed{0};
std::atomic<u32> shadow_claim_reports{0};

template <typename Shadow, typename Fn>
void ForEachShadowRun(const Shadow& shadow, Fn&& fn) {
    const auto* guest = reinterpret_cast<const u8*>(shadow.start);
    const size_t count = shadow.bytes.size();
    size_t run_begin = 0;
    bool run_owned = false;
    u64 equal_claimed = 0;
    for (size_t i = 0; i < count;) {
        const size_t end = std::min<size_t>(
            count, Common::AlignUp(shadow.start + i + 1, ShadowBlockBytes) - shadow.start);
        const bool owned = std::memcmp(guest + i, shadow.bytes.data() + i, end - i) != 0;
        if (owned) {
            for (size_t k = i; k < end; ++k) {
                equal_claimed += guest[k] == shadow.bytes[k];
            }
        }
        if (i != 0 && owned != run_owned) {
            fn(run_begin, i, run_owned);
            run_begin = i;
        }
        run_owned = owned;
        i = end;
    }
    if (count != 0) {
        fn(run_begin, count, run_owned);
    }
    if (equal_claimed != 0) {
        // BB_FRAME_STATS-free evidence that the old rule would have kept GPU bytes here.
        shadow_equal_bytes_claimed.fetch_add(equal_claimed, std::memory_order_relaxed);
        if (shadow_claim_reports.fetch_add(1, std::memory_order_relaxed) < 20) {
            std::printf("Buffer merge: guest data at 0x%llx (%zu bytes) kept %llu bytes it "
                        "rewrote unchanged (%llu so far)\n",
                        static_cast<unsigned long long>(shadow.start), count,
                        static_cast<unsigned long long>(equal_claimed),
                        static_cast<unsigned long long>(shadow_equal_bytes_claimed.load()));
        }
    }
}
} // namespace

void BufferCache::ShadowGpuData(VAddr page, u64 length) {
    std::scoped_lock lk{shadow_mutex};
    for (VAddr at = page; at < page + length; at += TRACKER_BYTES_PER_PAGE) {
        if (shadows.contains(at)) {
            continue;
        }
        std::vector<ShadowRange> list;
        gpu_modified_ranges.ForEachInRange(at, TRACKER_BYTES_PER_PAGE, [&](VAddr start, VAddr end) {
            const auto* bytes = reinterpret_cast<const u8*>(start);
            list.push_back({start, std::vector<u8>(bytes, bytes + (end - start))});
        });
        if (!list.empty()) {
            shadows.emplace(at, std::move(list));
            shadow_pages.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// bbport: the game fills its per-frame buffers (constants, skinning output) sequentially and
// every 4 KiB page cost a protection fault (~110k/s in Hunter's Nightmare, a fifth of each
// render worker's time in the kernel). A fault unprotects the aligned window around it instead;
// pages marked CPU-modified without being written only cost an upload when bound.
// Item: context = BufferCache, source = guest address, destination = host pointer, size,
// extra = the Buffer whose mapping holds the destination (flushed after the copy).
void BufferCache::RunGuestCopy(const BbCopy::Item& item) {
    auto* cache = static_cast<BufferCache*>(item.context);
    auto* dst = reinterpret_cast<u8*>(item.destination);
    cache->memory->CopySparseMemory(item.source, dst, item.size);
    auto* buffer = reinterpret_cast<Buffer*>(item.extra);
    buffer->Flush(dst - buffer->mapped_data.data(), item.size);
}

// Small guest copies run on the recording thread (it spins for work: no wakeup, and it is
// idle most of the time); PoolSmallCopies (toggle 524288) batches them for the copy threads.
void BufferCache::SmallGuestCopy(const BbCopy::Item& item) {
    if (scheduler.IsRecordingDeferred() && !BbToggle::Disabled(BbToggle::PoolSmallCopies)) {
        scheduler.RecordHostCopy([item] { item.run(item); });
        return;
    }
    BbCopy::QueueCopy(item);
}

void BufferCache::ExtendWriteFault(VAddr device_addr) {
    static const u64 window = [] {
        const char* env = std::getenv("BB_FAULT_WINDOW");
        const u64 kib = env ? std::strtoull(env, nullptr, 10) : 256;
        return std::bit_ceil(std::clamp<u64>(kib, 4, 1024)) * 1024;
    }();
    if (window <= TRACKER_BYTES_PER_PAGE || BbToggle::Disabled(BbToggle::FaultWindow)) {
        return;
    }
    memory_tracker->ExtendWriteFault(Common::AlignDown(device_addr, window), window);
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks) {
    const auto flush_request = [this, device_addr, size, is_write] {
        const u32 first_block = device_addr >> block_shift;
        const u32 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);

        // GPU-modified ranges come as many small scattered islands,
        // so the download is widened to a window around the request
        constexpr u64 WindowSize = 512_KB;
        const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
        const VAddr window_start =
            std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), arena->cpu_addr);
        const VAddr window_end = std::min<VAddr>(
            std::max<VAddr>(window_start + WindowSize, device_addr + size), arena_end);
        DownloadMemory(arena, window_start, window_end - window_start);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    };
    if (assume_locks) {
        flush_request();
    } else {
        liverpool->SendCommand<true>(std::move(flush_request));
    }
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    const VAddr arena_base = arena->cpu_addr;
    memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
        const auto add_download = [&](VAddr start, VAddr end) {
            const u64 new_offset = start - arena_base;
            const u64 new_size = end - start;
            copies.push_back(vk::BufferCopy{
                .srcOffset = new_offset,
                .dstOffset = total_size_bytes,
                .size = new_size,
            });
            // Align up to avoid cache conflicts
            constexpr u64 align = 64ULL;
            constexpr u64 mask = ~(align - 1ULL);
            total_size_bytes += (new_size + align - 1) & mask;
        };
        gpu_modified_ranges.ForEachInRange(address, size, add_download);
        gpu_modified_ranges.Subtract(address, size);
    });
    if (total_size_bytes == 0) {
        return;
    }
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    for (auto& copy : copies) {
        copy.dstOffset += download.offset;
    }
    runtime.CopyBuffer(arena, download.buffer, copies);
    scheduler.Finish();

    download.buffer->Invalidate(download.offset, download.size);
    // bbport diagnosis: BB_DOWNLOAD_CHECK=1 — GPU data written back over guest bytes that differ
    // and lie in pages the guest may have written since the last upload (CPU-modified).
    static const bool download_check = [] {
        const char* env = std::getenv("BB_DOWNLOAD_CHECK");
        return env && env[0] == '1';
    }();
    // Shadowed pages (ShadowGpuData) in the range: the blocks the guest changed since stay
    // (ForEachShadowRun), and the pages need no shadow afterwards (their GPU data is in guest
    // memory).
    std::vector<std::pair<VAddr, std::vector<u8>>> guest_bytes;
    if (shadow_pages.load(std::memory_order_relaxed)) {
        std::scoped_lock lk{shadow_mutex};
        for (VAddr page = Common::AlignDown(device_addr, TRACKER_BYTES_PER_PAGE);
             page < device_addr + size; page += TRACKER_BYTES_PER_PAGE) {
            const auto it = shadows.find(page);
            if (it == shadows.end()) {
                continue;
            }
            for (const auto& shadow : it->second) {
                const auto* guest = reinterpret_cast<const u8*>(shadow.start);
                ForEachShadowRun(shadow, [&](size_t begin, size_t end, bool guest_owned) {
                    if (guest_owned) {
                        guest_bytes.emplace_back(shadow.start + begin,
                                                 std::vector<u8>(guest + begin, guest + end));
                    }
                });
            }
            shadows.erase(it);
            shadow_pages.fetch_sub(1, std::memory_order_relaxed);
        }
    }
    for (const auto& copy : copies) {
        auto* dst_addr = std::bit_cast<u8*>(arena_base + copy.srcOffset);
        const u8* data = download.mapped + (copy.dstOffset - download.offset);
        if (download_check) {
            const VAddr address = reinterpret_cast<VAddr>(dst_addr);
            const bool cpu_modified = memory_tracker->IsRegionCpuModified(address, copy.size);
            const bool differs = std::memcmp(dst_addr, data, copy.size) != 0;
            static std::atomic<u32> reports{0};
            if (differs && cpu_modified && reports.fetch_add(1) < 300) {
                std::printf("Download check: %#llx (%llu bytes) written back over CPU-modified "
                            "guest data that differs (request %#llx, %llu bytes)\n",
                            static_cast<unsigned long long>(address),
                            static_cast<unsigned long long>(copy.size),
                            static_cast<unsigned long long>(device_addr),
                            static_cast<unsigned long long>(size));
            }
        }
        memory->TryWriteBacking(dst_addr, data, copy.size);
    }
    for (const auto& [address, bytes] : guest_bytes) {
        memory->TryWriteBacking(reinterpret_cast<void*>(address), bytes.data(), bytes.size());
    }
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
}

namespace {
// bbport: BB_BUFFER_STATS=1 — how buffer bindings reach the GPU, by guest region (256 MiB),
// printed every 5 s: small read-only copies into the stream buffer, arena bindings, and the
// bytes those re-upload after CPU writes. Input for engine-level short paths.
struct BufferStats {
    struct Region {
        u64 stream_count{}, stream_bytes{}, arena_count{}, arena_bytes{}, upload_bytes{};
    };
    std::map<u64, Region> regions;
    /// Hot window: frame of the last binding per 64 KiB block, and the distribution of the
    /// frames between uses (1, 2, 3, 4, 5-8, 9-16, 17+): the ring's reuse distance.
    std::unordered_map<u64, u64> last_use;
    std::array<u64, 7> reuse{};
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
    u64 calls = 0;
};
/// 256 MiB regions; 1 MiB inside the hot 0x104xxxxxxx window (the engine's frame data).
BufferStats& Stats();
void NoteHotUse(VAddr address, u32 size) {
    if ((address >> 28) != 0x104) {
        return;
    }
    auto& st = Stats();
    const u64 frame = BbStats::gpu_frames.load(std::memory_order_relaxed);
    for (u64 block = address >> 16; block <= (address + size - 1) >> 16; ++block) {
        auto [it, inserted] = st.last_use.try_emplace(block, frame);
        if (!inserted && it->second != frame) {
            const u64 d = frame - it->second;
            ++st.reuse[d <= 4 ? d - 1 : d <= 8 ? 4 : d <= 16 ? 5 : 6];
            it->second = frame;
        }
    }
}
u64 RegionKey(VAddr address) {
    return (address >> 28) == 0x104 ? (address >> 20) | (1ull << 40) : address >> 28;
}
bool BufferStatsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_BUFFER_STATS");
        return value && value[0] == '1';
    }();
    return enabled;
}
BufferStats& Stats() {
    static BufferStats stats;
    return stats;
}
void PrintBufferStats() {
    auto& st = Stats();
    if ((++st.calls & 4095) != 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - st.window).count();
    if (seconds < 5.0) {
        return;
    }
    std::printf("Buffer stats (%.1f s, per second): region, stream copies/bytes, arena bindings, "
                "re-uploaded bytes\n", seconds);
    std::printf("  hot window reuse distance in frames (1,2,3,4,5-8,9-16,17+): %llu %llu %llu %llu "
                "%llu %llu %llu\n",
                (unsigned long long)st.reuse[0], (unsigned long long)st.reuse[1],
                (unsigned long long)st.reuse[2], (unsigned long long)st.reuse[3],
                (unsigned long long)st.reuse[4], (unsigned long long)st.reuse[5],
                (unsigned long long)st.reuse[6]);
    st.reuse = {};
    for (const auto& [region, r] : st.regions) {
        const u64 base = (region >> 40) ? (region & ((1ull << 40) - 1)) << 20 : region << 28;
        if ((region >> 40) && r.stream_bytes + r.upload_bytes < 1e6 * seconds) {
            continue; // quiet MiB of the hot window
        }
        std::printf("  %#012llx: %8.0f copies %8.2f MB, %8.0f arena %8.2f MB bound, %8.2f MB uploads\n",
                    static_cast<unsigned long long>(base), r.stream_count / seconds,
                    r.stream_bytes / seconds / 1e6, r.arena_count / seconds,
                    r.arena_bytes / seconds / 1e6,
                    r.upload_bytes / seconds / 1e6);
    }
    st.regions.clear();
    st.window = now;
}
} // namespace

namespace {
// bbport diagnosis: BB_STABLE_CHECK=1 — guest data bound read-only should not change before the
// end-of-pipe fence after the draws that read it. A sample of each binding (four 64-byte
// pieces) is hashed when it is bound and again at that fence; ranges that changed are printed:
// the guest wrote them while the GPU could still read them, or finished them only after.
struct StableBinding {
    u32 size;
    u64 hash;
};
std::mutex stable_mutex;
std::unordered_map<VAddr, StableBinding> stable_bindings;
u64 stable_checked = 0, stable_changed = 0, stable_printed = 0;
auto stable_window = std::chrono::steady_clock::now();

bool StableCheckEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_STABLE_CHECK");
        return env && env[0] == '1';
    }();
    return enabled;
}

u64 SampleHash(VAddr address, u32 size) {
    const auto* bytes = reinterpret_cast<const u8*>(address);
    const u32 piece = std::min<u32>(size, 64);
    const u32 offsets[4] = {0, size / 3, size / 3 * 2, size - piece};
    u64 hash = 1469598103934665603ull;
    for (const u32 offset : offsets) {
        for (u32 i = 0; i < piece; ++i) {
            hash = (hash ^ bytes[offset + i]) * 1099511628211ull;
        }
    }
    return hash;
}
} // namespace

// bbport diagnosis: BB_CLOBBER_CHECK=1 — guest memory copied to the GPU over bytes the GPU wrote
// that were never read back (skinned vertices: guest memory holds zeros there).
static bool ClobberCheckEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_CLOBBER_CHECK");
        return env && env[0] == '1';
    }();
    return enabled;
}

static void ReportClobber(const char* path, VAddr address, u64 size, VAddr binding, u64 binding_size,
                          bool is_written) {
    static std::atomic<u32> reports{0};
    if (reports.fetch_add(1, std::memory_order_relaxed) < 300) {
        std::printf("Clobber check: %s %#llx (%llu bytes) over GPU-written data, binding %#llx "
                    "(%llu bytes, %s)\n",
                    path, static_cast<unsigned long long>(address),
                    static_cast<unsigned long long>(size), static_cast<unsigned long long>(binding),
                    static_cast<unsigned long long>(binding_size), is_written ? "written" : "read");
    }
}

void BufferCache::CheckStableBindings() {
    if (!StableCheckEnabled()) {
        return;
    }
    std::scoped_lock lk{stable_mutex};
    for (const auto& [address, binding] : stable_bindings) {
        ++stable_checked;
        if (!memory->IsValidGpuMapping(address, binding.size) ||
            IsRegionGpuModified(address, binding.size)) {
            continue;
        }
        if (SampleHash(address, binding.size) != binding.hash) {
            ++stable_changed;
            if (stable_printed++ < 300) {
                std::printf("Stable check: %#llx (%u bytes) changed before its fence\n",
                            static_cast<unsigned long long>(address), binding.size);
            }
        }
    }
    stable_bindings.clear();
    const auto now = std::chrono::steady_clock::now();
    if (now - stable_window >= std::chrono::seconds(5)) {
        std::printf("Stable check: %llu of %llu bindings changed before their fence (5 s)\n",
                    static_cast<unsigned long long>(stable_changed),
                    static_cast<unsigned long long>(stable_checked));
        stable_checked = stable_changed = 0;
        stable_window = now;
    }
}

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer) {
    const bool stats = BufferStatsEnabled();
    if (stats) {
        PrintBufferStats();
        NoteHotUse(device_addr, size);
    }
    NoteGpuUse(device_addr, size);
    if (StableCheckEnabled() && !is_written && size >= 16 &&
        memory->IsValidGpuMapping(device_addr, size) && !IsRegionGpuModified(device_addr, size)) {
        std::scoped_lock lk{stable_mutex};
        stable_bindings.try_emplace(device_addr, StableBinding{size, SampleHash(device_addr, size)});
    }
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && size <= STREAM_THRESHOLD && !IsRegionGpuModified(device_addr, size)) {
        if (ClobberCheckEnabled() && gpu_modified_ranges.Intersects(device_addr, size)) {
            ReportClobber("stream copy", device_addr, size, device_addr, size, false);
        }
        if (stats) {
            auto& region = Stats().regions[RegionKey(device_addr)];
            ++region.stream_count;
            region.stream_bytes += size;
        }
        // bbport: the guest data is copied on a copy thread, started now; submission and
        // guest-visible fences wait for it (Scheduler::WaitHostCopies).
        if (!stream_buffer.mapped_data.empty() &&
            !BbToggle::Disabled(BbToggle::DeferredStreamCopies)) {
            if (const auto offset = stream_buffer.Reserve(size, instance.UniformMinAlignment())) {
                SmallGuestCopy({
                    .run = &RunGuestCopy,
                    .context = this,
                    .source = device_addr,
                    .destination = reinterpret_cast<u64>(stream_buffer.mapped_data.data() + *offset),
                    .size = size,
                    .extra = reinterpret_cast<u64>(static_cast<Buffer*>(&stream_buffer)),
                });
                return {&stream_buffer, *offset};
            }
        }
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        return {&stream_buffer, offset};
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    if (device_addr < arena->cpu_addr || device_addr + size > arena->cpu_addr + arena->size_bytes) {
        std::printf("Buffer cache: request 0x%llx+0x%x outside its arena 0x%llx-0x%llx\n",
                    static_cast<unsigned long long>(device_addr), size,
                    static_cast<unsigned long long>(arena->cpu_addr),
                    static_cast<unsigned long long>(arena->cpu_addr + arena->size_bytes));
    }
    EnsureResident(arena, first_block, last_block);
    const u64 uploaded_before = BbStats::buffer_upload_bytes.load(std::memory_order_relaxed);
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    if (stats) {
        auto& region = Stats().regions[RegionKey(device_addr)];
        ++region.arena_count;
        region.arena_bytes += size;
        region.upload_bytes +=
            BbStats::buffer_upload_bytes.load(std::memory_order_relaxed) - uploaded_before;
    }
    if (is_written) {
        gpu_modified_ranges.Add(device_addr, size);
    }
    return {arena, arena->Offset(device_addr)};
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    if (IsRegionGpuModified(device_addr, size)) {
        return ObtainBuffer(device_addr, size, false);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    if (!BbToggle::Disabled(BbToggle::DeferredUploads)) {
        // bbport: texture data is copied on the copy threads (streaming: 100+ MB per frame);
        // the upload reads the staging only after submission, which waits for the copies.
        constexpr u64 Chunk = 1_MB;
        for (u64 offset = 0; offset < staging.size; offset += Chunk) {
            const BbCopy::Item item{
                .run = &RunGuestCopy,
                .context = this,
                .source = device_addr + offset,
                .destination = reinterpret_cast<u64>(staging.mapped + offset),
                .size = std::min(Chunk, staging.size - offset),
                .extra = reinterpret_cast<u64>(staging.buffer),
            };
            if (staging.size < Chunk) {
                SmallGuestCopy(item);
            } else {
                BbCopy::Async([item] { item.run(item); });
            }
        }
        return {staging.buffer, staging.offset};
    }
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

void BufferCache::ProcessFaultBuffer() {
    fault_manager->ProcessFaultBuffer();
}

void BufferCache::SynchronizeDmaBuffers() {
    for (const auto& range : resident_ranges) {
        const u64 page = range.start >> (arena_page_bits - block_shift);
        const VAddr device_addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        SynchronizeMemory(address_space[page], device_addr, size, false, false);
    }
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    ASSERT_MSG(last_page - first_page <= 1,
               "Buffer request cannot span more than two VA arena pages");

    const auto* first_arena = address_space[first_page];
    const auto* last_arena = address_space[last_page];
    if (first_arena == last_arena) {
        if (!first_arena) {
            const u64 base_block = Common::AlignDownPow2<u64>(first_block, blocks_per_arena_page);
            const u64 num_pages = last_page - first_page + 1;
            const auto* new_arena =
                &arenas.emplace_back(instance, base_block << block_shift,
                                     num_pages << arena_page_bits, MemoryType::Sparse);
            address_space[first_page] = new_arena;
            address_space[last_page] = new_arena;
        }
        return address_space[first_page];
    }

    std::printf("Buffer cache: migrating arena for blocks 0x%llx-0x%llx\n",
                static_cast<unsigned long long>(first_block << block_shift),
                static_cast<unsigned long long>((last_block + 1) << block_shift));

    const u64 first_addr = first_arena ? first_arena->cpu_addr : (first_page << arena_page_bits);
    const u64 first_size = first_arena ? first_arena->size_bytes : arena_page_size;
    const u64 last_size = last_arena ? last_arena->size_bytes : arena_page_size;

    const u64 base_block = first_addr >> block_shift;
    const u64 total_size = first_size + last_size;
    const u64 end_block = (first_addr + total_size) >> block_shift;
    auto* new_arena = &arenas.emplace_back(instance, first_addr, total_size, MemoryType::Sparse);
    auto* bind = BindsForArena(new_arena);
    resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
        const u64 start = std::max(base_block, backing.start);
        const u64 end = std::min(end_block, backing.end);
        bind->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = (start - base_block) << block_shift,
            .size = (end - start) << block_shift,
            .memory = backing.memory,
            .memoryOffset = backing.offset + ((start - backing.start) << block_shift),
        });
    });

    u64 base_page = first_addr >> arena_page_bits;
    for (u32 page = 0; page < (first_size >> arena_page_bits); ++page) {
        address_space[base_page + page] = new_arena;
    }
    base_page = last_page;
    for (u32 page = 0; page < (last_size >> arena_page_bits); ++page) {
        address_space[base_page + page] = new_arena;
    }
    return new_arena;
}

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block) {
    u32 resident_blocks{};
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) {
        resident_blocks += end - start;
        bind_ranges.Add({start, end});
    });

    if (bind_ranges.Empty()) {
        return;
    }
    BbStats::Timer timer{BbStats::t_resident};

    const vk::MemoryAllocateInfo alloc_info = {
        .allocationSize = resident_blocks << block_shift,
        .memoryTypeIndex = arena_memory_type_index,
    };
    const auto device_memory = Vulkan::Check(instance.GetDevice().allocateMemory(alloc_info));
    static const bool arena_log = [] {
        const char* env = std::getenv("BB_ARENA_LOG");
        return env && env[0] == '1';
    }();
    if (arena_log) {
        char thread_name[64] = "?";
#ifdef _WIN32
        PWSTR description = nullptr;
        if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &description)) && description) {
            WideCharToMultiByte(CP_UTF8, 0, description, -1, thread_name, sizeof(thread_name), nullptr,
                                nullptr);
            LocalFree(description);
        }
#endif
        std::printf("Arena: thread '%s'\n", thread_name);
        std::printf("Arena: memory 0x%llx (%u blocks) for guest 0x%llx-0x%llx, arena 0x%llx, %s, "
                    "pending binds %zu\n",
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(
                        static_cast<VkDeviceMemory>(device_memory))),
                    resident_blocks, static_cast<unsigned long long>(first_block << block_shift),
                    static_cast<unsigned long long>((last_block + 1) << block_shift),
                    static_cast<unsigned long long>(arena->cpu_addr),
                    AmdGpu::Liverpool::OnProcessorThread() ? "GPU thread" : "OTHER THREAD",
                    pending_binds.size());
    }

    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        staging_pool.Request(resident_blocks * sizeof(vk::DeviceAddress), MemoryType::HostUncached);

    u64 memory_offset{};
    ArenaBinds* binds = BindsForArena(arena);
    auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    u64 offset = staging.offset;
    for (const auto& range : bind_ranges) {
        Backing backing;
        backing.start = range.start;
        backing.end = range.end;
        backing.memory = device_memory;
        backing.offset = memory_offset;
        resident_ranges.Add(backing);

        LOG_INFO(Render, "Making range start={}, end={} resident", backing.start, backing.end);

        const auto& bind = binds->binds.emplace_back(vk::SparseMemoryBind{
            .resourceOffset = (range.start << block_shift) - arena->cpu_addr,
            .size = (range.end - range.start) << block_shift,
            .memory = device_memory,
            .memoryOffset = memory_offset,
        });
        if ((range.start << block_shift) < arena->cpu_addr ||
            bind.resourceOffset + bind.size > arena->size_bytes) {
            std::printf("Buffer cache: resident range 0x%llx-0x%llx outside its arena 0x%llx-0x%llx "
                        "(request blocks 0x%llx-0x%llx)\n",
                        static_cast<unsigned long long>(range.start << block_shift),
                        static_cast<unsigned long long>(range.end << block_shift),
                        static_cast<unsigned long long>(arena->cpu_addr),
                        static_cast<unsigned long long>(arena->cpu_addr + arena->size_bytes),
                        static_cast<unsigned long long>(first_block << block_shift),
                        static_cast<unsigned long long>((last_block + 1) << block_shift));
        }
        memory_offset += bind.size;

        for (u32 block = 0; block < bind.size; block += block_size) {
            *(bda_addrs++) = arena->BufferDeviceAddress() + bind.resourceOffset + block;
        }
        const u64 copy_size = (backing.end - backing.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, backing.start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
    }

    staging.Flush();
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    const Buffer* src_buffer{};
    const auto add_upload = [&](VAddr addr, u64 size) {
        if (ClobberCheckEnabled() && gpu_modified_ranges.Intersects(addr, size)) {
            ReportClobber("upload", addr, size, device_addr, size, is_written);
        }
        copies.emplace_back(total_size_bytes, addr, size);
        total_size_bytes += size;
    };
    memory_tracker->ForEachUploadRange(
        device_addr, size, is_written,
        [&](u64 addr, u64 size) {
            if (shadow_pages.load(std::memory_order_relaxed) == 0) {
                add_upload(addr, size);
                return;
            }
            // Shadowed pages (ShadowGpuData): blocks of GPU-written ranges the guest left as they
            // were keep the GPU's data; the blocks it changed are its data now (ForEachShadowRun).
            std::scoped_lock lk{shadow_mutex};
            const VAddr end = addr + size;
            VAddr cursor = addr;
            for (VAddr page = addr; page < end; page += TRACKER_BYTES_PER_PAGE) {
                const auto it = shadows.find(page);
                if (it == shadows.end()) {
                    continue;
                }
                for (const auto& shadow : it->second) {
                    ForEachShadowRun(shadow, [&](size_t i, size_t j, bool guest_owned) {
                        if (guest_owned) {
                            gpu_modified_ranges.Subtract(shadow.start + i, j - i);
                            return;
                        }
                        const VAddr skip = shadow.start + i;
                        if (skip > cursor) {
                            add_upload(cursor, skip - cursor);
                        }
                        cursor = shadow.start + j;
                    });
                }
                shadows.erase(it);
                shadow_pages.fetch_sub(1, std::memory_order_relaxed);
            }
            if (end > cursor) {
                add_upload(cursor, end - cursor);
            }
        },
        [&] { src_buffer = UploadCopies(arena, copies, total_size_bytes); });

    if (src_buffer) {
        runtime.CopyBuffer(src_buffer, arena, copies);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    return false;
}

const Buffer* BufferCache::UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                                        size_t total_size_bytes) {
    if (copies.empty()) {
        return nullptr;
    }
    BbStats::buffer_upload_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
    const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
    // bbport: the guest memory is copied into staging on the copy threads, started now in
    // groups of about 1 MiB; submission and guest-visible fences wait for them
    // (Scheduler::WaitHostCopies).
    if (!BbToggle::Disabled(BbToggle::DeferredUploads) && total_size_bytes < 1_MB) {
        // Small uploads join the calling thread's batch.
        for (auto& copy : copies) {
            SmallGuestCopy({
                .run = &RunGuestCopy,
                .context = this,
                .source = copy.dstOffset,
                .destination = reinterpret_cast<u64>(staging.mapped + copy.srcOffset),
                .size = copy.size,
                .extra = reinterpret_cast<u64>(staging.buffer),
            });
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        return staging.buffer;
    }
    if (!BbToggle::Disabled(BbToggle::DeferredUploads)) {
        struct HostCopy {
            VAddr source;
            u8* destination;
            u64 size;
        };
        using Group = boost::container::small_vector<HostCopy, 8>;
        auto group = std::make_shared<Group>();
        u64 group_bytes = 0;
        const auto launch = [&] {
            BbCopy::Async([group, memory = memory, staging] {
                for (const auto& copy : *group) {
                    memory->CopySparseMemory(copy.source, copy.destination, copy.size);
                }
                staging.Flush();
            });
        };
        for (auto& copy : copies) {
            group->push_back({copy.dstOffset, staging.mapped + copy.srcOffset, copy.size});
            group_bytes += copy.size;
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
            if (group_bytes >= 1_MB) {
                launch();
                group = std::make_shared<Group>();
                group_bytes = 0;
            }
        }
        if (!group->empty()) {
            launch();
        }
        return staging.buffer;
    }
    const auto copy = [&](std::size_t i) {
        memory->CopySparseMemory(copies[i].dstOffset, staging.mapped + copies[i].srcOffset,
                                 copies[i].size);
    };
    if (total_size_bytes >= 2_MB && copies.size() > 1) {
        BbCopy::ParallelFor(copies.size(), copy);
    } else {
        for (std::size_t i = 0; i < copies.size(); ++i) {
            copy(i);
        }
    }
    for (auto& copy : copies) {
        copy.srcOffset += staging.offset;
        copy.dstOffset -= arena->cpu_addr;
    }
    staging.Flush();
    return staging.buffer;
}

bool BufferCache::SyncImageToBuffer(VAddr addr, u32 size) {
    if (!texture_cache.FindImageFromRange(addr, size)) {
        return false;
    }
    const auto [arena, offset] = ObtainBuffer(addr, size, true);
    return SynchronizeMemoryFromImage(arena, addr, size);
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size) {
    if (auto type = texture_cache.IsMeta(device_addr)) {
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, ZmaskUncompressed);
            return true;
        } else {
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {}", magic_enum::enum_name(*type));
        }
    }
    // bbport: most texel buffers alias no image; remember misses until images change.
    const u64 generation = texture_cache.RegistryGeneration();
    auto& miss = image_miss_cache[((device_addr >> 6) ^ size * 0x9E3779B1u) % image_miss_cache.size()];
    if (miss.address == device_addr && miss.size == size && miss.generation == generation &&
        !BbToggle::Disabled(BbToggle::TextureBindingMemo)) {
        return false;
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        miss = {device_addr, size, generation};
        return false;
    }
    // bbport: the lookups above only read what the texture binding helper leaves alone; the
    // copy below changes image state, so the helper finishes first.
    runtime.BeforeImageAccess();
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u64 arena_offset = arena->Offset(device_addr);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset);
    return true;
}

void BufferCache::DumpArenas() const {
    std::printf("GPU crash: BDA page table at GPU 0x%llx, %zu arenas, block %u bytes\n",
                static_cast<unsigned long long>(bda_pagetable_buffer->BufferDeviceAddress()),
                arenas.size(), 1u << block_shift);
    for (const auto& arena : arenas) {
        const u64 gpu = arena.BufferDeviceAddress();
        std::printf("GPU crash: arena guest 0x%llx-0x%llx = GPU 0x%llx-0x%llx\n",
                    static_cast<unsigned long long>(arena.cpu_addr),
                    static_cast<unsigned long long>(arena.cpu_addr + arena.size_bytes),
                    static_cast<unsigned long long>(gpu),
                    static_cast<unsigned long long>(gpu + arena.size_bytes));
    }
    u64 ranges = 0;
    resident_ranges.ForEachInRange(0, ~u64{0} >> block_shift, [&](const Backing& backing) {
        if (ranges++ < 64) {
            std::printf("GPU crash: resident guest 0x%llx-0x%llx\n",
                        static_cast<unsigned long long>(backing.start << block_shift),
                        static_cast<unsigned long long>(backing.end << block_shift));
        }
    });
    std::printf("GPU crash: %llu resident ranges\n", static_cast<unsigned long long>(ranges));
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    if (pending_binds.empty()) {
        return;
    }

    std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
    buffer_binds.reserve(pending_binds.size());

    for (const auto& binds : pending_binds) {
        buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
            .buffer = binds.arena->Handle(),
            .bindCount = static_cast<u32>(binds.binds.size()),
            .pBinds = binds.binds.data(),
        });
    }

    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signal_tick,
    };

    const vk::BindSparseInfo sparse_info = {
        .pNext = &timeline_si,
        .bufferBindCount = static_cast<u32>(buffer_binds.size()),
        .pBufferBinds = buffer_binds.data(),
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal_sema,
    };

    info.AddWait(signal_sema, signal_tick);
    // bbport: BB_SPARSE_IDLE=1: the arena's page mappings change only while the GPU runs none of
    // the work submitted before (diagnosis of device-lost faults in freshly bound memory).
    static const bool sparse_idle = [] {
        const char* env = std::getenv("BB_SPARSE_IDLE");
        return env && env[0] == '1';
    }();
    if (sparse_idle) {
        // Called inside Scheduler::SubmitExecution (queue lock held), so the queue is ours.
        (void)instance.GetGraphicsQueue().waitIdle();
    }
    auto submit_result = instance.GetGraphicsQueue().bindSparse(sparse_info);
    if (submit_result == vk::Result::eErrorDeviceLost) {
        std::printf("Device lost during a sparse bind\n");
        Vulkan::DeviceLost(instance.GetDevice());
    }
    static const bool arena_log = [] {
        const char* env = std::getenv("BB_ARENA_LOG");
        return env && env[0] == '1';
    }();
    if (arena_log) {
        for (const auto& binds : pending_binds) {
            for (const auto& bind : binds.binds) {
                std::printf("Arena: bound memory 0x%llx+0x%llx at arena 0x%llx offset 0x%llx size 0x%llx "
                            "(tick %llu, %s)\n",
                            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(
                                static_cast<VkDeviceMemory>(bind.memory))),
                            static_cast<unsigned long long>(bind.memoryOffset),
                            static_cast<unsigned long long>(binds.arena->cpu_addr),
                            static_cast<unsigned long long>(bind.resourceOffset),
                            static_cast<unsigned long long>(bind.size),
                            static_cast<unsigned long long>(signal_tick),
                            vk::to_string(submit_result).c_str());
            }
        }
    }

    pending_binds.clear();
}

} // namespace VideoCore
