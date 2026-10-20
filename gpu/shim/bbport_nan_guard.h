// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the NaN guard on colour outputs, shared by the shader recompiler and the shader cache.
#pragma once

#include <cstdlib>
#include <string>

namespace BbNanGuard {

/// Colour outputs to float render targets: NaN becomes 0, so one bad pixel cannot grow into a
/// black 16x16 block through the games' tiled post-processing (Uncharted 2 on the Steam Deck).
/// BB_NAN_GUARD: 1 (default) NaN only, 2 also clamps +-Inf to the half-float range (that broke
/// Uncharted 1's lighting and fog: blue trees), 0 off.
inline int NanGuardMode() {
    static const int mode = [] {
        const char* env = std::getenv("BB_NAN_GUARD");
        return env && (env[0] == '0' || env[0] == '2') ? env[0] - '0' : 1;
    }();
    return mode;
}

inline bool ExportNanGuard() {
    return NanGuardMode() != 0;
}

inline bool EnvFlag(const char* name) {
    const char* env = std::getenv(name);
    return env && env[0] == '1';
}

/// BB_SATURATE_NAN: GCN's output clamp (and saturate) turns NaN into 0, SPIR-V's FClamp leaves
/// it undefined. 1: vertex shaders get the GCN result, 2: vertex and pixel shaders (compute
/// shaders never: in all stages a U3 shader hung the GPU). 0 (default): off.
inline int SaturateNanMode() {
    static const int mode = [] {
        const char* env = std::getenv("BB_SATURATE_NAN");
        return env && (env[0] == '1' || env[0] == '2') ? env[0] - '0' : 0;
    }();
    return mode;
}

/// BB_CMP_UNORDERED=1: V_CMP(X)_N{GE,GT,LE,LT,EQ,LG}_F32 and V_CMP_N{GE,GT,LE,LT,EQ}_F64 are the
/// negated compares, true when an operand is NaN (GCN ISA: NEQ = !(a == b) is unordered, LG is
/// ordered); otherwise they are translated as the ordered opposite compares, false for NaN, which
/// flips selects and kills that the game uses to step around NaN (x != x as an isnan() test).
/// V_CMP_NLG_F32/F64 and V_CMPX_*_F64 are not translated at all.
inline bool UnorderedNegatedCompares() {
    static const bool enabled = EnvFlag("BB_CMP_UNORDERED");
    return enabled;
}

/// BB_CMP_CLASS_EXACT=1: V_CMP_CLASS_F32 tests every class bit of its mask. Otherwise a mask with
/// both NaN bits is tested as "is NaN" alone: a !isfinite() test (mask 0x207) lets +-Inf through;
/// and the negative classes (0x3c) as x <= -0, which +0 passes.
inline bool ExactCmpClass() {
    static const bool enabled = EnvFlag("BB_CMP_CLASS_EXACT");
    return enabled;
}

/// BB_F16_RTZ=1: V_CVT_PKRTZ_F16_F32 rounds toward zero, so a finite value beyond the half-float
/// range becomes +-65504 on the PS4 (Inf and NaN stay). The recompiler folds the pack into the
/// colour export and the f32 value reaches the RGBA16F target, where Vulkan lets the host round
/// it to +-Inf (NVIDIA; AMD's drivers pack colour exports with V_CVT_PKRTZ as well), and a pack
/// kept in the shader (buffers, LDS) is rounded to nearest. Clamps finite values to +-65504
/// before the pack; values in range keep the host's rounding (at most one half-float ulp off).
/// run.py sets it for Uncharted 2 (its dropped weapons: white blocks spread by the bloom).
inline bool HalfRoundTowardZero() {
    static const bool enabled = EnvFlag("BB_F16_RTZ");
    return enabled;
}

/// BB_NONFINITE_MARK=1 (diagnosis): a colour output with a NaN component becomes magenta, +Inf
/// green, -Inf blue, and (float targets narrower than 32 bits only) a finite value beyond
/// +-65504 yellow (8.0 in those channels of the target, after its swizzle), before the NaN
/// guard, so the draw that writes a non-finite colour shows at its own pixels.
inline bool NonFiniteMark() {
    static const bool enabled = EnvFlag("BB_NONFINITE_MARK");
    return enabled;
}

/// BB_ALPHA_TO_COVERAGE=1: DB_ALPHA_TO_MASK enables Vulkan's alpha to coverage. Pipeline state
/// only, but the pipeline keys are stored with the shaders: its own cache keeps the default one
/// free of pipelines no default run can use (they would be compiled again at every start).
inline bool AlphaToCoverage() {
    static const bool enabled = EnvFlag("BB_ALPHA_TO_COVERAGE");
    return enabled;
}

/// Shaders (and the pipeline keys stored with them) compiled in each mode are cached apart.
inline const char* ShaderCacheSuffix() {
    static const std::string suffix = [] {
        std::string s = NanGuardMode() == 1 ? "-nan1" : NanGuardMode() == 2 ? "-nan2" : "";
        if (SaturateNanMode() != 0) {
            s += SaturateNanMode() == 1 ? "-sat1" : "-sat2";
        }
        if (UnorderedNegatedCompares()) {
            s += "-cmpu";
        }
        if (ExactCmpClass()) {
            s += "-cls";
        }
        if (HalfRoundTowardZero()) {
            s += "-rtz";
        }
        if (NonFiniteMark()) {
            s += "-mark";
        }
        if (AlphaToCoverage()) {
            s += "-a2c";
        }
        return s;
    }();
    return suffix.c_str();
}

} // namespace BbNanGuard
