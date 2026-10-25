// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>
#include "common/assert.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

// bbport: swaps the object motion address stand-ins in a vertex shader (runtime_info.h,
// MotionVectors) for the session's buffer addresses. The cache stores the stand-ins.
static void PatchMotionAddresses(std::vector<u32>& code) {
    using Shader::MotionVectors;
    constexpr u32 OpConstant = 43;
    for (size_t at = 5; at < code.size();) {
        const u32 op = code[at] & 0xFFFF;
        const u32 words = code[at] >> 16;
        if (words == 0 || at + words > code.size()) {
            return;
        }
        if (op == OpConstant && words == 5) {
            const u64 value = u64(code[at + 3]) | u64(code[at + 4]) << 32;
            u64 patched = value;
            if (value - MotionVectors::ParamsPlaceholder <= 16) {
                patched = MotionVectors::params_address + (value - MotionVectors::ParamsPlaceholder);
            } else if (value == MotionVectors::PositionsPlaceholder) {
                patched = MotionVectors::positions_address;
            }
            if (patched != value) {
                code[at + 3] = u32(patched);
                code[at + 4] = u32(patched >> 32);
            }
        }
        at += words;
    }
}

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

u32 SplitWorkgroupTarget() {
    static const u32 target = [] {
        // bbport: off unless BB_CS_SPLIT_1024 is set to a non-zero value.
        const char* env = std::getenv("BB_CS_SPLIT_1024");
        return env != nullptr && std::atoi(env) != 0 ? 512u : 0u;
    }();
    return target;
}

