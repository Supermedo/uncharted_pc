// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string_view>

#include "video_core/renderer_vulkan/vk_xess.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Vulkan {

int Xess::QualityForScale(float scale) {
    // XESS_QUALITY_SETTING_*: 100 ultra performance .. 104 ultra quality, 105 ultra quality plus,
    // 106 native AA.
    return scale >= 2.65f ? 100 : scale >= 2.15f ? 101 : scale >= 1.85f ? 102
         : scale >= 1.6f ? 103  : scale >= 1.4f ? 104  : scale >= 1.15f ? 105 : 106;
}

#ifdef _WIN32

namespace {
// The part of Intel's xess.h / xess_vk.h this file needs (the structures are plain C, 8-byte packed
// which is their natural layout on x64). Values and layout from XeSS SDK 3.0.
using XessContext = void*;
using XessResult = int;
constexpr XessResult XessSuccess = 0;
constexpr XessResult XessWarningOldDriver = 2;
constexpr u32 InitHighResMv = 1u << 0, InitAutoExposure = 1u << 8, InitLdrInput = 1u << 6;

struct XessVersion {
    u16 major, minor, patch, reserved;
};
struct Xess2d {
    u32 x, y;
};
struct XessVkImage {
    VkImageView imageView;
    VkImage image;
    VkImageSubresourceRange subresourceRange;
    VkFormat format;
    u32 width, height;
};
struct XessVkExecute {
    XessVkImage color, velocity, depth, exposure_scale, responsive_mask, output;
    float jitter_x, jitter_y, exposure_scale_value;
    u32 reset_history, input_width, input_height;
    Xess2d color_base, velocity_base, depth_base, responsive_base, reserved, output_base;
};
struct XessVkInit {
    Xess2d output_resolution;
    int quality;
    u32 init_flags, creation_node_mask, visible_node_mask;
    VkDeviceMemory temp_buffer_heap;
    u64 buffer_heap_offset;
    VkDeviceMemory temp_texture_heap;
    u64 texture_heap_offset;
    VkPipelineCache pipeline_cache;
};
static_assert(sizeof(XessVkImage) == 48);
static_assert(sizeof(XessVkExecute) == 6 * 48 + 24 + 48);
static_assert(sizeof(XessVkInit) == 64);

using GetVersionFn = XessResult (*)(XessVersion*);
using GetInstanceExtensionsFn = XessResult (*)(u32*, const char* const**, u32*);
using GetDeviceExtensionsFn = XessResult (*)(VkInstance, VkPhysicalDevice, u32*,
                                             const char* const**);
using GetDeviceFeaturesFn = XessResult (*)(VkInstance, VkPhysicalDevice, void**);
using CreateContextFn = XessResult (*)(VkInstance, VkPhysicalDevice, VkDevice, XessContext*);
using InitFn = XessResult (*)(XessContext, const XessVkInit*);
using ExecuteFn = XessResult (*)(XessContext, VkCommandBuffer, const XessVkExecute*);
using DestroyFn = XessResult (*)(XessContext);
using IsOptimalDriverFn = XessResult (*)(XessContext);
using LogCallback = void (*)(const char*, int);
using SetLoggingFn = XessResult (*)(XessContext, int, LogCallback);

std::filesystem::path ExecutableDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    path.resize(length);
    return std::filesystem::path{path}.parent_path();
}

std::filesystem::path LibraryPath() {
    if (const char* file = std::getenv("BB_XESS_DLL"); file && file[0]) {
        return std::filesystem::path{file};
    }
    return ExecutableDirectory() / L"libxess.dll";
}

void XessLog(const char* message, int level) {
    std::printf("XeSS: %s%s\n", level >= 3 ? "error: " : level == 2 ? "warning: " : "", message);
}
} // namespace

struct Xess::Impl {
    HMODULE module{};
    GetVersionFn get_version{};
    GetInstanceExtensionsFn get_instance_extensions{};
    GetDeviceExtensionsFn get_device_extensions{};
    GetDeviceFeaturesFn get_device_features{};
    CreateContextFn create_context{};
    InitFn init{};
    ExecuteFn execute{};
    DestroyFn destroy{};
    IsOptimalDriverFn is_optimal_driver{};
    SetLoggingFn set_logging{};
    vk::Instance instance;
    vk::PhysicalDevice physical;
    vk::Device device;
    XessContext context{};
    std::optional<FeatureDesc> feature;
    std::string problem;
    bool eligible{true}, instance_ready{}, device_ready{}, initialized{}, available{};
    bool auto_exposure{true};

    void Disable(std::string_view reason) {
        eligible = available = false;
        problem = reason;
        std::printf("XeSS: unavailable (%.*s)\n", int(reason.size()), reason.data());
    }
    void DestroyContext() {
        feature.reset();
        if (context && destroy) {
            destroy(context);
        }
        context = nullptr;
    }
};

