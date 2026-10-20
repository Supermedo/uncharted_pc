// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <xxhash.h>

#include "bbport_toggles.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/hash.h"
#include "common/scope_exit.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/texture_cache/tile_manager.h"

namespace VideoCore {

static constexpr u64 PageShift = 12;
static constexpr u64 NumFramesBeforeRemoval = 32;

namespace {
bool EnvFlag(const char* name, bool fallback) {
    const char* env = std::getenv(name);
    return env && env[0] ? env[0] != '0' : fallback;
}

/// bbport (#360, from the Linux fork 4f5a95a + e0baa3b): MaybeCpuDirty checks hash the same
/// guest range at marking and at refresh, and uploads of images the GPU may write record the
/// guest memory they saw, so a later CPU write elsewhere in their pages no longer reloads stale
/// guest memory over GPU-written contents. BB_TEX_GPU_HASH=0: the old hashing (A/B tests).
bool GpuHashFix() {
    static const bool value = EnvFlag("BB_TEX_GPU_HASH", true);
    return value;
}

/// bbport diagnosis (#360): BB_TEX_KEEP_GPU=1: a GPU-modified image that no GPU buffer write
/// made dirty is never reloaded from guest memory, whatever the CPU wrote there. If dropped
/// weapons look right with it, a reload over GPU-written contents is what made them white
/// (black without read backs). Not for normal play: a texture the game streams into memory a
/// render target used before keeps the render target's contents.
bool KeepGpuImages() {
    static const bool value = EnvFlag("BB_TEX_KEEP_GPU", false);
    return value;
}

/// bbport (#360): BB_GC_KEEP_GPU=1 (run.py sets it for Uncharted 2): GPU-modified images a
/// CPU write made dirty (not safe to write back) were evicted by the collector like any
/// texture, unused for 16 submissions, and their contents were lost: the next use rebuilt them
/// from guest memory, where the mip hashes would have kept the mips the CPU did not change.
/// They stay while VRAM is not under pressure; under pressure they go as before, tiled ones too
/// (kept for good, they would only ever grow VRAM: memory the game reuses for other data never
/// makes them clean again). GPU-dirty ones go as before too: their next use reloads them whole
/// from the buffer cache, as a new image would. Off: evicted as before.
bool GcKeepGpuImages() {
    static const bool value = EnvFlag("BB_GC_KEEP_GPU", false);
    return value;
}

/// bbport diagnosis (#360): BB_TEX_DIAG=1 prints how GPU-written data reaches textures (events
/// below); BB_TEX_DIAG=<hex address>[:<hex size>] only for images overlapping that range. A
/// line names the image (address+size, format, size, tile mode, pitch, flags: c CPU-dirty,
/// m maybe CPU-dirty, g GPU-dirty, G GPU-modified; usage: t texture, s storage, r render
/// target, d depth). Each event prints at most 3 lines per image, 3000 in all, and a count of
/// every event each 5 s.
struct TexDiagState {
    bool on = false;
    VAddr begin = 0;
    VAddr end = ~VAddr{0};
};
const TexDiagState& TexDiagConfig() {
    static const TexDiagState state = [] {
        TexDiagState config{};
        const char* env = std::getenv("BB_TEX_DIAG");
        if (!env || !env[0] || std::string_view{env} == "0") {
            return config;
        }
        config.on = true;
        if (std::string_view{env} != "1") {
            char* end = nullptr;
            config.begin = std::strtoull(env, &end, 16);
            const u64 size = end && *end == ':' ? std::strtoull(end + 1, nullptr, 16) : 1;
            config.end = config.begin + std::max<u64>(size, 1);
        }
        return config;
    }();
    return state;
}
bool TexDiagFor(VAddr address, u64 size) {
    const auto& config = TexDiagConfig();
    return config.on && address < config.end && config.begin < address + size;
}
enum TexDiagEvent : u32 {
    UploadOverGpu, ///< guest memory or a GPU buffer reloaded over a GPU-modified image
    UploadFill,    ///< the guest bytes uploaded to an image are all 0xff or all zero
    AliasMiss,     ///< an image created over a GPU-modified one it did not reuse
    StorageMiss,   ///< a storage-buffer write over an image that does not start at the buffer
    RangeSkip,     ///< a GPU buffer write over part of a GPU-modified image left it as it was
    FillGpu,       ///< a DMA fill on the GPU made an image reload
    BackingDirty,  ///< read back data written under an image made it reload (960e0d0)
    GcDrop,        ///< the collector evicted a GPU-modified image without writing it back
    GcKeep,        ///< the collector kept a GPU-modified dirty image (BB_GC_KEEP_GPU)
    NumTexDiagEvents,
};
constexpr std::array<const char*, NumTexDiagEvents> TexDiagNames = {
    "upload over GPU-written image", "upload of fill-like guest bytes",
    "new image over a GPU-written one", "storage write missed image",
    "partial GPU write left image", "DMA fill reloads image", "read back reloads image",
    "GC dropped GPU contents", "GC kept GPU-written dirty image",
};
std::array<std::atomic<u64>, NumTexDiagEvents> tex_diag_counts{};

/// Counts the event; true when a line may be printed for it (key: an image uid or address).
bool TexDiagNote(TexDiagEvent event, u64 key) {
    tex_diag_counts[event].fetch_add(1, std::memory_order_relaxed);
    static std::mutex diag_mutex;
    static std::unordered_map<u64, u32> printed;
    static u32 lines = 0;
    std::scoped_lock lk{diag_mutex};
    if (lines >= 3000) {
        return false;
    }
    u32& count = printed[key * NumTexDiagEvents + event];
    if (count >= 3) {
        return false;
    }
    ++count;
    ++lines;
    return true;
}

void TexDiagSummary() {
    static std::atomic<s64> last{0};
    const s64 now = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    s64 previous = last.load(std::memory_order_relaxed);
    if (now - previous < 5 || !last.compare_exchange_strong(previous, now)) {
        return;
    }
    std::string text;
    for (u32 event = 0; event < NumTexDiagEvents; ++event) {
        if (const u64 count = tex_diag_counts[event].load(std::memory_order_relaxed)) {
            text += fmt::format("; {} {}", TexDiagNames[event], count);
        }
    }
    if (!text.empty()) {
        std::printf("Texture diag: totals%s\n", text.c_str());
    }
}

std::string TexDiagImage(const Image& image) {
    const auto& info = image.info;
    const auto flag = [&](ImageFlagBits bit, const char* name) {
        return True(image.flags & bit) ? name : "";
    };
    return fmt::format("{:#x}+{:#x} {} {}x{}x{} mips {} layers {} tile {} pitch {} flags "
                       "{}{}{}{} usage {}{}{}{} uid {}",
                       info.guest_address, info.guest_size, vk::to_string(info.pixel_format),
                       info.size.width, info.size.height, info.size.depth, info.resources.levels,
                       info.resources.layers, static_cast<u32>(info.tile_mode), info.pitch,
                       flag(ImageFlagBits::CpuDirty, "c"), flag(ImageFlagBits::MaybeCpuDirty, "m"),
                       flag(ImageFlagBits::GpuDirty, "g"), flag(ImageFlagBits::GpuModified, "G"),
                       image.usage.texture ? "t" : "", image.usage.storage ? "s" : "",
                       image.usage.render_target ? "r" : "", image.usage.depth_target ? "d" : "",
                       image.image_uid);
}

/// "all ff" or "all 00" when the first 4 KiB of guest memory at address are, else nullptr.
/// Only for memory the upload reads from the guest (not GPU-modified: never read-protected).
const char* TexDiagFillKind(VAddr address, u64 size) {
    const u64 length = Core::Memory::Instance()->ClampRangeSize(address, std::min<u64>(size, 4096));
    if (length == 0) {
        return nullptr;
    }
    const auto* bytes = reinterpret_cast<const u8*>(address);
    bool all_ff = true, all_zero = true;
    for (u64 i = 0; i < length && (all_ff || all_zero); ++i) {
        all_ff &= bytes[i] == 0xff;
        all_zero &= bytes[i] == 0;
    }
    return all_ff ? "all ff" : all_zero ? "all 00" : nullptr;
}
} // namespace

TextureCache::TextureCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                           Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                           BufferCache& buffer_cache_, PageManager& tracker_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, liverpool{liverpool_},
      buffer_cache{buffer_cache_}, tracker{tracker_}, blit_helper{instance, scheduler},
      tile_manager{instance, scheduler, runtime, buffer_cache.GetStreamBuffer()},
      readback_linear_images{EmulatorSettings.IsReadbackLinearImagesEnabled()} {

    u32 max_samplers = instance.GetMaxSamplerAllocationCount();
    trigger_gc_samplers = max_samplers * 3 / 4;
    pressure_gc_samplers = max_samplers * 7 / 8;
    critical_gc_samplers = max_samplers * 15 / 16;

    // Set up garbage collection parameters.
    if (!instance.CanReportMemoryUsage()) {
        trigger_gc_memory = 0;
        pressure_gc_memory = DEFAULT_PRESSURE_GC_MEMORY;
        critical_gc_memory = DEFAULT_CRITICAL_GC_MEMORY;
        return;
    }

    const s64 device_local_memory = static_cast<s64>(instance.GetTotalMemoryBudget());
    const s64 min_spacing_expected = device_local_memory - 1_GB;
    const s64 min_spacing_critical = device_local_memory - 512_MB;
    const s64 mem_threshold = std::min<s64>(device_local_memory, TARGET_GC_THRESHOLD);
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
    pressure_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_expected, min_spacing_expected),
                      DEFAULT_PRESSURE_GC_MEMORY));
    critical_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_critical, min_spacing_critical),
                      DEFAULT_CRITICAL_GC_MEMORY));
    trigger_gc_memory = static_cast<u64>((device_local_memory - mem_threshold) / 2);
}

