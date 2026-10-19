// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

struct AVFrame;

namespace Libraries::Videodec {

/// `height_alignment`: rows the luma plane is padded to (the chroma plane follows it).
void CopyNV12Data(u8* dst, u64 max_size, const AVFrame& src, u32 height_alignment = 16,
                  u32 pitch_alignment = 64);

}