Xess* Xess::Get() {
    static Xess* const xess = []() -> Xess* {
        const char* setting = std::getenv("BB_XESS");
        if (setting && setting[0] == '0') {
            return nullptr;
        }
        std::error_code error;
        if (!std::filesystem::is_regular_file(LibraryPath(), error)) {
            return nullptr;
        }
        auto* created = new Xess;
        std::atexit([] {
            if (Xess* live = Get()) {
                live->Shutdown();
            }
        });
        return created;
    }();
    return xess;
}

Xess::Xess() : impl{std::make_unique<Impl>()} {
    const auto path = LibraryPath();
    // The DLL's own dependencies (the Visual C++ runtime) are searched next to it first.
    impl->module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!impl->module) {
        const DWORD error = GetLastError();
        char text[160];
        std::snprintf(text, sizeof(text),
                      "libxess.dll did not load (error %lu; it needs the Visual C++ "
                      "Redistributable 2022 x64)", static_cast<unsigned long>(error));
        impl->Disable(text);
        return;
    }
    const auto load = [&](auto& pointer, const char* name) {
        pointer = reinterpret_cast<std::remove_reference_t<decltype(pointer)>>(
            GetProcAddress(impl->module, name));
        return pointer != nullptr;
    };
    const bool loaded =
        load(impl->get_version, "xessGetVersion") &&
        load(impl->get_instance_extensions, "xessVKGetRequiredInstanceExtensions") &&
        load(impl->get_device_extensions, "xessVKGetRequiredDeviceExtensions") &&
        load(impl->get_device_features, "xessVKGetRequiredDeviceFeatures") &&
        load(impl->create_context, "xessVKCreateContext") && load(impl->init, "xessVKInit") &&
        load(impl->execute, "xessVKExecute") && load(impl->destroy, "xessDestroyContext") &&
        load(impl->is_optimal_driver, "xessIsOptimalDriver") &&
        load(impl->set_logging, "xessSetLoggingCallback");
    if (!loaded) {
        impl->Disable("libxess.dll has no Vulkan entry points (needs XeSS SDK 2.0 or newer)");
        return;
    }
    XessVersion version{};
    if (impl->get_version(&version) == XessSuccess) {
        std::printf("XeSS: libxess.dll %u.%u.%u\n", version.major, version.minor, version.patch);
    }
    const char* auto_exposure = std::getenv("BB_XESS_AUTOEXPOSURE");
    impl->auto_exposure = !(auto_exposure && auto_exposure[0] == '0');
}

Xess::~Xess() {
    Shutdown();
}

void Xess::AppendInstanceExtensions(std::vector<const char*>& enabled) {
    if (!impl->eligible) {
        return;
    }
    u32 count{}, version{};
    const char* const* names{};
    if (impl->get_instance_extensions(&count, &names, &version) != XessSuccess) {
        return impl->Disable("the Vulkan driver lacks the instance extensions XeSS needs");
    }
    const auto [result, available] = vk::enumerateInstanceExtensionProperties();
    if (result != vk::Result::eSuccess) {
        return impl->Disable("instance extension query failed");
    }
    for (u32 i = 0; i < count; ++i) {
        const std::string_view name{names[i]};
        if (std::ranges::none_of(available, [&](const auto& e) {
                return name == e.extensionName.data();
            })) {
            std::printf("XeSS: missing Vulkan instance extension %s\n", names[i]);
            return impl->Disable("required Vulkan instance extensions are missing");
        }
        if (std::ranges::none_of(enabled, [&](const char* e) { return name == e; })) {
            enabled.push_back(names[i]); // owned by libxess.dll, which stays loaded
        }
    }
    impl->instance_ready = true;
}

void Xess::AppendDeviceExtensions(vk::Instance instance, vk::PhysicalDevice physical,
                                  std::vector<const char*>& enabled) {
    if (!impl->eligible || !impl->instance_ready) {
        return;
    }
    u32 count{};
    const char* const* names{};
    if (impl->get_device_extensions(instance, physical, &count, &names) != XessSuccess) {
        return impl->Disable("this GPU or driver lacks the Vulkan device extensions XeSS needs");
    }
    const auto [result, available] = physical.enumerateDeviceExtensionProperties();
    if (result != vk::Result::eSuccess) {
        return impl->Disable("device extension query failed");
    }
    for (u32 i = 0; i < count; ++i) {
        const std::string_view name{names[i]};
        if (std::ranges::none_of(available, [&](const auto& e) {
                return name == e.extensionName.data();
            })) {
            std::printf("XeSS: missing Vulkan device extension %s\n", names[i]);
            return impl->Disable("required Vulkan device extensions are missing");
        }
        if (std::ranges::none_of(enabled, [&](const char* e) { return name == e; })) {
            enabled.push_back(names[i]);
        }
    }
    impl->device_ready = true;
}