TextureCache::~TextureCache() = default;

void TextureCache::ProcessDownloadImages() {
    std::unique_lock lk{download_images_mutex};
    DownloadImagesSync();
    download_images.clear();
    // bbport: tiny render targets the CPU reads, downloaded without waiting (they reach guest
    // memory when the GPU gets there, a frame late at most).
    for (const ImageId image_id : small_download_images) {
        DownloadImageMemory(image_id, false);
    }
    small_download_images.clear();
}

/// bbport: BB_READBACK_SMALL (default 1; 0 off): render targets of at most 8x8 pixels go back
/// to guest memory after the GPU writes them. Uncharted 2 reduces the scene depth to a 1x1
/// focus distance the CPU reads for the turret's depth of field; without it the focus stays
/// stale and the whole picture blurs.
static bool ReadbackSmallTargets() {
    static const bool value = [] {
        const char* env = std::getenv("BB_READBACK_SMALL");
        return !(env && env[0] == '0');
    }();
    return value;
}

/// The guest bytes of an image's download and its copy into `buffer_offset` of a staging buffer.
static u32 ImageDownloadSize(const Image& image) {
    return image.info.pitch * image.info.size.height * image.info.size.depth *
           image.info.resources.layers * (image.info.num_bits / 8);
}

static vk::BufferImageCopy ImageDownloadCopy(const Image& image, u64 buffer_offset) {
    return {
        .bufferOffset = buffer_offset,
        .bufferRowLength = image.info.pitch,
        .bufferImageHeight = image.info.size.height,
        .imageSubresource =
            {
                .aspectMask = image.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                        : vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.info.size.width, image.info.size.height, image.info.size.depth},
    };
}

/// bbport: the synchronous downloads (BB_READBACK_LINEAR) of a fence: every copy is recorded
/// first and the GPU is waited for once. Each image waited for the whole GPU queue on its own
/// before, a full stall per image at every end-of-pipe fence. BB_READBACK_BATCH=0: one wait
/// per image again. Under download_images_mutex.
void TextureCache::DownloadImagesSync() {
    static const bool batch = [] {
        const char* env = std::getenv("BB_READBACK_BATCH");
        return !(env && env[0] == '0');
    }();
    if (!batch) {
        for (const ImageId image_id : download_images) {
            DownloadImageMemory(image_id, true);
        }
        return;
    }
    struct Pending {
        VAddr address;
        Vulkan::StagingBufferRef download;
        u32 size;
    };
    boost::container::small_vector<Pending, 8> pending;
    for (const ImageId image_id : download_images) {
        Image& image = slot_images[image_id];
        if (False(image.flags & ImageFlagBits::GpuModified)) {
            continue;
        }
        const u32 download_size = ImageDownloadSize(image);
        ASSERT(download_size <= image.info.guest_size);
        const auto download =
            runtime.GetStagingPool().Request(download_size, MemoryType::HostCached, 16);
        const auto copy = ImageDownloadCopy(image, download.offset);
        runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
        pending.push_back({image.info.guest_address, download, download_size});
    }
    if (pending.empty()) {
        return;
    }
    scheduler.Finish();
    for (const auto& [address, download, size] : pending) {
        download.Invalidate();
        Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(address), download.mapped,
                                                  size);
    }
}

void TextureCache::DownloadImageMemory(ImageId image_id, bool sync) {
    Image& image = slot_images[image_id];
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        return;
    }
    const u32 download_size = ImageDownloadSize(image);
    ASSERT(download_size <= image.info.guest_size);
    const auto download =
        runtime.GetStagingPool().Request(download_size, MemoryType::HostCached, 16, !sync);
    const vk::BufferImageCopy image_download = ImageDownloadCopy(image, download.offset);
    runtime.DownloadImage(&image, download.buffer, std::span{&image_download, 1});
    if (sync) {
        scheduler.Finish();
        download.Invalidate();
        Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(image.info.guest_address),
                                                  download.mapped, download_size);
    } else {
        scheduler.DeferPriorityOperation(
            [this, device_addr = image.info.guest_address, download, download_size] {
                download.Invalidate();
                Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(device_addr),
                                                          download.mapped, download_size);
                runtime.GetStagingPool().FreeDeferred(download);
            });
    }
}

/// bbport: the guest memory a MaybeCpuDirty check compares, the same at marking and at
/// refresh (they hashed different ranges, the whole image and its first 8x8 pixels, so the
/// first check never matched). Such an image lies within the faulting page: cheap to hash
/// whole, and a CPU write past its first pixels still counts. (Linux fork 4f5a95a.)
u64 TextureCache::MaybeDirtyHash(const Image& image) {
    return XXH3_64bits(std::bit_cast<const u8*>(image.info.guest_address), image.info.guest_size);
}

void TextureCache::MarkAsMaybeDirty(ImageId image_id, Image& image) {
    if (image.hash == 0) {
        // Initialize hash
        const u8* addr = std::bit_cast<u8*>(image.info.guest_address);
        image.hash = XXH3_64bits(addr, image.info.guest_size);
        // bbport (#360): from UntrackImageHead/Tail (an image over two pages) its other page
        // was made writable at an earlier fault, and the guest may have written the image
        // there since without one: this first reference can already hold that write, and with
        // the same-range hash it would match. It reloads once instead, as the 8x8 check always
        // did at its first check (the mip hashes still keep unchanged GPU-written mips).
        const bool fully_tracked =
            image.track_addr == image.info.guest_address &&
            image.track_addr_end == image.info.guest_address + image.info.guest_size;
        if (GpuHashFix() && !fully_tracked) {
            image.flags |= ImageFlagBits::CpuDirty;
        }
    }
    image.flags |= ImageFlagBits::MaybeCpuDirty;
    UntrackImage(image_id);
}

