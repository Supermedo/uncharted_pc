// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

/**
 * @brief Creates a vulkan shader module from SPIR-V bytecode.
 * @param code The SPIR-V bytecode data.
 * @param device The vulkan device handle
 */
vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device);

/**
 * @brief bbport: the group size BB_CS_SPLIT_1024 asks a 1024-thread compute shader to run with.
 *
 * The variable is BB_CS_SPLIT_1024: unset or 0 leaves the shader alone (the split is off by
 * default), any non-zero value runs it as 512-thread groups.
 *
 * @return The group size, or 0 when the split is disabled.
 */
u32 SplitWorkgroupTarget();

/**
 * @brief bbport: rewrites a 1024-thread compute shader to run `threads` threads per group.
 *
 * A shader that needs 64 registers per thread asks for 64 * 1024 = 65536 registers for one
 * group, the whole register file of an SM on Pascal, and the launch then fails with
 * SKEDCHECK19_CTA_REGISTER_CONSUMPTION as soon as anything else is resident. The guest
 * computes its global invocation index as
 * "workgroup_id * 1024 + local_id"; running the same work as 1024/threads groups of `threads`
 * threads keeps that index (and every buffer offset derived from it) identical, because the
 * caller multiplies the group count by 1024/threads: (2g + k) * 512 + (l - 512k) ==
 * g * 1024 + l.
 *
 * Only the exact shape is rewritten: LocalSize 1024 1 1, the workgroup id's x component used
 * once, multiplied by the 1024 constant, added to the local id's x component, and no workgroup
 * memory or barriers (which would tie the threads of a group together).
 *
 * @param code The SPIR-V bytecode, rewritten in place when the shape matches.
 * @param threads The new group size (a divisor of 1024, less than 1024).
 * @return true when the module was rewritten.
 */
bool SplitWorkgroup(std::vector<u32>& code, u32 threads);

} // namespace Vulkan