bool SplitWorkgroup(std::vector<u32>& code, u32 threads) {
    constexpr u32 OpExecutionMode = 16;
    constexpr u32 OpConstant = 43;
    constexpr u32 OpFunction = 54;
    constexpr u32 OpVariable = 59;
    constexpr u32 OpLoad = 61;
    constexpr u32 OpDecorate = 71;
    constexpr u32 OpMemberDecorate = 72;
    constexpr u32 OpCompositeExtract = 81;
    constexpr u32 OpIAdd = 128;
    constexpr u32 OpIMul = 132;
    constexpr u32 OpControlBarrier = 224;
    constexpr u32 OpMemoryBarrier = 225;
    constexpr u32 ExecutionModeLocalSize = 17;
    constexpr u32 DecorationBuiltIn = 11;
    constexpr u32 BuiltInWorkgroupId = 26;
    constexpr u32 BuiltInLocalInvocationId = 27;
    constexpr u32 StorageClassWorkgroup = 4;
    constexpr u32 GuestThreads = 1024;

    if (code.size() < 6 || threads == 0 || threads >= GuestThreads ||
        GuestThreads % threads != 0) {
        return false;
    }

    u32 workgroup_var = 0;
    u32 local_var = 0;
    u32 workgroup_load = 0;
    u32 local_load = 0;
    u32 workgroup_x = 0;
    u32 local_x = 0;
    u32 u32_type = 0;
    u32 constants[4] = {};
    u32 num_constants = 0;
    u32 product = 0;
    u32 index = 0;
    u32 multiplies = 0;
    size_t workgroup_x_at = 0;
    size_t local_x_at = 0;
    size_t product_at = 0;
    size_t local_size_at = 0;
    size_t multiply_operand_at = 0;
    size_t insert_at = 0;

    for (size_t at = 5; at < code.size();) {
        const u32 words = code[at] >> 16;
        if (words == 0 || at + words > code.size()) {
            return false;
        }
        switch (code[at] & 0xFFFF) {
        case OpExecutionMode:
            if (words == 6 && code[at + 2] == ExecutionModeLocalSize &&
                code[at + 3] == GuestThreads && code[at + 4] == 1 && code[at + 5] == 1) {
                local_size_at = at + 3;
            }
            break;
        case OpDecorate:
            if (words == 4 && code[at + 2] == DecorationBuiltIn) {
                if (code[at + 3] == BuiltInWorkgroupId) {
                    workgroup_var = code[at + 1];
                } else if (code[at + 3] == BuiltInLocalInvocationId) {
                    local_var = code[at + 1];
                } else {
                    return false; // NumWorkGroups, WorkgroupSize, SubgroupId, ... depend on the group
                }
            }
            break;
        case OpMemberDecorate:
            if (words == 5 && code[at + 3] == DecorationBuiltIn) {
                return false; // a builtin block member is not the direct-variable shape handled here
            }
            break;
        case OpVariable:
            if (words == 4 && code[at + 3] == StorageClassWorkgroup) {
                return false; // the threads of the group share state: they cannot be split
            }
            break;
        case OpControlBarrier:
        case OpMemoryBarrier:
            return false; // ditto
        case OpLoad:
            if (words == 4) {
                if (workgroup_var != 0 && code[at + 3] == workgroup_var) {
                    workgroup_load = code[at + 2];
                } else if (local_var != 0 && code[at + 3] == local_var) {
                    local_load = code[at + 2];
                }
            }
            break;
        case OpCompositeExtract:
            if (words == 5 && code[at + 4] == 0) {
                if (workgroup_load != 0 && code[at + 3] == workgroup_load) {
                    workgroup_x = code[at + 2];
                    workgroup_x_at = at + 2;
                } else if (local_load != 0 && code[at + 3] == local_load) {
                    local_x = code[at + 2];
                    local_x_at = at + 2;
                }
            }
            break;
        case OpConstant:
            if (words == 4 && code[at + 3] == GuestThreads && num_constants < 4) {
                constants[num_constants++] = code[at + 2];
                u32_type = code[at + 1];
            }
            break;
        case OpIMul:
            if (words == 5 && workgroup_x != 0) {
                const auto is_guest_size = [&constants, num_constants](u32 id) {
                    for (u32 i = 0; i < num_constants; ++i) {
                        if (constants[i] == id) {
                            return true;
                        }
                    }
                    return false;
                };
                const bool left_constant = is_guest_size(code[at + 3]);
                const bool right_constant = is_guest_size(code[at + 4]);
                if ((code[at + 3] == workgroup_x && right_constant) ||
                    (code[at + 4] == workgroup_x && left_constant)) {
                    ++multiplies;
                    product = code[at + 2];
                    product_at = at + 2;
                    multiply_operand_at = left_constant ? at + 3 : at + 4;
                }
            }
            break;
        case OpIAdd:
            if (words == 5 && product != 0 && local_x != 0 &&
                ((code[at + 3] == product && code[at + 4] == local_x) ||
                 (code[at + 3] == local_x && code[at + 4] == product))) {
                index = code[at + 2];
            }
            break;
        case OpFunction:
            if (insert_at == 0) {
                insert_at = at;
            }
            break;
        default:
            break;
        }
        at += words;
    }

    if (local_size_at == 0 || insert_at == 0 || workgroup_x == 0 || local_x == 0 ||
        product == 0 || index == 0 || multiplies != 1) {
        return false;
    }
    // The three values have to feed the global index and nothing else, otherwise halving them
    // would change another use. (Counting every operand word is a superset: a coincidental
    // match only makes the rewrite refuse to run.)
    const auto count_uses = [&code](u32 id, size_t definition) {
        u32 uses = 0;
        for (size_t at = 5; at < code.size();) {
            const u32 words = code[at] >> 16;
            if (words == 0 || at + words > code.size()) {
                return uses;
            }
            for (size_t i = at + 1; i < at + words; ++i) {
                if (i != definition && code[i] == id) {
                    ++uses;
                }
            }
            at += words;
        }
        return uses;
    };
    if (count_uses(workgroup_x, workgroup_x_at) != 1 || count_uses(local_x, local_x_at) != 1 ||
        count_uses(product, product_at) != 1) {
        return false;
    }

    code[local_size_at] = threads;
    const u32 new_constant = code[3]; // the header's bound is the first free id
    const u32 instruction[4] = {(4u << 16) | OpConstant, u32_type, new_constant, threads};
    code.insert(code.begin() + insert_at, instruction, instruction + 4);
    code[multiply_operand_at + 4] = new_constant; // the insertion is before the function body
    code[3] = new_constant + 1;
    return true;
}

vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device) {
    // Test knob: BB_TEST_SPV_SALT=<n> changes the SPIR-V generator word of every module, so the
    // driver's own on-disk shader cache cannot hit (a cold-driver measurement on a used PC).
    static const char* salt_env = std::getenv("BB_TEST_SPV_SALT");
    std::vector<u32> patched;
    if (Shader::MotionVectors::positions_address != 0 && code.size() > 5) {
        // Cheap pre-check: only vertex shaders with the motion path contain the stand-ins.
        bool has_placeholder = false;
        for (size_t i = 5; i + 1 < code.size() && !has_placeholder; ++i) {
            has_placeholder = code[i + 1] == u32(Shader::MotionVectors::ParamsPlaceholder >> 32) &&
                              code[i] == u32(Shader::MotionVectors::ParamsPlaceholder);
        }
        if (has_placeholder) {
            patched.assign(code.begin(), code.end());
            PatchMotionAddresses(patched);
            code = patched;
        }
    }
    std::vector<u32> movie_patched;
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
                movie_patched = std::move(scaled);
                code = movie_patched;
            }
        }
    }
    std::vector<u32> salted;
    if (salt_env && code.size() > 5) {
        salted.assign(code.begin(), code.end());
        salted[2] ^= u32(std::strtoul(salt_env, nullptr, 0)) * 2654435761u;
        code = salted;
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