void TextureCache::InvalidateMemory(VAddr addr, size_t size) {
    std::scoped_lock lock{mutex};
    const auto pages_start = PageManager::GetPageAddr(addr);
    const auto pages_end = PageManager::GetNextPageAddr(addr + size - 1);
    ForEachImageInRegion(pages_start, pages_end - pages_start, [&](ImageId image_id, Image& image) {
        const auto image_begin = image.info.guest_address;
        const auto image_end = image.info.guest_address + image.info.guest_size;
        if (image.Overlaps(addr, size)) {
            // Modified region overlaps image, so the image was definitely accessed by this fault.
            // Untrack the image, so that the range is unprotected and the guest can write freely.
            image.flags |= ImageFlagBits::CpuDirty;
            UntrackImage(image_id);
        } else if (pages_end < image_end) {
            // This page access may or may not modify the image.
            // We should not mark it as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            // Remove tracking from this page only.
            UntrackImageHead(image_id);
        } else if (image_begin < pages_start) {
            // This page access does not modify the image but the page should be untracked.
            // We should not mark this image as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            UntrackImageTail(image_id);
        } else {
            // Image begins and ends on this page so it can not receive any more invalidations.
            // We will check it's hash later to see if it really was modified.
            MarkAsMaybeDirty(image_id, image);
        }
    });
}

void TextureCache::NoteBackingWrite(VAddr address, u64 size) {
    std::scoped_lock lock{backing_writes_mutex};
    backing_writes.emplace_back(address, size);
    has_backing_writes.store(true, std::memory_order_release);
}

void TextureCache::ApplyBackingWrites() {
    if (!has_backing_writes.load(std::memory_order_acquire)) {
        return;
    }
    std::vector<std::pair<VAddr, u64>> writes;
    {
        std::scoped_lock lock{backing_writes_mutex};
        writes.swap(backing_writes);
        has_backing_writes.store(false, std::memory_order_release);
    }
    // gow3: God of War III streams textures with GPU copies through large storage images over the
    // texture pool; their readbacks put the new texels in memory without a fault. Textures
    // there kept the old contents (yellow ground #138, missing textures #194) until a restart.
    for (const auto& [address, size] : writes) {
        ForEachImageInRegion(address, size, [&](ImageId image_id, Image& image) {
            if (True(image.flags & ImageFlagBits::GpuModified)) {
                return;
            }
            if (TexDiagFor(image.info.guest_address, image.info.guest_size) &&
                TexDiagNote(BackingDirty, image.image_uid)) {
                std::printf("Texture diag: read back data at %#llx+%#llx reloads %s\n",
                            static_cast<unsigned long long>(address),
                            static_cast<unsigned long long>(size), TexDiagImage(image).c_str());
            }
            image.flags |= ImageFlagBits::CpuDirty;
            UntrackImage(image_id);
        });
    }
}

void TextureCache::InvalidateMemoryFromGPU(VAddr address, size_t max_size, bool whole_range) {
    std::scoped_lock lock{mutex};
    ForEachImageInRegion(address, max_size, [&](ImageId image_id, Image& image) {
        // Only consider images that match base address, or with whole_range every image
        // overlapping the written range.
        // TODO: Maybe also consider subresources
        if (whole_range ? !image.Overlaps(address, max_size) : image.info.guest_address != address) {
            if (!whole_range && image.Overlaps(address, max_size) &&
                TexDiagFor(image.info.guest_address, image.info.guest_size) &&
                TexDiagNote(StorageMiss, image.image_uid)) {
                std::printf("Texture diag: storage buffer %#llx+%#llx written, not invalidating "
                            "%s\n",
                            static_cast<unsigned long long>(address),
                            static_cast<unsigned long long>(max_size), TexDiagImage(image).c_str());
            }
            return;
        }
        // Ensure image is reuploaded when accessed again.
        image.flags |= ImageFlagBits::GpuDirty;
    });
}

void TextureCache::InvalidateMemoryFromGpuRange(VAddr address, size_t size,
                                                bool covered_gpu_images) {
    std::scoped_lock lock{mutex};
    ForEachImageInRegion(address, size, [&](ImageId image_id, Image& image) {
        if (!image.Overlaps(address, size)) {
            return;
        }
        const bool diag = TexDiagFor(image.info.guest_address, image.info.guest_size);
        if (True(image.flags & ImageFlagBits::GpuModified)) {
            const bool covered = address <= image.info.guest_address &&
                                 image.info.guest_address + image.info.guest_size <= address + size;
            if (covered_gpu_images ? !covered : image.info.guest_address != address) {
                if (diag && TexDiagNote(RangeSkip, image.image_uid)) {
                    std::printf("Texture diag: GPU buffer write %#llx+%#llx over part of %s, "
                                "left as it is\n",
                                static_cast<unsigned long long>(address),
                                static_cast<unsigned long long>(size), TexDiagImage(image).c_str());
                }
                return;
            }
        }
        if (diag && covered_gpu_images && TexDiagNote(FillGpu, image.image_uid)) {
            std::printf("Texture diag: DMA fill %#llx+%#llx on the GPU reloads %s\n",
                        static_cast<unsigned long long>(address),
                        static_cast<unsigned long long>(size), TexDiagImage(image).c_str());
        }
        image.flags |= ImageFlagBits::GpuDirty;
    });
}

void TextureCache::UnmapMemory(VAddr cpu_addr, size_t size) {
    std::scoped_lock lk{mutex};

    ImageIds deleted_images;
    ForEachImageInRegion(cpu_addr, size, [&](ImageId id, Image&) { deleted_images.push_back(id); });
    for (const ImageId id : deleted_images) {
        // TODO: Download image data back to host.
        FreeImage(id);
    }
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                          ImageId cache_image_id) {
    auto& cache_image = slot_images[cache_image_id];

    if (!cache_image.info.props.is_depth && !requested_info.props.is_depth) {
        return {};
    }

    const bool stencil_match =
        requested_info.props.has_stencil == cache_image.info.props.has_stencil;
    const bool bpp_match = requested_info.num_bits == cache_image.info.num_bits;

    // If an image in the cache has less slices we need to expand it
    bool recreate = cache_image.info.resources < requested_info.resources;

    switch (binding) {
    case BindingType::Texture:
        // The guest requires a depth sampled texture, but cache can offer only Rxf. Need to
        // recreate the image.
        recreate |= requested_info.props.is_depth && !cache_image.info.props.is_depth;
        break;
    case BindingType::Storage:
        // If the guest is going to use previously created depth as storage, the image needs to be
        // recreated. (TODO: Probably a case with linear rgba8 aliasing is legit)
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::RenderTarget:
        // Render target can have only Rxf format. If the cache contains only Dx[S8] we need to
        // re-create the image.
        ASSERT(!requested_info.props.is_depth);
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::DepthTarget:
        // The guest has requested previously allocated texture to be bound as a depth target.
        // In this case we need to convert Rx float to a Dx[S8] as requested
        recreate |= !cache_image.info.props.is_depth;

        // The guest is trying to bind a depth target and cache has it. Need to be sure that aspects
        // and bpp match
        recreate |= cache_image.info.props.is_depth && !(stencil_match && bpp_match);
        break;
    default:
        break;
    }

    if (recreate) {
        auto new_info = requested_info;
        new_info.resources = std::max(requested_info.resources, cache_image.info.resources);
        new_info.UpdateSize();
        const auto new_image_id = slot_images.insert(instance, runtime, slot_image_views, new_info);
        RegisterImage(new_image_id);

        // Inherit image usage
        auto& new_image = slot_images[new_image_id];
        new_image.usage = cache_image.usage;
        new_image.flags &= ~ImageFlagBits::Dirty;
        // When creating a depth buffer through overlap resolution don't clear it on first use.
        new_image.info.meta_info.htile_clear_mask = 0;
        runtime.CopyColorAndDepth(&cache_image, &new_image);

        // Free the cache image.
        FreeImage(cache_image_id);
        return new_image_id;
    }

    // Will be handled by view
    return cache_image_id;
}

