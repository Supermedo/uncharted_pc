// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <magic_enum/magic_enum.hpp>
#include <vulkan/vulkan_format_traits.hpp>

namespace VideoCore {

vk::ImageViewType ConvertImageViewType(AmdGpu::ImageType type) {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
        return vk::ImageViewType::e1D;
    case AmdGpu::ImageType::Color1DArray:
        return vk::ImageViewType::e1DArray;
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
        return vk::ImageViewType::e2D;
    case AmdGpu::ImageType::Color2DArray:
        return vk::ImageViewType::e2DArray;
    case AmdGpu::ImageType::Color3D:
        return vk::ImageViewType::e3D;
    default:
        UNREACHABLE();
    }
}

bool IsViewTypeCompatible(AmdGpu::ImageType view_type, AmdGpu::ImageType image_type) {
    switch (view_type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color1DArray:
        return image_type == AmdGpu::ImageType::Color1D;
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DArray:
    case AmdGpu::ImageType::Color2DMsaa:
    case AmdGpu::ImageType::Color2DMsaaArray:
        return image_type == AmdGpu::ImageType::Color2D || image_type == AmdGpu::ImageType::Color3D;
    case AmdGpu::ImageType::Color3D:
        return image_type == AmdGpu::ImageType::Color3D;
    default:
        UNREACHABLE();
    }
}

ImageViewInfo::ImageViewInfo(const AmdGpu::Image& image, const Shader::ImageResource& desc) noexcept
    : is_storage{desc.is_written} {
    const auto dfmt = image.GetDataFmt();
    auto nfmt = image.GetNumberFmt();
    if (is_storage && nfmt == AmdGpu::NumberFormat::Srgb) {
        nfmt = AmdGpu::NumberFormat::Unorm;
    }
    format = Vulkan::LiverpoolToVK::SurfaceFormat(dfmt, nfmt);
    if (desc.is_depth) {
        format = Vulkan::LiverpoolToVK::PromoteFormatToDepth(format);
    }

    range.base.level = std::min<u32>(image.base_level, image.NumLevels() - 1);
    range.base.layer = std::min<u32>(image.base_array, image.NumLayers() - 1);
    range.extent.levels = image.NumViewLevels(desc.is_array);
    range.extent.layers = image.NumViewLayers(desc.is_array);
    type = image.GetViewType(desc.is_array);
    min_lod = static_cast<u32>(image.min_lod);

    if (!is_storage) {
        mapping = Vulkan::LiverpoolToVK::ComponentMapping(image.DstSelect());
    }
    // bbport diagnosis: BB_TSHARP_LOG=1 prints each texture format and swizzle the first time
    // it is seen (raw dst_sel: 0 zero, 1 one, 4..7 x..w).
    static const bool tsharp_log = [] {
        const char* env = std::getenv("BB_TSHARP_LOG");
        return env && env[0] == '1';
    }();
    if (tsharp_log) {
        static std::mutex seen_mutex;
        static std::set<u64> seen;
        const u64 key = u64(image.data_format) | u64(image.num_format) << 8 | u64(image.dst_sel_x) << 16 |
                        u64(image.dst_sel_y) << 20 | u64(image.dst_sel_z) << 24 | u64(image.dst_sel_w) << 28 |
                        u64(desc.is_depth) << 32 | u64(image.width) << 33 | u64(image.min_lod) << 47 |
                        u64(image.base_level) << 59;
        std::scoped_lock lk{seen_mutex};
        if (seen.insert(key).second) {
            std::printf("T#: data format %u, number format %u, dst_sel %u%u%u%u, %ux%u, depth %d, "
                        "levels %u-%u, min lod %.2f, at 0x%llx -> %s %s %s %s %s\n",
                        u32(image.data_format), u32(image.num_format), u32(image.dst_sel_x),
                        u32(image.dst_sel_y), u32(image.dst_sel_z), u32(image.dst_sel_w),
                        u32(image.width + 1), u32(image.height + 1), int(desc.is_depth),
                        u32(image.base_level), u32(image.last_level), double(image.min_lod) / 256.0,
                        static_cast<unsigned long long>(image.Address()),
                        vk::to_string(format).c_str(), vk::to_string(mapping.r).c_str(),
                        vk::to_string(mapping.g).c_str(), vk::to_string(mapping.b).c_str(),
                        vk::to_string(mapping.a).c_str());
        }
    }
}

