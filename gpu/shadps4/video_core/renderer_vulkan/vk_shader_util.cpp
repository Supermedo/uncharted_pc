// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>
#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

// bbport: BB_GAME_RES (the game renders at another size than 1920x1080). The compute shader that
// composes the decoded movie frame maps each output pixel to the video texture with constants
// baked for 1920x1088: (x + 0.5) / 1920 and (y + 0.5) / 1088. With the movie fitted into a W x H
// canvas (videodec2_impl.cpp) the constants become 1 / W and 1 / H. The shader is found by those
// four floats, so every game and version is covered.
static bool PatchMovieScale(std::vector<u32>& code) {
    static const std::pair<u32, u32> size = [] {
        unsigned w = 0, h = 0;
        const char* res = std::getenv("BB_GAME_RES");
        if (!res || std::sscanf(res, "%ux%u", &w, &h) != 2 || w < 640 || h < 360 ||
            (w == 1920 && h == 1080)) {
            return std::pair<u32, u32>{0, 0};
        }
        return std::pair<u32, u32>{w & ~1u, h & ~1u};
    }();
    if (!size.first) {
        return false;
    }
    const auto bits = [](float f) {
        u32 v;
        std::memcpy(&v, &f, sizeof(v));
        return v;
    };
    const u32 from[4] = {bits(1.0f / 1920.0f), bits(0.5f / 1920.0f), bits(1.0f / 1088.0f),
                         bits(0.5f / 1088.0f)};
    const u32 to[4] = {bits(1.0f / float(size.first)), bits(0.5f / float(size.first)),
                       bits(1.0f / float(size.second)), bits(0.5f / float(size.second))};
    size_t found[4] = {~size_t{0}, ~size_t{0}, ~size_t{0}, ~size_t{0}};
    constexpr u32 OpConstant = 43;
    for (size_t at = 5; at < code.size();) {
        const u32 op = code[at] & 0xFFFF;
        const u32 words = code[at] >> 16;
        if (words == 0 || at + words > code.size()) {
            return false;
        }
        if (op == OpConstant && words == 4) {
            for (int i = 0; i < 4; ++i) {
                if (code[at + 3] == from[i]) {
                    found[i] = at + 3;
                }
            }
        }
        at += words;
    }
    for (int i = 0; i < 4; ++i) {
        if (found[i] == ~size_t{0}) {
            return false;
        }
    }
    for (int i = 0; i < 4; ++i) {
        code[found[i]] = to[i];
    }
    static bool printed = false;
    if (!printed) {
        printed = true;
        std::printf("Movie compose shader: video scale 1/%u x 1/%u\n", size.first, size.second);
    }
    return true;
}

vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device) {
    std::vector<u32> patched;
    if (code.size() > 5 && std::getenv("BB_GAME_RES")) {
        // Cheap pre-check before copying: the 1 / 1920 constant.
        float inv = 1.0f / 1920.0f;
        u32 inv_bits;
        std::memcpy(&inv_bits, &inv, sizeof(inv_bits));
        bool has = false;
        for (size_t i = 5; i < code.size() && !has; ++i) {
            has = code[i] == inv_bits;
        }
        if (has) {
            std::vector<u32> scaled(code.begin(), code.end());
            if (PatchMovieScale(scaled)) {
                patched = std::move(scaled);
                code = patched;
            }
        }
    }
    const vk::ShaderModuleCreateInfo shader_info = {
        .codeSize = code.size() * sizeof(u32),
        .pCode = code.data(),
    };

    auto [module_result, module] = device.createShaderModule(shader_info);
    ASSERT_MSG(module_result == vk::Result::eSuccess, "Failed to compile SPIR-V shader: {}",
               vk::to_string(module_result));
    return module;
}

} // namespace Vulkan