std::tuple<ImageId, int, int> TextureCache::ResolveOverlap(const ImageInfo& image_info,
                                                           BindingType binding,
                                                           ImageId cache_image_id,
                                                           ImageId merged_image_id) {
    auto& cache_image = slot_images[cache_image_id];
    const bool safe_to_delete =
        scheduler.CurrentTick() - cache_image.tick_accessed_last > NumFramesBeforeRemoval;

    // Equal address
    if (image_info.guest_address == cache_image.info.guest_address) {
        const u32 lhs_block_size = image_info.num_bits * image_info.num_samples;
        const u32 rhs_block_size = cache_image.info.num_bits * cache_image.info.num_samples;
        if (image_info.BlockDim() != cache_image.info.BlockDim() ||
            lhs_block_size != rhs_block_size) {
            // Very likely this kind of overlap is caused by allocation from a pool.
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        if (const auto depth_image_id = ResolveDepthOverlap(image_info, binding, cache_image_id)) {
            return {depth_image_id, -1, -1};
        }

        // Compressed view of uncompressed image with same block size.
        if (image_info.props.is_block && !cache_image.info.props.is_block) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        if (image_info.guest_size == cache_image.info.guest_size &&
            (image_info.type == AmdGpu::ImageType::Color3D ||
             cache_image.info.type == AmdGpu::ImageType::Color3D)) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        const bool pow2_padding_only =
            image_info.props.is_pow2 != cache_image.info.props.is_pow2 &&
            image_info.tile_mode == cache_image.info.tile_mode &&
            image_info.size == cache_image.info.size &&
            image_info.pitch == cache_image.info.pitch && image_info.resources.levels == 1 &&
            cache_image.info.resources.levels == 1 && image_info.resources.layers == 1 &&
            cache_image.info.resources.layers == 1;

        // Size and resources are less than or equal, use image view.
        if (image_info.pixel_format != cache_image.info.pixel_format ||
            image_info.guest_size <= cache_image.info.guest_size || pow2_padding_only) {
            auto result_id = merged_image_id ? merged_image_id : cache_image_id;
            const auto& result_image = slot_images[result_id];
            const bool is_compatible =
                IsVulkanFormatCompatible(result_image.info.pixel_format, image_info.pixel_format);
            return {is_compatible ? result_id : ImageId{}, -1, -1};
        }

        // Size and resources are greater, expand the image.
        if (image_info.type == cache_image.info.type &&
            image_info.resources > cache_image.info.resources) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        // Size is greater but resources are not, because the tiling mode is different.
        // Likely the address is reused for a image with a different tiling mode.
        if (image_info.tile_mode != cache_image.info.tile_mode) {
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        // Enhanced debug logging for unreachable case
        // Calculate expected size based on format and dimensions
        u64 expected_size =
            (static_cast<u64>(image_info.size.width) * static_cast<u64>(image_info.size.height) *
             static_cast<u64>(image_info.size.depth) * static_cast<u64>(image_info.num_bits) / 8);
        LOG_ERROR(Render_Vulkan,
                  "Unresolvable image overlap with equal memory address:\n"
                  "=== OLD IMAGE (cached) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "  Last accessed:  tick {}\n"
                  "  Safe to delete: {}\n"
                  "  isPow2:         {}\n"
                  "  Alt tile:       {}\n"
                  "\n"
                  "=== NEW IMAGE (requested) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "  isPow2:         {}\n"
                  "  Alt tile:       {}\n"
                  "\n"
                  "=== COMPARISON ===\n"
                  "  Same format:           {}\n"
                  "  Same type:             {}\n"
                  "  Same tile mode:        {}\n"
                  "  Same block size:       {}\n"
                  "  Same BlockDim:         {}\n"
                  "  Same pitch:            {}\n"
                  "  Same pow2:             {}\n"
                  "  Same alt tile:         {}\n"
                  "  Old resources <= new:  {} (old: {}, new: {})\n"
                  "  Old size <= new size:  {}\n"
                  "  Expected size (calc):  {} bytes\n"
                  "  Size ratio (new/expected): {:.2f}x\n"
                  "  Size ratio (new/old):  {:.2f}x\n"
                  "  Old vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  New vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  Merged image ID:       {}\n"
                  "  Binding type:          {}\n"
                  "  Current tick:          {}\n"
                  "  Age (ticks since last access): {}",

                  // Old image details
                  cache_image.info.guest_address, cache_image.info.guest_size,
                  vk::to_string(cache_image.info.pixel_format),
                  static_cast<int>(cache_image.info.type), cache_image.info.size.width,
                  cache_image.info.size.height, cache_image.info.size.depth, cache_image.info.pitch,
                  cache_image.info.resources.levels, cache_image.info.resources.layers,
                  cache_image.info.num_samples, static_cast<u32>(cache_image.info.tile_mode),
                  cache_image.info.num_bits, +cache_image.info.props.is_block,
                  cache_image.info.guest_size, cache_image.tick_accessed_last, safe_to_delete,
                  bool(cache_image.info.props.is_pow2), cache_image.info.alt_tile,

                  // New image details
                  image_info.guest_address, image_info.guest_size,
                  vk::to_string(image_info.pixel_format), static_cast<int>(image_info.type),
                  image_info.size.width, image_info.size.height, image_info.size.depth,
                  image_info.pitch, image_info.resources.levels, image_info.resources.layers,
                  image_info.num_samples, static_cast<u32>(image_info.tile_mode),
                  image_info.num_bits, image_info.props.is_block, image_info.guest_size,
                  bool(image_info.props.is_pow2), image_info.alt_tile,

                  // Comparison
                  (image_info.pixel_format == cache_image.info.pixel_format),
                  (image_info.type == cache_image.info.type),
                  (image_info.tile_mode == cache_image.info.tile_mode),
                  (image_info.num_bits == cache_image.info.num_bits),
                  (image_info.BlockDim() == cache_image.info.BlockDim()),
                  (image_info.pitch == cache_image.info.pitch),
                  (image_info.props.is_pow2 == cache_image.info.props.is_pow2),
                  (image_info.alt_tile == cache_image.info.alt_tile),
                  (cache_image.info.resources <= image_info.resources),
                  cache_image.info.resources.levels, image_info.resources.levels,
                  (cache_image.info.guest_size <= image_info.guest_size), expected_size,

                  // Size ratios
                  static_cast<double>(image_info.guest_size) / expected_size,
                  static_cast<double>(image_info.guest_size) / cache_image.info.guest_size,

                  // Difference between actual and expected sizes with percentages
                  static_cast<s64>(cache_image.info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(cache_image.info.guest_size) / expected_size - 1.0) * 100.0,

                  static_cast<s64>(image_info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(image_info.guest_size) / expected_size - 1.0) * 100.0,

                  merged_image_id.index, static_cast<int>(binding), scheduler.CurrentTick(),
                  scheduler.CurrentTick() - cache_image.tick_accessed_last);

        UNREACHABLE_MSG("Encountered unresolvable image overlap with equal memory address.");
    }

    // Right overlap, the image requested is a possible subresource of the image from cache.
    if (image_info.guest_address > cache_image.info.guest_address) {
        if (auto mip = image_info.MipOf(cache_image.info); mip >= 0) {
            if (auto slice = image_info.SliceOf(cache_image.info, mip); slice >= 0) {
                return {cache_image_id, mip, slice};
            }
        }

        // Image isn't a subresource but a chance overlap.
        if (safe_to_delete) {
            FreeImage(cache_image_id);
        }

        return {{}, -1, -1};
    } else {
        // Left overlap, the image from cache is a possible subresource of the image requested
        if (auto mip = cache_image.info.MipOf(image_info); mip >= 0) {
            if (auto slice = cache_image.info.SliceOf(image_info, mip); slice >= 0) {
                // We have a larger image created and a separate one, representing a subres of it
                // bound as render target. In this case we need to rebind render target.
                if (cache_image.binding.is_target) {
                    cache_image.binding.needs_rebind = 1u;
                    if (merged_image_id) {
                        GetImage(merged_image_id).binding.is_target = 1u;
                    }

                    FreeImage(cache_image_id);
                    return {merged_image_id, -1, -1};
                }

                // We need to have a larger, already allocated image to copy this one into
                if (merged_image_id) {
                    auto& merged_image = slot_images[merged_image_id];
                    runtime.CopyMip(&cache_image, &merged_image, mip, slice);
                    FreeImage(cache_image_id);
                }
            }
        }
    }

    return {merged_image_id, -1, -1};
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId image_id) {
    const auto new_image_id = slot_images.insert(instance, runtime, slot_image_views, info);
    RegisterImage(new_image_id);

    auto& src_image = slot_images[image_id];
    auto& new_image = slot_images[new_image_id];

    RefreshImage(new_image);
    runtime.CopyImage(&src_image, &new_image);

    if (src_image.binding.is_bound || src_image.binding.is_target) {
        src_image.binding.needs_rebind = 1u;
    }

    FreeImage(image_id);
    TrackImage(new_image_id);
    return new_image_id;
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_fmt) {
    const auto& info = desc.info;
    ASSERT(info.guest_address != 0);

    std::scoped_lock lock{mutex};

    u64 key_hash = info.guest_address ^ info.guest_size << 17 ^ u64(info.pixel_format) << 40 ^
                   u64(info.size.width) << 24 ^ info.size.height ^ u64(desc.type) << 58 ^
                   u64(exact_fmt) << 63;
    key_hash = (key_hash ^ key_hash >> 33) * 0xFF51AFD7ED558CCDull;
    key_hash = (key_hash ^ key_hash >> 33) * 0xC4CEB9FE1A85EC53ull;
    key_hash ^= key_hash >> 33;
    auto& cached = find_image_cache[key_hash % find_image_cache.size()];
    // An exact match stays valid while that image is registered with the same description;
    // resolved overlaps (views into other images) only until any image registration changes.
    const auto still_exact = [&] {
        if (cached.view_mip >= 0 || cached.view_slice >= 0 || !cached.image_id) {
            return false;
        }
        const Image& image = slot_images[cached.image_id];
        return True(image.flags & ImageFlagBits::Registered) &&
               image.info.guest_address == info.guest_address &&
               image.info.guest_size == info.guest_size && image.info.size == info.size &&
               image.info.pixel_format == info.pixel_format;
    };
    if ((cached.generation == registry_generation.load(std::memory_order_relaxed) || still_exact()) &&
        cached.address == info.guest_address &&
        cached.size == info.guest_size && cached.extent == info.size &&
        cached.format == info.pixel_format && cached.type == info.type &&
        cached.exact_fmt == exact_fmt && cached.binding == desc.type &&
        cached.levels == info.resources.levels && cached.layers == info.resources.layers &&
        !BbToggle::Disabled(BbToggle::FindImageCache)) {
        Image& image = slot_images[cached.image_id];
        image.tick_accessed_last = scheduler.CurrentTick();
        TouchImage(image);
        if (cached.view_mip > 0) {
            desc.view_info.range.base.level = cached.view_mip;
        }
        if (cached.view_slice > 0) {
            desc.view_info.range.base.layer = cached.view_slice;
        }
        return cached.image_id;
    }

    ImageIds image_ids;
    ForEachImageInRegion(info.guest_address, info.guest_size,
                         [&](ImageId image_id, Image& image) { image_ids.push_back(image_id); });
    // bbport diagnosis (#360): GPU-modified images here, described before ResolveOverlap may
    // free them, for the report when a new image is created beside them.
    boost::container::small_vector<std::string, 2> diag_gpu_images;
    if (desc.type != BindingType::RenderTarget && desc.type != BindingType::DepthTarget &&
        TexDiagFor(info.guest_address, info.guest_size)) {
        for (const ImageId cache_id : image_ids) {
            const Image& cache_image = slot_images[cache_id];
            if (True(cache_image.flags & ImageFlagBits::GpuModified)) {
                diag_gpu_images.push_back(TexDiagImage(cache_image));
            }
        }
    }

    ImageId image_id{};

    // Check for a perfect match first
    for (const auto& cache_id : image_ids) {
        auto& cache_image = slot_images[cache_id];
        if (cache_image.info.guest_address != info.guest_address) {
            continue;
        }
        if (cache_image.info.guest_size != info.guest_size) {
            continue;
        }
        if (cache_image.info.size != info.size) {
            continue;
        }
        if (!IsVulkanFormatCompatible(cache_image.info.pixel_format, info.pixel_format) ||
            (cache_image.info.type != info.type && info.size != Extent3D{1, 1, 1})) {
            continue;
        }
        if (exact_fmt && info.pixel_format != cache_image.info.pixel_format) {
            continue;
        }
        image_id = cache_id;
    }

    // Try to resolve overlaps (if any)
    int view_mip{-1};
    int view_slice{-1};
    if (!image_id) {
        for (const auto& cache_id : image_ids) {
            view_mip = -1;
            view_slice = -1;

            const auto& merged_info = image_id ? slot_images[image_id].info : info;
            auto [overlap_image_id, overlap_view_mip, overlap_view_slice] =
                ResolveOverlap(merged_info, desc.type, cache_id, image_id);
            if (overlap_image_id) {
                image_id = overlap_image_id;
                view_mip = overlap_view_mip;
                view_slice = overlap_view_slice;
            }
        }
    }

    if (image_id) {
        Image& image_resolved = slot_images[image_id];
        if (exact_fmt && info.pixel_format != image_resolved.info.pixel_format) {
            // Cannot reuse this image as we need the exact requested format.
            image_id = {};
        } else if (image_resolved.info.resources < info.resources) {
            // The image was clearly picked up wrong.
            FreeImage(image_id);
            image_id = {};
            LOG_WARNING(Render_Vulkan, "Image overlap resolve failed");
        }
    }
    // Create and register a new image
    if (!image_id) {
        image_id = slot_images.insert(instance, runtime, slot_image_views, info);
        RegisterImage(image_id);
        if (!diag_gpu_images.empty() && TexDiagNote(AliasMiss, info.guest_address)) {
            std::printf("Texture diag: new %s image %#llx+%#llx %s %ux%ux%u mips %u layers %u "
                        "tile %u pitch %u uploaded from memory, beside GPU-written %s%s\n",
                        desc.type == BindingType::Storage ? "storage" : "texture",
                        static_cast<unsigned long long>(info.guest_address),
                        static_cast<unsigned long long>(info.guest_size),
                        vk::to_string(info.pixel_format).c_str(), info.size.width,
                        info.size.height, info.size.depth, info.resources.levels,
                        info.resources.layers, static_cast<u32>(info.tile_mode), info.pitch,
                        diag_gpu_images[0].c_str(),
                        diag_gpu_images.size() > 1 ? " (and more)" : "");
        }
    }

    Image& image = slot_images[image_id];
    image.tick_accessed_last = scheduler.CurrentTick();
    TouchImage(image);

    // If the image requested is a subresource of the image from cache record its location.
    if (view_mip > 0) {
        desc.view_info.range.base.level = view_mip;
    }
    if (view_slice > 0) {
        desc.view_info.range.base.layer = view_slice;
    }

    cached = FindImageCacheEntry{
        .address = info.guest_address,
        .size = info.guest_size,
        .extent = info.size,
        .format = info.pixel_format,
        .type = info.type,
        .exact_fmt = exact_fmt,
        .binding = desc.type,
        .levels = info.resources.levels,
        .layers = info.resources.layers,
        .generation = registry_generation.load(std::memory_order_relaxed),
        .image_id = image_id,
        .view_mip = view_mip,
        .view_slice = view_slice,
    };
    return image_id;
}

ImageId TextureCache::FindImageFromRange(VAddr address, size_t size, bool ensure_valid) {
    ImageIds image_ids;
    ForEachImageInRegion(address, size, [&](ImageId image_id, Image& image) {
        if (image.info.guest_address != address) {
            return;
        }
        if (ensure_valid && !image.SafeToDownload()) {
            return;
        }
        image_ids.push_back(image_id);
    });
    if (image_ids.size() == 1) {
        // Sometimes image size might not exactly match with requested buffer size
        // If we only found 1 candidate image use it without too many questions.
        return image_ids.back();
    }
    if (!image_ids.empty()) {
        for (s32 i = 0; i < image_ids.size(); ++i) {
            Image& image = slot_images[image_ids[i]];
            if (image.info.guest_size == size) {
                return image_ids[i];
            }
        }
        LOG_WARNING(Render_Vulkan,
                    "Failed to find exact image match for copy addr={:#x}, size={:#x}", address,
                    size);
    }
    return {};
}

ImageView& TextureCache::FindTexture(ImageId image_id, const ImageDesc& desc, ViewMemo* memo,
                                     bool refresh) {
    Image& image = slot_images[image_id];
    if (desc.type == BindingType::Storage) {
        image.flags |= ImageFlagBits::GpuModified;
        if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8) &&
            image.info.guest_address != 0) {
            std::unique_lock lk{download_images_mutex};
            download_images.emplace(image_id);
        }
    }
    if (refresh) {
        UpdateImage(image_id);
    }
    if (memo && !BbToggle::Disabled(BbToggle::TextureViewMemo)) {
        if (memo->image_id == image_id && memo->backing == image.backing && memo->view_id) {
            return slot_image_views[memo->view_id];
        }
        ImageView& view = image.FindView(desc.view_info);
        const auto& ids = image.backing->image_view_ids;
        const u32 last = image.backing->last_view;
        memo->image_id = image_id;
        memo->backing = image.backing;
        memo->view_id = last < ids.size() && &slot_image_views[ids[last]] == &view ? ids[last]
                                                                                    : ids.back();
        return view;
    }
    return image.FindView(desc.view_info);
}