ImageViewInfo::ImageViewInfo(const AmdGpu::ColorBuffer& col_buffer) noexcept {
    range.base.layer = col_buffer.BaseSlice();
    range.extent.layers = col_buffer.NumSlices() - range.base.layer;
    type = range.extent.layers > 1 ? AmdGpu::ImageType::Color2DArray : AmdGpu::ImageType::Color2D;
    format =
        Vulkan::LiverpoolToVK::SurfaceFormat(col_buffer.GetDataFmt(), col_buffer.GetNumberFmt());
}

ImageViewInfo::ImageViewInfo(const AmdGpu::DepthBuffer& depth_buffer, AmdGpu::DepthView view,
                             AmdGpu::DepthControl ctl) {
    format = Vulkan::LiverpoolToVK::DepthFormat(depth_buffer.z_info.format,
                                                depth_buffer.stencil_info.format);
    is_storage = ctl.depth_write_enable;
    range.base.layer = view.slice_start;
    range.extent.layers = view.NumSlices() - range.base.layer;
    type = range.extent.layers > 1 ? AmdGpu::ImageType::Color2DArray : AmdGpu::ImageType::Color2D;
}

ImageView::ImageView(const Vulkan::Instance& instance, const ImageViewInfo& info_,
                     const Image& image)
    : info{info_} {
    // bbport: a T# can ask for more mips or layers than the cached image has (a stale cache
    // entry at a reused address, e.g. at a level load). Vulkan does not check this without the
    // validation layer and the GPU then reads past the image's memory (device lost on NVIDIA).
    // Keep the view inside the image.
    const u32 image_levels = std::max(image.info.resources.levels, 1u);
    const u32 image_layers = std::max(image.info.resources.layers, 1u);
    auto& range = info.range;
    if (range.base.level >= image_levels || range.extent.levels > image_levels - range.base.level ||
        range.base.layer >= image_layers || range.extent.layers > image_layers - range.base.layer ||
        range.extent.levels == 0 || range.extent.layers == 0) {
        static std::atomic<u32> reported{0};
        if (reported.fetch_add(1, std::memory_order_relaxed) < 32) {
            std::printf("Texture cache: clamped image view of %ux%ux%u image at 0x%llx (%u mips, "
                        "%u layers): asked for mips %u+%u, layers %u+%u\n",
                        image.info.size.width, image.info.size.height, image.info.size.depth,
                        static_cast<unsigned long long>(image.info.guest_address), image_levels,
                        image_layers, range.base.level, range.extent.levels, range.base.layer,
                        range.extent.layers);
        }
        range.base.level = std::min(range.base.level, image_levels - 1);
        range.extent.levels =
            std::clamp(range.extent.levels, 1u, image_levels - range.base.level);
        range.base.layer = std::min(range.base.layer, image_layers - 1);
        range.extent.layers =
            std::clamp(range.extent.layers, 1u, image_layers - range.base.layer);
    }

    vk::ImageViewUsageCreateInfo usage_ci{.usage = image.usage_flags};
    if (!info.is_storage) {
        usage_ci.usage &= ~vk::ImageUsageFlagBits::eStorage;
    }
    vk::ImageViewMinLodCreateInfoEXT min_lod_ci{};
    if (info.min_lod != 0 && instance.IsImageViewMinLodSupported()) {
        const float last_level =
            static_cast<float>(info.range.base.level + info.range.extent.levels - 1);
        min_lod_ci.minLod = std::min(static_cast<float>(info.min_lod) / 256.f, last_level);
        usage_ci.pNext = &min_lod_ci;
    }
    // When sampling D32/D16 texture from shader, the T# specifies R32/R16 format so adjust it.
    vk::Format format = info.format;
    vk::ImageAspectFlags aspect = image.aspect_mask;
    if (image.aspect_mask & vk::ImageAspectFlagBits::eDepth &&
        Vulkan::LiverpoolToVK::IsFormatDepthCompatible(format)) {
        format = image.info.pixel_format;
        aspect = vk::ImageAspectFlagBits::eDepth;
    }
    if (image.aspect_mask & vk::ImageAspectFlagBits::eStencil &&
        Vulkan::LiverpoolToVK::IsFormatStencilCompatible(format)) {
        format = image.info.pixel_format;
        aspect = vk::ImageAspectFlagBits::eStencil;
    }

    // bbport: storage writes never convert to sRGB and sRGB formats cannot be storage images
    // (AMD's driver loses the device on such a view; NVIDIA tolerates it): write through the
    // UNORM format, which stores the same bytes.
    if (info.is_storage) {
        switch (format) {
        case vk::Format::eR8G8B8A8Srgb:
            format = vk::Format::eR8G8B8A8Unorm;
            break;
        case vk::Format::eB8G8R8A8Srgb:
            format = vk::Format::eB8G8R8A8Unorm;
            break;
        case vk::Format::eA8B8G8R8SrgbPack32:
            format = vk::Format::eA8B8G8R8UnormPack32;
            break;
        case vk::Format::eR8Srgb:
            format = vk::Format::eR8Unorm;
            break;
        case vk::Format::eR8G8Srgb:
            format = vk::Format::eR8G8Unorm;
            break;
        default:
            break;
        }
    }

    const vk::ImageViewCreateInfo image_view_ci = {
        .pNext = &usage_ci,
        .image = image.GetImage(),
        .viewType = ConvertImageViewType(info.type),
        .format = instance.GetSupportedFormat(format, image.format_features),
        .components = info.mapping,
        .subresourceRange{
            .aspectMask = aspect,
            .baseMipLevel = info.range.base.level,
            .levelCount = info.range.extent.levels,
            .baseArrayLayer = info.range.base.layer,
            .layerCount = info.range.extent.layers,
        },
    };
    // bbport diagnosis: a view whose texels are larger than the image's makes the GPU address
    // memory past the image (Vulkan only allows size-compatible formats).
    {
        const vk::Format view_format = image_view_ci.format;
        const vk::Format image_format = image.info.pixel_format;
        const auto texel_bytes = [](vk::Format f) {
            const auto extent = vk::blockExtent(f);
            return double(vk::blockSize(f)) / double(extent[0] * extent[1]);
        };
        const bool depth_image = static_cast<bool>(
            image.aspect_mask & (vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil));
        if (view_format != image_format &&
            (depth_image || texel_bytes(view_format) != texel_bytes(image_format))) {
            static std::atomic<u32> reported{0};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 64) {
                std::printf("Texture cache: view format %s (%.2f bytes/texel) on %s image (%.2f) "
                            "%ux%ux%u at 0x%llx, VkImage 0x%llx\n",
                            vk::to_string(view_format).c_str(), texel_bytes(view_format),
                            vk::to_string(image_format).c_str(), texel_bytes(image_format),
                            image.info.size.width, image.info.size.height, image.info.size.depth,
                            static_cast<unsigned long long>(image.info.guest_address),
                            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(
                                static_cast<VkImage>(image.GetImage()))));
            }
        }
    }
    if (!IsViewTypeCompatible(info.type, image.info.type)) {
        LOG_ERROR(Render_Vulkan, "image view type {} is incompatible with image type {}",
                  magic_enum::enum_name(info.type), magic_enum::enum_name(image.info.type));
    }

    auto [view_result, view] = instance.GetDevice().createImageViewUnique(image_view_ci);
    ASSERT_MSG(view_result == vk::Result::eSuccess, "Failed to create image view: {}",
               vk::to_string(view_result));
    image_view = std::move(view);

    const auto view_aspect = aspect & vk::ImageAspectFlagBits::eDepth     ? "Depth"
                             : aspect & vk::ImageAspectFlagBits::eStencil ? "Stencil"
                                                                          : "Color";
    Vulkan::SetObjectName(
        instance.GetDevice(), *image_view, "ImageView {}x{}x{} {:#x}:{:#x} {}:{} {}:{} ({})",
        image.info.size.width, image.info.size.height, image.info.size.depth,
        image.info.guest_address, image.info.guest_size, info.range.base.level,
        info.range.base.level + info.range.extent.levels - 1, info.range.base.layer,
        info.range.base.layer + info.range.extent.layers - 1, view_aspect);
}

ImageView::~ImageView() = default;

} // namespace VideoCore
