// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;

layout (push_constant) uniform settings {
    float gamma;
    bool hdr;
    bool srgb_input;
    uint downsample_filter; // 0 bilinear, 1 Lanczos2, 2 area
} pp;

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

// Exact inverse of gamma() at unit gamma, for buffers that are sRGB encoded but must be sampled
// through a UNORM view because Vulkan has no sRGB variant of the 10-bit format.
vec3 degamma(vec3 rgb) {
    return mix(
        pow(max(rgb + b, 0.0) / a, vec3(2.4)),
        rgb / d,
        lessThan(rgb, vec3(d * cutoff))
    );
}

vec4 linearTexel(ivec2 p) {
    ivec2 size = textureSize(texSampler, 0);
    vec4 value = texelFetch(texSampler, clamp(p, ivec2(0), size - 1), 0);
    if (!pp.hdr && pp.srgb_input) value.rgb = degamma(value.rgb);
    return value;
}

float lanczos2(float x) {
    x = abs(x);
    if (x < 0.00001) return 1.0;
    if (x >= 2.0) return 0.0;
    const float pi = 3.14159265358979323846;
    float p = pi * x;
    return sin(p) * sin(p * 0.5) / (p * p * 0.5);
}

vec4 downsample(vec2 footprint) {
    vec2 size = vec2(textureSize(texSampler, 0));
    vec2 center = uv * size;
    vec2 scale = max(footprint, vec2(1.0));
    vec2 low = center - scale * 0.5;
    vec2 high = center + scale * 0.5;
    ivec2 first, last;
    if (pp.downsample_filter == 2u) {
        // Exact source-pixel coverage of the destination pixel.
        first = ivec2(floor(low));
        last = ivec2(ceil(high)) - 1;
    } else {
        // Widen the kernel to reject frequencies above the output Nyquist limit.
        center -= 0.5;
        first = ivec2(ceil(center - 2.0 * scale));
        last = ivec2(floor(center + 2.0 * scale));
    }
    vec4 sum = vec4(0.0);
    float total = 0.0;
    for (int y = first.y; y <= last.y; ++y) {
        float wy = pp.downsample_filter == 2u
            ? max(0.0, min(high.y, float(y + 1)) - max(low.y, float(y)))
            : lanczos2((float(y) - center.y) / scale.y);
        for (int x = first.x; x <= last.x; ++x) {
            float wx = pp.downsample_filter == 2u
                ? max(0.0, min(high.x, float(x + 1)) - max(low.x, float(x)))
                : lanczos2((float(x) - center.x) / scale.x);
            float weight = wx * wy;
            sum += linearTexel(ivec2(x, y)) * weight;
            total += weight;
        }
    }
    vec4 result = sum / max(total, 0.000001);
    if (pp.downsample_filter == 1u) {
        // Bound negative-lobe overshoot to the local source range, without global
        // clipping of HDR highlights or an extra sharpening pass.
        ivec2 p = ivec2(floor(center));
        vec4 a = linearTexel(p), b = linearTexel(p + ivec2(1, 0));
        vec4 c = linearTexel(p + ivec2(0, 1)), d = linearTexel(p + ivec2(1, 1));
        result = clamp(result, min(min(a, b), min(c, d)), max(max(a, b), max(c, d)));
    }
    return result;
}

void main() {
    vec2 size = vec2(textureSize(texSampler, 0));
    vec2 footprint = vec2(length(dFdx(uv) * size), length(dFdy(uv) * size));
    bool reduced = any(greaterThan(footprint, vec2(1.0001)));
    vec4 color_linear;
    if (reduced && pp.downsample_filter != 0u) {
        color_linear = downsample(footprint);
    } else {
        color_linear = texture(texSampler, uv);
        if (!pp.hdr && pp.srgb_input) color_linear.rgb = degamma(color_linear.rgb);
    }
    if (pp.hdr) {
        color = color_linear;
    } else {
        color = vec4(gamma(color_linear.rgb), color_linear.a);
    }
}