ImageView& TextureCache::FindRenderTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    // bbport: tiny targets go back without waiting even with BB_READBACK_LINEAR (the advice for
    // 0.3.6, which players keep): a synchronous download waits for the whole GPU queue.
    if (ReadbackSmallTargets() && image.info.size.width <= 8 && image.info.size.height <= 8 &&
        image.info.guest_address != 0) {
        std::unique_lock lk{download_images_mutex};
        small_download_images.emplace(image_id);
    } else if (readback_linear_images &&
               (!image.info.props.is_tiled || image.info.size.width <= 8)) {
        std::unique_lock lk{download_images_mutex};
        download_images.emplace(image_id);
    }
    image.usage.render_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this color buffer
    if (desc.info.meta_info.cmask_addr) {
        surface_metas.emplace(desc.info.meta_info.cmask_addr,
                              MetaDataInfo{.type = MetaType::CMask});
        image.info.meta_info.cmask_addr = desc.info.meta_info.cmask_addr;
    }

    if (desc.info.meta_info.fmask_addr) {
        surface_metas.emplace(desc.info.meta_info.fmask_addr,
                              MetaDataInfo{.type = MetaType::FMask});
        image.info.meta_info.fmask_addr = desc.info.meta_info.fmask_addr;
    }

    return image.FindView(desc.view_info, false);
}