void Xess::PatchDeviceFeatures(vk::Instance instance, vk::PhysicalDevice physical,
                               void* features) {
    if (!impl->eligible || !impl->device_ready) {
        return;
    }
    void* chain = features;
    if (impl->get_device_features(instance, physical, &chain) != XessSuccess) {
        impl->device_ready = false;
        impl->Disable("this GPU or driver lacks the Vulkan features XeSS needs");
    }
}

void Xess::Initialize(vk::Instance instance, vk::PhysicalDevice physical, vk::Device device) {
    if (!impl->eligible || !impl->device_ready || impl->initialized) {
        return;
    }
    impl->initialized = true;
    impl->instance = instance;
    impl->physical = physical;
    impl->device = device;
    impl->available = true;
    std::printf("XeSS: ready\n");
}

bool Xess::Available() const {
    return impl->available;
}

std::string Xess::Problem() const {
    return impl->problem;
}

bool Xess::HasFeature(const FeatureDesc& desc) const {
    return impl->feature && *impl->feature == desc;
}

bool Xess::CreateFeature(const FeatureDesc& desc) {
    if (!impl->available) {
        return false;
    }
    impl->feature.reset();
    if (!impl->context) {
        const auto result = impl->create_context(impl->instance, impl->physical, impl->device,
                                                 &impl->context);
        if (result != XessSuccess || !impl->context) {
            std::printf("XeSS: context creation failed (%d)\n", result);
            impl->context = nullptr;
            return false;
        }
        impl->set_logging(impl->context, 2, XessLog);
        if (impl->is_optimal_driver(impl->context) == XessWarningOldDriver) {
            std::printf("XeSS: warning: this GPU driver is older than the one XeSS was tested "
                        "with; update it if the image looks wrong\n");
        }
    }
    XessVkInit init{};
    init.output_resolution = {desc.output_width, desc.output_height};
    init.quality = desc.quality;
    // Motion vectors at the render resolution with the scene depth (XeSS dilates them): the
    // inputs FSR 3 and DLSS get. Without an exposure texture XeSS measures the exposure itself.
    init.init_flags = (desc.hdr ? 0 : InitLdrInput) | (impl->auto_exposure ? InitAutoExposure : 0);
    const auto result = impl->init(impl->context, &init);
    if (result < 0) {
        std::printf("XeSS: initialization failed (%d)\n", result);
        return false;
    }
    impl->feature = desc;
    std::printf("XeSS: %ux%u -> %ux%u, quality %d%s\n", desc.input_width, desc.input_height,
                desc.output_width, desc.output_height, desc.quality,
                impl->auto_exposure ? ", auto exposure" : "");
    return true;
}

bool Xess::Evaluate(vk::CommandBuffer command, const Frame& frame) {
    if (!impl->available || !impl->feature || !impl->context) {
        return false;
    }
    const auto image = [](const Resource& r) {
        const vk::ImageSubresourceRange range{r.aspect, 0, 1, 0, 1};
        return XessVkImage{r.view, r.image, static_cast<VkImageSubresourceRange>(range),
                           static_cast<VkFormat>(r.format), r.width, r.height};
    };
    XessVkExecute params{};
    params.color = image(frame.color);
    params.velocity = image(frame.motion);
    params.depth = image(frame.depth);
    params.output = image(frame.output);
    params.jitter_x = frame.jitter_x;
    params.jitter_y = frame.jitter_y;
    params.exposure_scale_value = 1.0f;
    params.reset_history = frame.reset ? 1 : 0;
    params.input_width = impl->feature->input_width;
    params.input_height = impl->feature->input_height;
    const auto result = impl->execute(impl->context, command, &params);
    if (result < 0) {
        std::printf("XeSS: execute failed (%d)\n", result);
    }
    return result >= 0;
}

void Xess::ReleaseFeature() {
    impl->feature.reset();
}

void Xess::Shutdown() {
    impl->available = false;
    impl->DestroyContext();
    impl->initialized = false;
}

#else

struct Xess::Impl {};
Xess::Xess() = default;
Xess::~Xess() = default;
Xess* Xess::Get() {
    return nullptr;
}
void Xess::AppendInstanceExtensions(std::vector<const char*>&) {}
void Xess::AppendDeviceExtensions(vk::Instance, vk::PhysicalDevice, std::vector<const char*>&) {}
void Xess::PatchDeviceFeatures(vk::Instance, vk::PhysicalDevice, void*) {}
void Xess::Initialize(vk::Instance, vk::PhysicalDevice, vk::Device) {}
void Xess::Shutdown() {}
bool Xess::Available() const {
    return false;
}
std::string Xess::Problem() const {
    return {};
}
bool Xess::HasFeature(const FeatureDesc&) const {
    return false;
}
bool Xess::CreateFeature(const FeatureDesc&) {
    return false;
}
bool Xess::Evaluate(vk::CommandBuffer, const Frame&) {
    return false;
}
void Xess::ReleaseFeature() {}

#endif

} // namespace Vulkan
