// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the aiming autofocus at render sizes other than 1920x1080, shared by the shader
// recompiler and the shader cache.
#pragma once

#include <cstdlib>

namespace BbFocusFix {

/// Uncharted 2 and 3's aiming autofocus (pixel shader 9f801363) samples the scene depth around
/// constants[0..1], the screen centre in 0..1, with constants[6..7] = 1 / render size. The game
/// computes the centre as 960 and 540 times those, so at 3840x2160 it measured at 0.25, 0.25
/// (a pillar next to the camera, focus ~1 m) and blurred everything behind Drake while aiming.
/// The translated shader takes the centre as constants[0] / (constants[6] * 1920) and
/// constants[1] / (constants[7] * 1080): unchanged at 1920x1080, 0.5 at any other size.
/// BB_FOCUS_FIX=0: off.
inline bool FocusCentreShader(unsigned long long hash) {
    static const bool on = [] {
        const char* env = std::getenv("BB_FOCUS_FIX");
        return !(env && env[0] == '0');
    }();
    return on && static_cast<unsigned>(hash) == 0x9f801363u;
}

} // namespace BbFocusFix