ImageView& TextureCache::FindDepthTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    image.usage.depth_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this depth buffer
    if (desc.info.meta_info.htile_addr) {
        surface_metas.emplace(desc.info.meta_info.htile_addr,
                              MetaDataInfo{.type = MetaType::HTile,
                                           .clear_mask = image.info.meta_info.htile_clear_mask});
        image.info.meta_info.htile_addr = desc.info.meta_info.htile_addr;
    }

    // If there is a stencil attachment, link depth and stencil.
    if (desc.info.stencil_addr != 0) {
        ImageId stencil_id{};
        ForEachImageInRegion(desc.info.stencil_addr, desc.info.stencil_size,
                             [&](ImageId image_id, Image& image) {
                                 if (image.info.guest_address == desc.info.stencil_addr) {
                                     stencil_id = image_id;
                                 }
                             });
        if (!stencil_id) {
            ImageInfo info{};
            info.guest_address = desc.info.stencil_addr;
            info.guest_size = desc.info.stencil_size;
            info.size = desc.info.size;
            stencil_id = slot_images.insert(instance, runtime, slot_image_views, info);
            RegisterImage(stencil_id);
        }
        Image& stencil_image = slot_images[stencil_id];
        TouchImage(stencil_image);
        stencil_image.AssociateDepth(image_id, image.image_uid);
    }

    return image.FindView(desc.view_info, false);
}

void TextureCache::RefreshImage(Image& image) {
    if (False(image.flags & ImageFlagBits::Dirty) || image.info.num_samples > 1) {
        return;
    }
    BbStats::Timer timer{BbStats::t_refresh};

    RENDERER_TRACE;
    TRACE_HINT(fmt::format("{:x}:{:x}", image.info.guest_address, image.info.guest_size));

    if (True(image.flags & ImageFlagBits::MaybeCpuDirty) &&
        False(image.flags & ImageFlagBits::CpuDirty)) {
        u64 hash;
        if (GpuHashFix()) {
            hash = MaybeDirtyHash(image);
        } else {
            // The image size should be less than page size to be considered MaybeCpuDirty
            // So this calculation should be very uncommon and reasonably fast
            // For now we'll just check up to 64 first pixels
            const auto addr = std::bit_cast<u8*>(image.info.guest_address);
            const u32 w = std::min(image.info.size.width, u32(8));
            const u32 h = std::min(image.info.size.height, u32(8));

            const u32 s_w = image.info.props.is_block ? Common::DivCeil(w, 4u) : w;
            const u32 s_h = image.info.props.is_block ? Common::DivCeil(h, 4u) : h;
            const u32 size = s_w * s_h * (image.info.num_bits / 8);
            hash = XXH3_64bits(addr, size);
        }
        if (image.hash == hash) {
            image.flags &= ~ImageFlagBits::MaybeCpuDirty;
            // bbport (#360): a GPU buffer write marked the image too (GpuDirty): it reloads
            // from the buffer cache now, not one binding later. The same-range hash matches
            // from the first check on, where the 8x8 one never did.
            if (!GpuHashFix() || False(image.flags & ImageFlagBits::GpuDirty)) {
                return;
            }
        }
        image.hash = hash;
    }

    const u32 num_layers = image.info.resources.layers;
    const u32 num_mips = image.info.resources.levels;
    const bool is_gpu_modified = True(image.flags & ImageFlagBits::GpuModified);
    const bool is_gpu_dirty = True(image.flags & ImageFlagBits::GpuDirty);
    const bool diag = TexDiagFor(image.info.guest_address, image.info.guest_size);
    const ImageFlagBits flags_before = image.flags;
    const bool had_reference =
        diag && std::ranges::any_of(image.mip_hashes, [](u64 hash) { return hash != 0; });

    if (KeepGpuImages() && is_gpu_modified && !is_gpu_dirty) {
        if (diag && TexDiagNote(UploadOverGpu, image.image_uid)) {
            std::printf("Texture diag: kept GPU-written %s (BB_TEX_KEEP_GPU)\n",
                        TexDiagImage(image).c_str());
        }
        image.flags &= ~ImageFlagBits::Dirty;
        return;
    }

    BbStats::image_upload_bytes.fetch_add(image.info.guest_size, std::memory_order_relaxed);
    boost::container::small_vector<vk::BufferImageCopy, 14> image_copies;
    for (u32 m = 0; m < num_mips; m++) {
        const u32 width = std::max(image.info.size.width >> m, 1u);
        const u32 height = std::max(image.info.size.height >> m, 1u);
        const u32 depth =
            image.info.props.is_volume ? std::max(image.info.size.depth >> m, 1u) : 1u;
        const auto [mip_size, mip_pitch, mip_height, mip_offset] = image.info.mips_layout[m];

        // Protect GPU modified resources from accidental CPU reuploads.
        if (GpuHashFix()) {
            // bbport (Linux fork 4f5a95a + e0baa3b): uploads of images the GPU writes or may
            // write (GPU-modified ones, GPU-dirty too; small ones, targets, storage images)
            // record the guest memory they saw, not only uploads of images the GPU had already
            // written. An image the GPU writes after an upload from the buffer cache (GpuDirty)
            // or from a plain texture otherwise had no reference, and the first CPU write
            // anywhere in its pages replaced the GPU's contents with stale guest memory. A
            // storage image's first binding counts through GpuModified: FindTexture marks it
            // before the upload, BindTextures sets usage.storage only after. Streamed textures
            // are not hashed: their whole size again on the GPU thread.
            const bool track = is_gpu_modified || mip_size <= 64_KB || image.usage.storage ||
                               image.usage.render_target || image.usage.depth_target;
            if (track) {
                const u8* mip_addr = std::bit_cast<u8*>(image.info.guest_address) + mip_offset;
                const u64 mip_hash = XXH3_64bits(mip_addr, mip_size);
                if (is_gpu_modified && !is_gpu_dirty && image.mip_hashes[m] == mip_hash) {
                    continue;
                }
                image.mip_hashes[m] = mip_hash;
            }
        } else if (is_gpu_modified && !is_gpu_dirty) {
            const u8* addr = std::bit_cast<u8*>(image.info.guest_address);
            const u64 hash = XXH3_64bits(addr + mip_offset, mip_size);
            if (image.mip_hashes[m] == hash) {
                continue;
            }
            image.mip_hashes[m] = hash;
        }

        const u32 extent_width = mip_pitch ? std::min(mip_pitch, width) : width;
        const u32 extent_height = mip_height ? std::min(mip_height, height) : height;
        image_copies.push_back({
            .bufferOffset = mip_offset,
            .bufferRowLength = mip_pitch,
            .bufferImageHeight = mip_height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = m,
                .baseArrayLayer = 0,
                .layerCount = num_layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent_width, extent_height, depth},
        });
    }

    if (image_copies.empty()) {
        image.flags &= ~ImageFlagBits::Dirty;
        return;
    }

    if (diag) {
        // A new image has every dirty bit (Image's initial flags); later reasons come one by one.
        const bool fresh = (flags_before & ImageFlagBits::Dirty) == ImageFlagBits::Dirty;
        const bool from_gpu =
            buffer_cache.IsRegionGpuModified(image.info.guest_address, image.info.guest_size);
        const char* fill =
            from_gpu ? nullptr : TexDiagFillKind(image.info.guest_address, image.info.guest_size);
        const char* reason = fresh ? "new image"
                             : True(flags_before & ImageFlagBits::GpuDirty) ? "GPU buffer write"
                             : True(flags_before & ImageFlagBits::CpuDirty)
                                 ? "CPU write or read back"
                                 : "CPU write in its page";
        if (is_gpu_modified && !fresh && TexDiagNote(UploadOverGpu, image.image_uid)) {
            std::printf("Texture diag: upload over GPU-written %s: %s, %u of %u mips, reference "
                        "%s, from %s%s%s\n",
                        TexDiagImage(image).c_str(), reason, u32(image_copies.size()), num_mips,
                        had_reference ? "yes" : "no", from_gpu ? "a GPU buffer" : "guest memory",
                        fill ? ", " : "", fill ? fill : "");
        } else if (fill && (std::string_view{fill} == "all ff" || !is_gpu_modified) &&
                   TexDiagNote(UploadFill, image.image_uid)) {
            // All-0xff always; all-zero only for images the GPU did not write (new render
            // targets over cleared memory are common).
            std::printf("Texture diag: upload of %s guest memory to %s: %s\n", fill,
                        TexDiagImage(image).c_str(), reason);
        }
    }

    scheduler.EndRendering();

    const auto [in_buffer, in_offset] =
        buffer_cache.ObtainBufferForImage(image.info.guest_address, image.info.guest_size);
    const auto [buffer, offset] = tile_manager.DetileImage(in_buffer, in_offset, image.info);
    for (auto& copy : image_copies) {
        copy.bufferOffset += offset;
    }

    runtime.UploadImage(&image, buffer, image_copies);
}

