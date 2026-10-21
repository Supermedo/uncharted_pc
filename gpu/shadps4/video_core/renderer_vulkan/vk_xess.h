// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// bbport: Intel XeSS Super Resolution through Intel's libxess.dll (XeSS SDK, Intel Simplified
// Software License), loaded at run time from next to bb-probe.exe (BB_XESS_DLL=<file> overrides
// the place). The port contains no Intel code: the entry points are looked up with GetProcAddress
// and the few structures it passes are declared in vk_xess.cpp from the SDK's public C header.
// Without the DLL, with BB_XESS=0, or when the GPU or driver lacks what XeSS asks for, nothing
// changes. It takes the inputs DLSS and FSR get: scene color, depth, render-pixel motion vectors
// (current to previous) and the Halton jitter. Runs on any Vulkan GPU (Intel XMX/DP4a path on Intel,
// the DP4a path elsewhere).

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Xess {
public:
    /// The process-wide instance, or null when libxess.dll is absent or BB_XESS=0.
    static Xess* Get();
    ~Xess();

    /// Instance and device creation: adds what XeSS needs (or disables XeSS when it is missing).
    void AppendInstanceExtensions(std::vector<const char*>& enabled);
    void AppendDeviceExtensions(vk::Instance instance, vk::PhysicalDevice physical,
                                std::vector<const char*>& enabled);
    /// Last step before vkCreateDevice: `features` is the head (a VkPhysicalDeviceFeatures2) of the
    /// feature chain; XeSS enables its features in it and may append structures to the chain's end.
    void PatchDeviceFeatures(vk::Instance instance, vk::PhysicalDevice physical, void* features);
    /// After the device exists. The XeSS context itself is made on the first use, on the thread
    /// that records the frames (XeSS is not thread safe).
    void Initialize(vk::Instance instance, vk::PhysicalDevice physical, vk::Device device);
    void Shutdown();

    [[nodiscard]] bool Available() const;
    /// Why XeSS cannot run on this system (empty when it can, or before device creation).
    [[nodiscard]] std::string Problem() const;

    struct Resource {
        vk::Image image;
        vk::ImageView view;
        vk::Format format;
        vk::ImageAspectFlags aspect;
        u32 width, height;
    };
    struct FeatureDesc {
        u32 input_width, input_height, output_width, output_height;
        int quality; ///< xess_quality_settings_t
        bool hdr;
        bool operator==(const FeatureDesc&) const = default;
    };
    struct Frame {
        Resource color, depth, motion, output;
        float jitter_x, jitter_y;
        bool reset;
    };

    /// XeSS quality preset for an output/render size ratio (AA 1.0, Ultra Quality Plus 1.3,
    /// Ultra Quality 1.5, Quality 1.7, Balanced 2.0, Performance 2.3, Ultra Performance 3.0).
    static int QualityForScale(float scale);
    [[nodiscard]] bool HasFeature(const FeatureDesc& desc) const;
    /// Creates the context on first use and (re)initializes it. Blocking (kernel compilation can
    /// take a few seconds the first time); the GPU must be done with the previous feature.
    bool CreateFeature(const FeatureDesc& desc);
    /// Records XeSS into `command`. Color, depth and motion must be in ShaderReadOnlyOptimal, the
    /// output in General.
    bool Evaluate(vk::CommandBuffer command, const Frame& frame);
    void ReleaseFeature();

private:
    Xess();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
