// SPDX-License-Identifier: GPL-2.0-or-later
// Run button-prompts-test with BB_BUTTON_PROMPTS=auto. No window or GPU is needed.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "video_core/texture_cache/button_prompts.h"

// Use the same embedded font as the renderer without linking the in-game overlay.
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n.global bb_font_ttf\nbb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n.text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n.global bb_font_ttf\nbb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n.previous\n");
#endif

using namespace VideoCore::ButtonPrompts;

static const u8* Pixel(const Patch& patch, u32 x, u32 y) {
    assert(x >= patch.x && x < patch.x + patch.width);
    assert(y >= patch.y && y < patch.y + patch.height);
    return &patch.data[((y - patch.y) * patch.width + x - patch.x) * 4];
}

int main(int argc, char** argv) {
    const bool gamecube = argc > 1 && !std::strcmp(argv[1], "gamecube");
    const int device = gamecube ? 4 : 3;
    SetDevice(device);
    assert(Shown() == (gamecube ? Style::GameCube : Style::Nintendo));
    constexpr u64 hashes[] = {0xad5ada34fd34c76bull, 0x97779d0906f16551ull,
                              0x8d03f2ff457f032dull, 0xae0c06306763d424ull};
    const std::vector<Patch>* sheets[4];
    for (int i = 0; i < 4; ++i) {
        sheets[i] = Find(hashes[i], 512, 512, 4, Format::Rgba8);
        assert(sheets[i] && !sheets[i]->empty());
    }
    // Circle, Square, Triangle, Cross cells (Switch: A/Y/X/B; GameCube: X/B/Y/A).
    constexpr u32 bounds[4][4] = {{5,81,59,134}, {69,80,123,134},
                                  {134,81,187,134}, {197,81,251,134}};
    for (int sheet : {0, 1, 3}) {
        for (int button = 0; button < 4; ++button) {
            const auto& p = (*sheets[sheet])[button];
            assert(p.mip == 0);
            const auto& b = bounds[button];
            const float cx = (b[0] + b[2]) * 0.5f, cy = (b[1] + b[3]) * 0.5f;
            const float radius = std::min(b[2] - b[0], b[3] - b[1]) * 0.5f - 2;
            unsigned inside = 0, outside = 0;
            for (u32 y = p.y; y < p.y + p.height; ++y) {
                for (u32 x = p.x; x < p.x + p.width; ++x) {
                    const float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy) - radius;
                    const auto alpha = Pixel(p, x, y)[sheet == 3 ? 0 : 3];
                    if (d < -2) {
                        if (alpha != 255) {
                            std::fprintf(stderr, "Translucent face: sheet %d button %d (%u,%u) alpha=%u\n",
                                         sheet, button, x, y, alpha);
                            return 1;
                        }
                        ++inside;
                    } else if (d > 4) {
                        if (alpha != 0) {
                            std::fprintf(stderr, "Rectangular background: sheet %d button %d (%u,%u) alpha=%u\n",
                                         sheet, button, x, y, alpha);
                            return 1;
                        }
                        ++outside;
                    }
                }
            }
            assert(inside > 0 && outside > 0);
        }
    }
    // U3's separate mask must carry exactly the alpha used by U1/U2, at every mip.
    assert(sheets[0]->size() == sheets[3]->size());
    for (size_t i = 0; i < sheets[0]->size(); ++i) {
        const auto& rgba = (*sheets[0])[i];
        const auto& mask = (*sheets[3])[i];
        const auto& colour = (*sheets[2])[i];
        assert(rgba.data == (*sheets[1])[i].data);
        assert(rgba.data.size() == mask.data.size() && mask.data.size() == colour.data.size());
        for (size_t j = 0; j < rgba.data.size(); j += 4) {
            assert(mask.data[j] == rgba.data[j + 3]);
            assert(mask.data[j + 1] == mask.data[j] && mask.data[j + 2] == mask.data[j]);
            assert(mask.data[j + 3] == 0 && colour.data[j + 3] == 0);
        }
    }
    SetDevice(1);
    assert(Shown() == Style::Original);
    assert(Find(hashes[0], 512, 512, 4, Format::Rgba8)->empty());
    SetDevice(device);
    assert(Find(hashes[0], 512, 512, 4, Format::Rgba8) == sheets[0]);
    SetDevice(gamecube ? 3 : 4);
    const auto* other = Find(hashes[0], 512, 512, 4, Format::Rgba8);
    for (int button = 0; button < 4; ++button) {
        assert((*other)[button].data != (*sheets[0])[button].data);
    }
    std::printf("PASS: %s opaque faces, transparent corners, U1/U2 RGBA, U3 masks, four mips and separate style cache\n",
                gamecube ? "GameCube" : "Switch");
}