vk::Sampler TextureCache::GetSampler(const AmdGpu::Sampler& sampler,
                                     AmdGpu::BorderColorBuffer border_color_base,
                                     const bool is_depth, float extra_lod_bias) {
    // Compare and plain uses of one S# need separate samplers; so do extra LOD biases.
    const u64 hash = HashCombine(HashCombine(XXH3_64bits(&sampler, sizeof(sampler)), is_depth),
                                 u64(std::bit_cast<u32>(extra_lod_bias)));

    std::scoped_lock lock{samplers_mutex};
    const auto [it, new_sampler] = samplers.try_emplace(hash, instance, sampler, border_color_base,
                                                        is_depth, extra_lod_bias);
    if (new_sampler) {
        samplers.at(hash).lru_id = sampler_lru_cache.Insert(hash, gc_tick);
    } else {
        sampler_lru_cache.Touch(it->second.lru_id, gc_tick);
    }

    return it->second.Handle();
}

void TextureCache::RegisterImage(ImageId image_id) {
    BbStats::images_registered.fetch_add(1, std::memory_order_relaxed);
    Image& image = slot_images[image_id];
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered),
               "Trying to register an already registered image");
    image.flags |= ImageFlagBits::Registered;
    ++registry_generation;
    total_used_memory += Common::AlignUp(image.info.guest_size, 1024);
    registered_image_bytes += Common::AlignUp(image.info.guest_size, 1024);
    image.lru_id = lru_cache.Insert(image_id, gc_tick);
    image.lru_touched_tick = gc_tick;
    ForEachPage(image.info.guest_address, image.info.guest_size,
                [this, image_id](u64 page) { page_table[page].push_back(image_id); });
}

void TextureCache::UnregisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(True(image.flags & ImageFlagBits::Registered),
               "Trying to unregister an already unregistered image");
    image.flags &= ~ImageFlagBits::Registered;
    ++registry_generation;
    lru_cache.Free(image.lru_id);
    total_used_memory -= Common::AlignUp(image.info.guest_size, 1024);
    registered_image_bytes -= Common::AlignUp(image.info.guest_size, 1024);
    ForEachPage(image.info.guest_address, image.info.guest_size, [this, image_id](u64 page) {
        const auto page_it = page_table.find(page);
        if (page_it == nullptr) {
            UNREACHABLE_MSG("Unregistering unregistered page=0x{:x}", page << PageShift);
            return;
        }
        auto& image_ids = *page_it;
        const auto vector_it = std::ranges::find(image_ids, image_id);
        if (vector_it == image_ids.end()) {
            ASSERT_MSG(false, "Unregistering unregistered image in page=0x{:x}", page << PageShift);
            return;
        }
        image_ids.erase(vector_it);
    });
}

void TextureCache::TrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_begin == image.track_addr && image_end == image.track_addr_end) {
        return;
    }

    if (!image.IsTracked()) {
        // Re-track the whole image
        image.track_addr = image_begin;
        image.track_addr_end = image_end;
        tracker.UpdatePageWatchers<1>(image_begin, image.info.guest_size);
    } else {
        if (image_begin < image.track_addr) {
            TrackImageHead(image_id);
        }
        if (image.track_addr_end < image_end) {
            TrackImageTail(image_id);
        }
    }
}

void TextureCache::TrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    if (image_begin == image.track_addr) {
        return;
    }
    ASSERT(image.track_addr != 0 && image_begin < image.track_addr);
    const auto size = image.track_addr - image_begin;
    image.track_addr = image_begin;
    tracker.UpdatePageWatchers<1>(image_begin, size);
}

void TextureCache::TrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_end == image.track_addr_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0 && image.track_addr_end < image_end);
    const auto addr = image.track_addr_end;
    const auto size = image_end - image.track_addr_end;
    image.track_addr_end = image_end;
    tracker.UpdatePageWatchers<1>(addr, size);
}

void TextureCache::UntrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!image.IsTracked()) {
        return;
    }
    const auto addr = image.track_addr;
    const auto size = image.track_addr_end - image.track_addr;
    image.track_addr = 0;
    image.track_addr_end = 0;
    if (size != 0) {
        tracker.UpdatePageWatchers<false>(addr, size);
    }
}

void TextureCache::UntrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_begin = image.info.guest_address;
    if (!image.IsTracked() || image_begin < image.track_addr) {
        return;
    }
    const auto addr = tracker.GetNextPageAddr(image_begin);
    const auto size = addr - image_begin;
    image.track_addr = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers<false>(image_begin, size);
}

