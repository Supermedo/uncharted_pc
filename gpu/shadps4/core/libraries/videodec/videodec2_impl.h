// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "videodec2.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace Libraries::Videodec2 {

/// BB_GAME_RES (WxH): the games read decoded movie frames at their display size, so frames are
/// fitted (aspect kept, black bars) into a canvas of that size. 0 when the game runs at 1920x1080.
void GameResolution(s32& width, s32& height);

class VdecDecoder {
public:
    VdecDecoder(const OrbisVideodec2DecoderConfigInfo& configInfo,
                const OrbisVideodec2DecoderMemoryInfo& memoryInfo);
    ~VdecDecoder();

    s32 Decode(const OrbisVideodec2InputData& inputData, OrbisVideodec2FrameBuffer& frameBuffer,
               OrbisVideodec2OutputInfo& outputInfo);
    s32 Flush(OrbisVideodec2FrameBuffer& frameBuffer, OrbisVideodec2OutputInfo& outputInfo);
    s32 Reset();

private:
    AVFrame* ConvertNV12Frame(AVFrame& frame);
    /// The frame fitted into the game's display size (frame is consumed), or the frame itself.
    AVFrame* FitToDisplay(AVFrame* frame);

private:
    AVCodecContext* m_codec_context = nullptr;
    SwsContext* m_sws_context = nullptr;
    SwsContext* m_fit_context = nullptr;
};

} // namespace Libraries::Videodec2