void TextureCache::UntrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (!image.IsTracked() || image.track_addr_end < image_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0);
    const auto addr = tracker.GetPageAddr(image_end);
    const auto size = image_end - addr;
    image.track_addr_end = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers<false>(addr, size);
}

void TextureCache::GarbageCollectImages() {
    if (instance.CanReportMemoryUsage()) {
        total_used_memory = instance.GetDeviceMemoryUsage();
        // bbport: on integrated GPUs (Steam Deck) the usage covers system-memory heaps holding
        // much more than images (buffers backing guest memory), and the startup budget left
        // ~1 GB after its 8 GB system reserve: usage stayed above the critical mark, so the
        // collector evicted images used two or three frames ago on every submission and wrote
        // GPU-written ones back. Compare with the driver's current budget instead.
        // BB_GC_BUDGET_MB=N: this rule with a fixed budget on any GPU (tests on a desktop).
        static const u64 forced_budget = [] {
            const char* env = std::getenv("BB_GC_BUDGET_MB");
            return env ? std::strtoull(env, nullptr, 10) << 20 : 0;
        }();
        if (instance.IsIntegrated() || forced_budget) {
            const u64 budget = forced_budget ? forced_budget : instance.GetDeviceMemoryBudgetNow();
            if (budget != 0) {
                trigger_gc_memory = budget / 10 * 7;
                pressure_gc_memory = budget / 100 * 85;
                critical_gc_memory = budget / 100 * 95;
            }
        }
    }
    if (total_used_memory < trigger_gc_memory) {
        return;
    }
    std::scoped_lock lock{mutex};
    bool pressured = false;
    bool aggresive = false;
    u64 ticks_to_destroy = 0;
    size_t num_deletions = 0;

    const auto configure = [&](bool allow_aggressive) {
        pressured = total_used_memory >= pressure_gc_memory;
        aggresive = allow_aggressive && total_used_memory >= critical_gc_memory;
        // bbport: critical used 160 ticks, longer than pressure's 80, so the aggressive pass
        // never freed more than the first one (#256).
        ticks_to_destroy = aggresive ? 40 : pressured ? 80 : 16;
        ticks_to_destroy = std::min(ticks_to_destroy, gc_tick);
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
    };
    const auto clean_up = [&](ImageId image_id) {
        if (num_deletions == 0) {
            return true;
        }
        auto& image = slot_images[image_id];
        const bool download = image.SafeToDownload();
        const bool tiled = image.info.IsTiled();
        const bool gpu_written = True(image.flags & ImageFlagBits::GpuModified);
        const bool diag = TexDiagFor(image.info.guest_address, image.info.guest_size);
        if (gpu_written && !download && False(image.flags & ImageFlagBits::GpuDirty) &&
            !pressured && GcKeepGpuImages()) {
            // Kept images neither use up a deletion nor stay first in line (a run of them
            // stopped the collector, Linux fork 7511492/0680e7c): back of the list.
            lru_cache.Touch(image.lru_id, gc_tick);
            if (diag && TexDiagNote(GcKeep, image.image_uid)) {
                std::printf("Texture diag: GC kept %s\n", TexDiagImage(image).c_str());
            }
            return false;
        }
        --num_deletions;
        if (tiled && download) {
            // This is a workaround for now. We can't handle non-linear image downloads.
            return false;
        }
        if (download && !pressured) {
            return false;
        }
        if (download) {
            // bbport: synchronously, while the image still protects its pages. A deferred
            // write-back landed after FreeImage had unprotected them, over whatever the game
            // had meanwhile stored there (e.g. its heap after unloading an area).
            DownloadImageMemory(image_id, true);
            ++gc_downloads;
        } else if (gpu_written && diag && TexDiagNote(GcDrop, image.image_uid)) {
            std::printf("Texture diag: GC evicts %s without writing it back\n",
                        TexDiagImage(image).c_str());
        }
        ++gc_evictions;
        FreeImage(image_id);
        if (total_used_memory < critical_gc_memory) {
            if (aggresive) {
                num_deletions >>= 2;
                aggresive = false;
                return false;
            }
            if (pressured && total_used_memory < pressure_gc_memory) {
                num_deletions >>= 1;
                pressured = false;
            }
        }
        return false;
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);

    if (total_used_memory >= critical_gc_memory) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
    }
    // bbport: evictions under memory pressure, at most every 5 s (BB_FRAME_STATS or not).
    if (pressured || gc_downloads != 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now - gc_report_time >= std::chrono::seconds(5)) {
            std::printf("Texture cache: memory pressure, %llu of %llu MiB (critical %llu): "
                        "%llu images evicted, %llu written back since the last report "
                        "(registered images: %llu MiB guest size)\n",
                        (unsigned long long)(total_used_memory >> 20),
                        (unsigned long long)(pressure_gc_memory >> 20),
                        (unsigned long long)(critical_gc_memory >> 20),
                        (unsigned long long)gc_evictions, (unsigned long long)gc_downloads,
                        (unsigned long long)(registered_image_bytes >> 20));
            gc_report_time = now;
            gc_evictions = gc_downloads = 0;
        }
    }
}

void TextureCache::GarbageCollectSamplers() {
    total_used_samplers = samplers.size();
    if (total_used_samplers < trigger_gc_samplers) {
        return;
    }
    std::scoped_lock lock{samplers_mutex};
    bool pressured = false;
    bool aggresive = false;
    u64 ticks_to_destroy = 0;
    size_t num_deletions = 0;

    const auto configure = [&](bool allow_aggressive) {
        pressured = total_used_samplers >= pressure_gc_samplers;
        aggresive = allow_aggressive && total_used_samplers >= critical_gc_samplers;
        ticks_to_destroy = aggresive ? 160 : pressured ? 80 : 16;
        ticks_to_destroy = std::min(ticks_to_destroy, gc_tick);
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
    };
    const auto clean_up = [&](u64 hash) {
        if (num_deletions == 0) {
            return true;
        }
        --num_deletions;
        const size_t lru_id = samplers.at(hash).lru_id;
        samplers.erase(hash);
        sampler_lru_cache.Free(lru_id);
        return false;
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);

    if (total_used_samplers >= critical_gc_samplers) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
    }
}

void TextureCache::RunGarbageCollector() {
    SCOPE_EXIT {
        ++gc_tick;
    };
    {
        std::scoped_lock lock{mutex};
        ApplyBackingWrites();
    }
    if (TexDiagConfig().on) {
        TexDiagSummary();
    }

    GarbageCollectImages();
    GarbageCollectSamplers();
}

void TextureCache::TouchImage(const Image& image) {
    if (image.lru_touched_tick == gc_tick) {
        return;
    }
    image.lru_touched_tick = gc_tick;
    lru_cache.Touch(image.lru_id, gc_tick);
}

void TextureCache::DeleteImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(!image.IsTracked(), "Image was not untracked");
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered), "Image was not unregistered");

    // Remove any registered meta areas.
    const auto& meta_info = image.info.meta_info;
    if (meta_info.cmask_addr) {
        surface_metas.erase(meta_info.cmask_addr);
    }
    if (meta_info.fmask_addr) {
        surface_metas.erase(meta_info.fmask_addr);
    }
    if (meta_info.htile_addr) {
        surface_metas.erase(meta_info.htile_addr);
    }

    {
        std::unique_lock lk{download_images_mutex};
        if (download_images.contains(image_id)) {
            download_images.erase(image_id);
        }
        small_download_images.erase(image_id);
    }

    // Reclaim image and any image views it references.
    scheduler.DeferOperation([this, image_id] {
        Image& image = slot_images[image_id];
        for (auto& backing : image.backing_images) {
            for (const ImageViewId image_view_id : backing.image_view_ids) {
                slot_image_views.erase(image_view_id);
            }
        }
        slot_images.erase(image_id);
    });
}

} // namespace VideoCore
