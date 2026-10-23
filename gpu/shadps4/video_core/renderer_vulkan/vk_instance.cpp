// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <boost/container/static_vector.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include "common/assert.h"
#include "common/debug.h"
#include "common/types.h"
#include "imgui/renderer/imgui_core.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/renderer_vulkan/vk_xess.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_vram.h"
#include "video_core/renderer_vulkan/vk_platform.h"

#include <vk_mem_alloc.h>

namespace Vulkan {

bool g_device_fault_ext = false;
// bbport diagnosis: BB_GPU_CHECKPOINTS=1 (VK_NV_device_diagnostic_checkpoints): the queue whose
// last checkpoints a GPU crash report prints.
bool g_gpu_checkpoints = false;
VkQueue g_crash_queue = VK_NULL_HANDLE;

namespace {

std::vector<vk::PhysicalDevice> EnumeratePhysicalDevices(vk::UniqueInstance& instance) {
    auto [devices_result, devices] = instance->enumeratePhysicalDevices();
    ASSERT_MSG(devices_result == vk::Result::eSuccess, "Failed to enumerate physical devices: {}",
               vk::to_string(devices_result));
    return std::move(devices);
}

std::vector<std::string> GetSupportedExtensions(vk::PhysicalDevice physical) {
    const auto [extensions_result, extensions] = physical.enumerateDeviceExtensionProperties();
    if (extensions_result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "Could not query supported extensions: {}",
                  vk::to_string(extensions_result));
        return {};
    }
    std::vector<std::string> supported_extensions;
    supported_extensions.reserve(extensions.size());
    for (const auto& extension : extensions) {
        supported_extensions.emplace_back(extension.extensionName.data());
    }
    return supported_extensions;
}

vk::FormatProperties3 GetFormatProperties(vk::PhysicalDevice physical, vk::Format format) {
    vk::FormatProperties3 properties3{};
    vk::FormatProperties2 properties2 = {
        .pNext = &properties3,
    };
    physical.getFormatProperties2(format, &properties2);
    return properties3;
}

std::unordered_map<vk::Format, vk::FormatProperties3> GetFormatProperties(
    vk::PhysicalDevice physical) {
    std::unordered_map<vk::Format, vk::FormatProperties3> format_properties;
    for (const auto& format_info : LiverpoolToVK::SurfaceFormats()) {
        const auto format = format_info.vk_format;
        if (!format_properties.contains(format)) {
            format_properties.emplace(format, GetFormatProperties(physical, format));
        }
    }
    for (const auto& format_info : LiverpoolToVK::DepthFormats()) {
        const auto format = format_info.vk_format;
        if (!format_properties.contains(format)) {
            format_properties.emplace(format, GetFormatProperties(physical, format));
        }
    }
    // Other miscellaneous formats, e.g. for color buffers, swizzles, or compatibility
    static constexpr std::array misc_formats = {
        vk::Format::eA2R10G10B10UnormPack32,
        vk::Format::eB8G8R8A8Unorm,
        vk::Format::eB8G8R8A8Srgb,
        vk::Format::eD24UnormS8Uint,
    };
    for (const auto& format : misc_formats) {
        if (!format_properties.contains(format)) {
            format_properties.emplace(format, GetFormatProperties(physical, format));
        }
    }
    return format_properties;
}

std::string GetReadableVersion(u32 version) {
    return fmt::format("{}.{}.{}", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version),
                       VK_VERSION_PATCH(version));
}

} // Anonymous namespace

Instance::Instance(bool enable_validation, bool enable_crash_diagnostic)
    : instance{CreateInstance(Frontend::WindowSystemType::Headless, enable_validation,
                              enable_crash_diagnostic)},
      physical_devices{EnumeratePhysicalDevices(instance)} {}

Instance::Instance(Frontend::WindowSDL& window, s32 physical_device_index,
                   bool enable_validation /*= false*/, bool enable_crash_diagnostic /*= false*/)
    : Instance(window.GetWindowInfo(), physical_device_index, enable_validation, enable_crash_diagnostic) {}

Instance::Instance(s32 index, bool validation)
    : Instance(Frontend::WindowSystemInfo{}, index, validation, false) {}

Instance::Instance(const Frontend::WindowSystemInfo& window_info, s32 physical_device_index,
                   bool enable_validation, bool enable_crash_diagnostic)
    : instance{CreateInstance(window_info.type, enable_validation, enable_crash_diagnostic)},
      physical_devices{EnumeratePhysicalDevices(instance)} {
    if (enable_validation) {
        debug_callback = CreateDebugCallback(*instance);
    }
    if (AddressReportEnabled()) {
        address_report_callback = CreateAddressReportCallback(*instance);
    }
    const std::size_t num_physical_devices = static_cast<u16>(physical_devices.size());
    ASSERT_MSG(num_physical_devices > 0, "No physical devices found");
    LOG_INFO(Render_Vulkan, "Found {} physical devices", num_physical_devices);

    if (physical_device_index < 0) {
        std::vector<
            std::tuple<size_t, vk::PhysicalDeviceProperties2, vk::PhysicalDeviceMemoryProperties>>
            properties2{};
        for (auto const& physical : physical_devices) {
            properties2.emplace_back(properties2.size(), physical.getProperties2(),
                                     physical.getMemoryProperties());
        }
        std::sort(properties2.begin(), properties2.end(), [](const auto& left, const auto& right) {
            const vk::PhysicalDeviceProperties& left_prop = std::get<1>(left).properties;
            const vk::PhysicalDeviceProperties& right_prop = std::get<1>(right).properties;
            const bool left_supports_api = left_prop.apiVersion >= TargetVulkanApiVersion;
            const bool right_supports_api = right_prop.apiVersion >= TargetVulkanApiVersion;
            if (left_supports_api != right_supports_api) {
                return left_supports_api;
            }

            const bool left_is_discrete =
                left_prop.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
            const bool right_is_discrete =
                right_prop.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
            if (left_is_discrete != right_is_discrete) {
                return left_is_discrete;
            }

            // Software renderers advertise system memory as device local, so they
            // would win the memory comparison below against real hardware.
            const bool left_is_cpu = left_prop.deviceType == vk::PhysicalDeviceType::eCpu;
            const bool right_is_cpu = right_prop.deviceType == vk::PhysicalDeviceType::eCpu;
            if (left_is_cpu != right_is_cpu) {
                return right_is_cpu;
            }

            constexpr auto get_mem = [](const vk::PhysicalDeviceMemoryProperties& mem) -> size_t {
                size_t max = 0;
                for (u32 i = 0; i < mem.memoryHeapCount; i++) {
                    const vk::MemoryHeap& heap = mem.memoryHeaps[i];
                    if (heap.flags & vk::MemoryHeapFlagBits::eDeviceLocal && heap.size > max) {
                        max = heap.size;
                    }
                }
                return max;
            };
            size_t left_mem_size = get_mem(std::get<2>(left));
            size_t right_mem_size = get_mem(std::get<2>(right));
            return left_mem_size > right_mem_size;
        });
        physical_device = physical_devices[std::get<0>(properties2[0])];
    } else {
        ASSERT_MSG(physical_device_index < num_physical_devices,
                   "Invalid physical device index {} provided when only {} devices exist",
                   physical_device_index, num_physical_devices);

        physical_device = physical_devices[physical_device_index];
    }

    available_extensions = GetSupportedExtensions(physical_device);
    format_properties = GetFormatProperties(physical_device);
    properties = physical_device.getProperties();
    memory_properties = physical_device.getMemoryProperties();
    CollectDeviceParameters();
    ASSERT_MSG(properties.apiVersion >= TargetVulkanApiVersion,
               "Vulkan {}.{} is required, but only {}.{} is supported by device!",
               VK_VERSION_MAJOR(TargetVulkanApiVersion), VK_VERSION_MINOR(TargetVulkanApiVersion),
               VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion));

    CreateDevice();
    CollectPhysicalMemoryInfo();
    CollectImageFormatInfo();
    CollectToolingInfo();
}

Instance::~Instance() {
    ImGui::Core::Shutdown(GetDevice());
    vmaDestroyAllocator(allocator);
}

std::string Instance::GetDriverVersionName() {
    // Extracted from
    // https://github.com/SaschaWillems/vulkan.gpuinfo.org/blob/5dddea46ea1120b0df14eef8f15ff8e318e35462/functions.php#L308-L314
    const u32 version = properties.driverVersion;
    if (driver_id == vk::DriverId::eNvidiaProprietary) {
        const u32 major = (version >> 22) & 0x3ff;
        const u32 minor = (version >> 14) & 0x0ff;
        const u32 secondary = (version >> 6) & 0x0ff;
        const u32 tertiary = version & 0x003f;
        return fmt::format("{}.{}.{}.{}", major, minor, secondary, tertiary);
    }
    if (driver_id == vk::DriverId::eIntelProprietaryWindows) {
        const u32 major = version >> 14;
        const u32 minor = version & 0x3fff;
        return fmt::format("{}.{}", major, minor);
    }
    return GetReadableVersion(version);
}

bool Instance::CreateDevice() {
    const vk::StructureChain feature_chain = physical_device.getFeatures2<
        vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features,
        vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan13Features,
        vk::PhysicalDeviceRobustness2FeaturesEXT,
        vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT,
        vk::PhysicalDevicePrimitiveTopologyListRestartFeaturesEXT,
        vk::PhysicalDeviceShaderAtomicFloat2FeaturesEXT,
        vk::PhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR,
        vk::PhysicalDeviceImage2DViewOf3DFeaturesEXT, vk::PhysicalDeviceShaderClockFeaturesKHR,
        vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR,
        vk::PhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE>();
    features = feature_chain.get().features;

    const vk::StructureChain properties_chain = physical_device.getProperties2<
        vk::PhysicalDeviceProperties2, vk::PhysicalDeviceVulkan11Properties,
        vk::PhysicalDeviceVulkan12Properties, vk::PhysicalDeviceVulkan13Properties,
        vk::PhysicalDevicePushDescriptorPropertiesKHR>();
    vk11_props = properties_chain.get<vk::PhysicalDeviceVulkan11Properties>();
    vk12_props = properties_chain.get<vk::PhysicalDeviceVulkan12Properties>();
    vk13_props = properties_chain.get<vk::PhysicalDeviceVulkan13Properties>();
    push_descriptor_props = properties_chain.get<vk::PhysicalDevicePushDescriptorPropertiesKHR>();
    LOG_INFO(Render_Vulkan, "Physical device subgroup size {}", vk11_props.subgroupSize);

    if (available_extensions.empty()) {
        LOG_CRITICAL(Render_Vulkan, "No extensions supported by device.");
        return false;
    }

    boost::container::static_vector<const char*, 64> enabled_extensions;
    const auto add_extension = [&](std::string_view extension) -> bool {
        const auto result =
            std::find_if(available_extensions.begin(), available_extensions.end(),
                         [&](const std::string& name) { return name == extension; });

        if (result != available_extensions.end()) {
            LOG_INFO(Render_Vulkan, "Enabling extension: {}", extension);
            enabled_extensions.push_back(extension.data());
            return true;
        }

        LOG_WARNING(Render_Vulkan, "Extension {} unavailable.", extension);
        return false;
    };

    // Required
    ASSERT_MSG(add_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME),
               "Required Vulkan extension unavailable: {}", VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    ASSERT_MSG(add_extension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME),
               "Required Vulkan extension unavailable: {}", VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    ASSERT_MSG(add_extension(VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME),
               "Required Vulkan extension unavailable: {}",
               VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    ASSERT_MSG(add_extension(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME),
               "Required Vulkan extension unavailable: {}", VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);

    const auto robustness2_features = feature_chain.get<vk::PhysicalDeviceRobustness2FeaturesEXT>();
    ASSERT_MSG(robustness2_features.robustBufferAccess2,
               "Required Vulkan feature unavailable: robustBufferAccess2");
    ASSERT_MSG(robustness2_features.robustImageAccess2,
               "Required Vulkan feature unavailable: robustImageAccess2");
    ASSERT_MSG(robustness2_features.nullDescriptor,
               "Required Vulkan feature unavailable: nullDescriptor");

    // Optional
    maintenance_5 = add_extension(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    maintenance_8 = add_extension(VK_KHR_MAINTENANCE_8_EXTENSION_NAME);
    attachment_feedback_loop = add_extension(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME);
    if (attachment_feedback_loop) {
        attachment_feedback_loop =
            add_extension(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME);
        if (!attachment_feedback_loop) {
            // We want both extensions so remove the first if the second isn't available
            enabled_extensions.pop_back();
        }
    }
    depth_range_unrestricted = add_extension(VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME);
    dynamic_state_3 = add_extension(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    if (dynamic_state_3) {
        dynamic_state_3_features =
            feature_chain.get<vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT>();
        LOG_INFO(Render_Vulkan, "- extendedDynamicState3ColorWriteMask: {}",
                 dynamic_state_3_features.extendedDynamicState3ColorWriteMask);
    }
    custom_border_color = add_extension(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    depth_clip_control = add_extension(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
    depth_clip_enable = add_extension(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
    // bbport: Intel's Windows driver draws nothing with it (HD 630: black screen, issue #56; the
    // God of War III port saw the same). BB_VERTEX_INPUT_DYNAMIC=1/0 forces it on or off.
    const char* vertex_input_env = std::getenv("BB_VERTEX_INPUT_DYNAMIC");
    const bool vertex_input_wanted =
        vertex_input_env ? vertex_input_env[0] == '1' : GetVendorID() != 0x8086;
    vertex_input_dynamic_state =
        vertex_input_wanted && add_extension(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    list_restart = add_extension(VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME);
    if (list_restart) {
        list_restart_features =
            feature_chain.get<vk::PhysicalDevicePrimitiveTopologyListRestartFeaturesEXT>();
        LOG_INFO(Render_Vulkan, "- primitiveTopologyListRestart: {}",
                 list_restart_features.primitiveTopologyListRestart);
        LOG_INFO(Render_Vulkan, "- primitiveTopologyPatchListRestart: {}",
                 list_restart_features.primitiveTopologyPatchListRestart);
    }
    amd_shader_explicit_vertex_parameter =
        add_extension(VK_AMD_SHADER_EXPLICIT_VERTEX_PARAMETER_EXTENSION_NAME);
    if (!amd_shader_explicit_vertex_parameter) {
        fragment_shader_barycentric =
            add_extension(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
    }
    provoking_vertex = add_extension(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);
    shader_stencil_export = add_extension(VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
    image_load_store_lod = add_extension(VK_AMD_SHADER_IMAGE_LOAD_STORE_LOD_EXTENSION_NAME);
    amd_gcn_shader = add_extension(VK_AMD_GCN_SHADER_EXTENSION_NAME);
    amd_shader_trinary_minmax = add_extension(VK_AMD_SHADER_TRINARY_MINMAX_EXTENSION_NAME);
    nv_framebuffer_mixed_samples = add_extension(VK_NV_FRAMEBUFFER_MIXED_SAMPLES_EXTENSION_NAME);
    amd_mixed_attachment_samples = add_extension(VK_AMD_MIXED_ATTACHMENT_SAMPLES_EXTENSION_NAME);
    shader_atomic_float = add_extension(VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME);
    shader_atomic_float2 = add_extension(VK_EXT_SHADER_ATOMIC_FLOAT_2_EXTENSION_NAME);
    if (shader_atomic_float2) {
        shader_atomic_float2_features =
            feature_chain.get<vk::PhysicalDeviceShaderAtomicFloat2FeaturesEXT>();
        LOG_INFO(Render_Vulkan, "- shaderBufferFloat32AtomicMinMax: {}",
                 shader_atomic_float2_features.shaderBufferFloat32AtomicMinMax);
        LOG_INFO(Render_Vulkan, "- shaderImageFloat32AtomicMinMax: {}",
                 shader_atomic_float2_features.shaderImageFloat32AtomicMinMax);
    }
    workgroup_memory_explicit_layout =
        add_extension(VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME);
    if (workgroup_memory_explicit_layout) {
        workgroup_memory_explicit_layout_features =
            feature_chain.get<vk::PhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR>();
        LOG_INFO(Render_Vulkan, "- workgroupMemoryExplicitLayout: {}",
                 workgroup_memory_explicit_layout_features.workgroupMemoryExplicitLayout);
        LOG_INFO(Render_Vulkan, "- workgroupMemoryExplicitLayoutScalarBlockLayout: {}",
                 workgroup_memory_explicit_layout_features
                     .workgroupMemoryExplicitLayoutScalarBlockLayout);
        LOG_INFO(
            Render_Vulkan, "- workgroupMemoryExplicitLayout16BitAccess: {}",
            workgroup_memory_explicit_layout_features.workgroupMemoryExplicitLayout16BitAccess);
    }
    image_2d_view_of_3d = add_extension(VK_EXT_IMAGE_2D_VIEW_OF_3D_EXTENSION_NAME);
    if (image_2d_view_of_3d) {
        image_2d_view_of_3d_features =
            feature_chain.get<vk::PhysicalDeviceImage2DViewOf3DFeaturesEXT>();
        LOG_INFO(Render_Vulkan, "- image2DViewOf3D: {}",
                 image_2d_view_of_3d_features.image2DViewOf3D);
        LOG_INFO(Render_Vulkan, "- sampler2DViewOf3D: {}",
                 image_2d_view_of_3d_features.sampler2DViewOf3D);
    }
    image_view_min_lod = add_extension(VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME);
    supports_memory_budget = add_extension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    // bbport: FSR 4 v07 INT8 (vk_temporal_upscaler): quad derivatives in compute shaders.
    compute_shader_derivatives = add_extension(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
    // bbport: FSR 4.1.1 passes (dot2 of halves accumulated in float, as vkd3d-proton translates them).
    mixed_float_dot_product =
        feature_chain.get<vk::PhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE>()
            .shaderMixedFloatDotProductFloat16AccFloat32 &&
        add_extension(VK_VALVE_SHADER_MIXED_FLOAT_DOT_PRODUCT_EXTENSION_NAME);
    if (compute_shader_derivatives) {
        compute_shader_derivatives_features =
            feature_chain.get<vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR>();
    }
    shader_clock = add_extension(VK_KHR_SHADER_CLOCK_EXTENSION_NAME);
    if (shader_clock) {
        shader_clock_features = feature_chain.get<vk::PhysicalDeviceShaderClockFeaturesKHR>();
        LOG_INFO(Render_Vulkan, "- shaderSubgroupClock: {}",
                 shader_clock_features.shaderSubgroupClock);
    }
    const bool calibrated_timestamps =
        TRACY_GPU_ENABLED ? add_extension(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) : false;
    // bbport: what the GPU touched when the device is lost (reported by vk_semaphore.cpp).
    g_device_fault_ext = add_extension(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    if (const char* env = std::getenv("BB_GPU_CHECKPOINTS"); env && env[0] == '1') {
        g_gpu_checkpoints = add_extension(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
    }
    const bool address_report =
        AddressReportEnabled() && add_extension(VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME);

    const auto family_properties = physical_device.getQueueFamilyProperties();
    if (family_properties.empty()) {
        LOG_CRITICAL(Render_Vulkan, "Physical device reported no queues.");
        return false;
    }

    bool graphics_queue_found = false;
    for (std::size_t i = 0; i < family_properties.size(); i++) {
        const u32 index = static_cast<u32>(i);
        if (family_properties[i].queueFlags & vk::QueueFlagBits::eGraphics) {
            queue_family_index = index;
            graphics_queue_found = true;
        }
    }

    if (!graphics_queue_found) {
        LOG_CRITICAL(Render_Vulkan, "Unable to find graphics and/or present queues.");
        return false;
    }

    // bbport: a side queue (BufferCache reads GPU data back there, past the work queued for
    // graphics): a transfer-only family, a copy engine that runs beside graphics work; else a
    // second queue of the graphics family.
    static constexpr std::array queue_priorities = {1.0f, 1.0f};
    side_queue_family_index = queue_family_index;
    for (u32 i = 0; i < family_properties.size(); ++i) {
        const auto flags =
            family_properties[i].queueFlags & ~vk::QueueFlags{vk::QueueFlagBits::eSparseBinding};
        if (flags == vk::QueueFlagBits::eTransfer && family_properties[i].queueCount > 0) {
            side_queue_family_index = i;
            break;
        }
    }
    // bbport: RADV (Steam Deck) has no transfer-only family and one graphics queue, so there was
    // no side queue. An async compute family copies as well. Used for the read backs of
    // BB_GPU_MERGE=0 only (BufferCache::DownloadOnSideQueue); the merging default keeps the
    // device as it was.
    const char* merge_env = std::getenv("BB_GPU_MERGE");
    const bool accurate_gpu_memory = merge_env && merge_env[0] == '0';
    for (u32 i = 0; accurate_gpu_memory && side_queue_family_index == queue_family_index &&
                    i < family_properties.size();
         ++i) {
        const auto flags = family_properties[i].queueFlags;
        if (!(flags & vk::QueueFlagBits::eGraphics) && (flags & vk::QueueFlagBits::eCompute) &&
            (flags & vk::QueueFlagBits::eTransfer) && family_properties[i].queueCount > 0) {
            side_queue_family_index = i;
        }
    }
    const bool side_family = side_queue_family_index != queue_family_index;
    sparse_queue_family_index = queue_family_index;
    if (!(family_properties[sparse_queue_family_index].queueFlags &
          vk::QueueFlagBits::eSparseBinding)) {
        bool found = false;
        for (u32 i = 0; i < family_properties.size(); ++i) {
            if (family_properties[i].queueCount &&
                (family_properties[i].queueFlags & vk::QueueFlagBits::eSparseBinding)) {
                sparse_queue_family_index = i;
                found = true;
                break;
            }
        }
        if (!found) {
            LOG_CRITICAL(Render_Vulkan, "No queue supports sparse binding.");
            return false;
        }
    }
    const u32 graphics_queues =
        side_family ? 1u : std::min<u32>(2u, family_properties[queue_family_index].queueCount);
    std::vector<vk::DeviceQueueCreateInfo> queue_infos = {
        vk::DeviceQueueCreateInfo{
            .queueFamilyIndex = queue_family_index,
            .queueCount = graphics_queues,
            .pQueuePriorities = queue_priorities.data(),
        },
    };
    if (side_family) {
        queue_infos.push_back(vk::DeviceQueueCreateInfo{
            .queueFamilyIndex = side_queue_family_index,
            .queueCount = 1,
            .pQueuePriorities = queue_priorities.data(),
        });
    }
    if (sparse_queue_family_index != queue_family_index &&
        sparse_queue_family_index != side_queue_family_index) {
        queue_infos.push_back(vk::DeviceQueueCreateInfo{
            .queueFamilyIndex = sparse_queue_family_index,
            .queueCount = 1,
            .pQueuePriorities = queue_priorities.data(),
        });
    }

    const auto vk11_features = feature_chain.get<vk::PhysicalDeviceVulkan11Features>();
    vk12_features = feature_chain.get<vk::PhysicalDeviceVulkan12Features>();
    vk13_features = feature_chain.get<vk::PhysicalDeviceVulkan13Features>();
    // bbport: DLSS (optional bridge DLL) needs its own device extensions on NVIDIA GPUs.
    std::vector<const char*> dlss_extensions;
    if (Dlss* dlss = Dlss::Get()) {
        dlss->AppendDeviceExtensions(*instance, physical_device, dlss_extensions);
        for (const char* name : dlss_extensions) {
            if (std::none_of(enabled_extensions.begin(), enabled_extensions.end(),
                             [&](const char* e) { return std::string_view{e} == name; }) &&
                enabled_extensions.size() < enabled_extensions.capacity()) {
                enabled_extensions.push_back(name);
            }
        }
    }
    // bbport: Intel XeSS (optional libxess.dll) asks for its own device extensions and features.
    if (Xess* xess = Xess::Get()) {
        std::vector<const char*> xess_extensions;
        xess->AppendDeviceExtensions(*instance, physical_device, xess_extensions);
        for (const char* name : xess_extensions) {
            if (std::none_of(enabled_extensions.begin(), enabled_extensions.end(),
                             [&](const char* e) { return std::string_view{e} == name; }) &&
                enabled_extensions.size() < enabled_extensions.capacity()) {
                enabled_extensions.push_back(name);
            }
        }
    }
    vk::StructureChain device_chain = {
        vk::DeviceCreateInfo{
            .queueCreateInfoCount = static_cast<u32>(queue_infos.size()),
            .pQueueCreateInfos = queue_infos.data(),
            .enabledExtensionCount = static_cast<u32>(enabled_extensions.size()),
            .ppEnabledExtensionNames = enabled_extensions.data(),
        },
        vk::PhysicalDeviceFeatures2{
            .features{
                .robustBufferAccess = features.robustBufferAccess,
                .imageCubeArray = features.imageCubeArray,
                .independentBlend = features.independentBlend,
                .geometryShader = features.geometryShader,
                .tessellationShader = features.tessellationShader,
                .sampleRateShading = features.sampleRateShading,
                .dualSrcBlend = features.dualSrcBlend,
                .logicOp = features.logicOp,
                .multiDrawIndirect = features.multiDrawIndirect,
                .depthClamp = features.depthClamp,
                .depthBiasClamp = features.depthBiasClamp,
                .fillModeNonSolid = features.fillModeNonSolid,
                .depthBounds = features.depthBounds,
                .wideLines = features.wideLines,
                .multiViewport = features.multiViewport,
                .samplerAnisotropy = features.samplerAnisotropy,
                .vertexPipelineStoresAndAtomics = features.vertexPipelineStoresAndAtomics,
                .fragmentStoresAndAtomics = features.fragmentStoresAndAtomics,
                .shaderImageGatherExtended = features.shaderImageGatherExtended,
                .shaderStorageImageExtendedFormats = features.shaderStorageImageExtendedFormats,
                .shaderStorageImageMultisample = features.shaderStorageImageMultisample,
                // bbport: required by the FSR 3 accumulate shaders (vk_temporal_upscaler).
                .shaderStorageImageWriteWithoutFormat =
                    features.shaderStorageImageWriteWithoutFormat,
                .shaderClipDistance = features.shaderClipDistance,
                .shaderFloat64 = features.shaderFloat64,
                .shaderInt64 = features.shaderInt64,
                .shaderInt16 = features.shaderInt16,
                .sparseBinding = features.sparseBinding,
                .sparseResidencyBuffer = features.sparseResidencyBuffer,
            },
        },
        vk::PhysicalDeviceVulkan11Features{
            .storageBuffer16BitAccess = vk11_features.storageBuffer16BitAccess,
            .shaderDrawParameters = vk11_features.shaderDrawParameters,
        },
        vk::PhysicalDeviceVulkan12Features{
            .samplerMirrorClampToEdge = vk12_features.samplerMirrorClampToEdge,
            .drawIndirectCount = vk12_features.drawIndirectCount,
            .storageBuffer8BitAccess = vk12_features.storageBuffer8BitAccess,
            .shaderBufferInt64Atomics = vk12_features.shaderBufferInt64Atomics,
            .shaderSharedInt64Atomics = vk12_features.shaderSharedInt64Atomics,
            .shaderFloat16 = vk12_features.shaderFloat16,
            .shaderInt8 = vk12_features.shaderInt8,
            .scalarBlockLayout = vk12_features.scalarBlockLayout,
            .uniformBufferStandardLayout = vk12_features.uniformBufferStandardLayout,
            .separateDepthStencilLayouts = vk12_features.separateDepthStencilLayouts,
            .hostQueryReset = vk12_features.hostQueryReset,
            .timelineSemaphore = vk12_features.timelineSemaphore,
            .bufferDeviceAddress = vk12_features.bufferDeviceAddress,
            .shaderOutputLayer = vk12_features.shaderOutputLayer,
        },
        vk::PhysicalDeviceVulkan13Features{
            .robustImageAccess = vk13_features.robustImageAccess,
            .shaderDemoteToHelperInvocation = vk13_features.shaderDemoteToHelperInvocation,
            .subgroupSizeControl = vk13_features.subgroupSizeControl,
            .synchronization2 = vk13_features.synchronization2,
            .dynamicRendering = vk13_features.dynamicRendering,
            .shaderIntegerDotProduct = vk13_features.shaderIntegerDotProduct,
            .maintenance4 = vk13_features.maintenance4,
        },
        // Extensions
        vk::PhysicalDeviceCustomBorderColorFeaturesEXT{
            .customBorderColors = true,
            .customBorderColorWithoutFormat = true,
        },
        vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT{
            .extendedDynamicState3ColorWriteMask =
                dynamic_state_3_features.extendedDynamicState3ColorWriteMask,
        },
        vk::PhysicalDeviceDepthClipControlFeaturesEXT{
            .depthClipControl = true,
        },
        vk::PhysicalDeviceDepthClipEnableFeaturesEXT{
            .depthClipEnable = true,
        },
        vk::PhysicalDeviceRobustness2FeaturesEXT{
            .robustBufferAccess2 = true,
            .robustImageAccess2 = true,
            .nullDescriptor = true,
        },
        vk::PhysicalDeviceVertexInputDynamicStateFeaturesEXT{
            .vertexInputDynamicState = true,
        },
        vk::PhysicalDevicePrimitiveTopologyListRestartFeaturesEXT{
            .primitiveTopologyListRestart = list_restart_features.primitiveTopologyListRestart,
            .primitiveTopologyPatchListRestart =
                list_restart_features.primitiveTopologyPatchListRestart,
        },
        vk::PhysicalDeviceFragmentShaderBarycentricFeaturesKHR{
            .fragmentShaderBarycentric = true,
        },
        vk::PhysicalDeviceProvokingVertexFeaturesEXT{
            .provokingVertexLast = true,
        },
        vk::PhysicalDeviceVertexAttributeDivisorFeatures{
            .vertexAttributeInstanceRateDivisor = true,
        },
        vk::PhysicalDeviceMaintenance5FeaturesKHR{
            .maintenance5 = true,
        },
        vk::PhysicalDeviceMaintenance8FeaturesKHR{
            .maintenance8 = true,
        },
        vk::PhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT{
            .attachmentFeedbackLoopLayout = true,
        },
        vk::PhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT{
            .attachmentFeedbackLoopDynamicState = true,
        },
        vk::PhysicalDeviceShaderAtomicFloat2FeaturesEXT{
            .shaderBufferFloat32AtomicMinMax =
                shader_atomic_float2_features.shaderBufferFloat32AtomicMinMax,
            .shaderImageFloat32AtomicMinMax =
                shader_atomic_float2_features.shaderImageFloat32AtomicMinMax,
        },
        vk::PhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR{
            .workgroupMemoryExplicitLayout =
                workgroup_memory_explicit_layout_features.workgroupMemoryExplicitLayout,
            .workgroupMemoryExplicitLayoutScalarBlockLayout =
                workgroup_memory_explicit_layout_features
                    .workgroupMemoryExplicitLayoutScalarBlockLayout,
            .workgroupMemoryExplicitLayout16BitAccess =
                workgroup_memory_explicit_layout_features.workgroupMemoryExplicitLayout16BitAccess,
        },
        vk::PhysicalDeviceImage2DViewOf3DFeaturesEXT{
            .image2DViewOf3D = image_2d_view_of_3d_features.image2DViewOf3D,
            .sampler2DViewOf3D = image_2d_view_of_3d_features.sampler2DViewOf3D,
        },
        vk::PhysicalDeviceImageViewMinLodFeaturesEXT{
            .minLod = true,
        },
        vk::PhysicalDeviceShaderClockFeaturesKHR{
            .shaderSubgroupClock = shader_clock_features.shaderSubgroupClock,
        },
        vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR{
            .computeDerivativeGroupQuads =
                compute_shader_derivatives_features.computeDerivativeGroupQuads,
            .computeDerivativeGroupLinear =
                compute_shader_derivatives_features.computeDerivativeGroupLinear,
        },
        vk::PhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE{
            .shaderMixedFloatDotProductFloat16AccFloat32 = true,
        },
        vk::PhysicalDeviceFaultFeaturesEXT{
            .deviceFault = true,
        },
        vk::PhysicalDeviceAddressBindingReportFeaturesEXT{
            .reportAddressBinding = true,
        },
    };
    if (!g_device_fault_ext) {
        device_chain.unlink<vk::PhysicalDeviceFaultFeaturesEXT>();
    }
    if (!address_report) {
        device_chain.unlink<vk::PhysicalDeviceAddressBindingReportFeaturesEXT>();
    }

    if (!custom_border_color) {
        device_chain.unlink<vk::PhysicalDeviceCustomBorderColorFeaturesEXT>();
    }
    if (!dynamic_state_3) {
        device_chain.unlink<vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT>();
    }
    if (!depth_clip_control) {
        device_chain.unlink<vk::PhysicalDeviceDepthClipControlFeaturesEXT>();
    }
    if (!depth_clip_enable) {
        device_chain.unlink<vk::PhysicalDeviceDepthClipEnableFeaturesEXT>();
    }
    if (!vertex_input_dynamic_state) {
        device_chain.unlink<vk::PhysicalDeviceVertexInputDynamicStateFeaturesEXT>();
    }
    if (!list_restart) {
        device_chain.unlink<vk::PhysicalDevicePrimitiveTopologyListRestartFeaturesEXT>();
    }
    if (!fragment_shader_barycentric) {
        device_chain.unlink<vk::PhysicalDeviceFragmentShaderBarycentricFeaturesKHR>();
    }
    if (!provoking_vertex) {
        device_chain.unlink<vk::PhysicalDeviceProvokingVertexFeaturesEXT>();
    }
    if (!maintenance_5) {
        device_chain.unlink<vk::PhysicalDeviceMaintenance5FeaturesKHR>();
    }
    if (!maintenance_8) {
        device_chain.unlink<vk::PhysicalDeviceMaintenance8FeaturesKHR>();
    }
    if (!attachment_feedback_loop) {
        device_chain.unlink<vk::PhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT>();
        device_chain.unlink<vk::PhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT>();
    }
    if (!shader_atomic_float2) {
        device_chain.unlink<vk::PhysicalDeviceShaderAtomicFloat2FeaturesEXT>();
    }
    if (!workgroup_memory_explicit_layout) {
        device_chain.unlink<vk::PhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR>();
    }
    if (!image_2d_view_of_3d) {
        device_chain.unlink<vk::PhysicalDeviceImage2DViewOf3DFeaturesEXT>();
    }
    if (!image_view_min_lod) {
        device_chain.unlink<vk::PhysicalDeviceImageViewMinLodFeaturesEXT>();
    }
    if (!shader_clock) {
        device_chain.unlink<vk::PhysicalDeviceShaderClockFeaturesKHR>();
    }
    if (!compute_shader_derivatives) {
        device_chain.unlink<vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR>();
    }
    if (!mixed_float_dot_product) {
        device_chain.unlink<vk::PhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE>();
    }

    // bbport: XeSS enables its features in the chain (and may append structures to its end).
    if (Xess* xess = Xess::Get()) {
        xess->PatchDeviceFeatures(*instance, physical_device,
                                  &device_chain.get<vk::PhysicalDeviceFeatures2>());
    }
    auto [device_result, dev] = physical_device.createDeviceUnique(device_chain.get());
    if (device_result != vk::Result::eSuccess) {
        LOG_CRITICAL(Render_Vulkan, "Failed to create device: {}", vk::to_string(device_result));
        return false;
    }
    device = std::move(dev);

    VULKAN_HPP_DEFAULT_DISPATCHER.init(*device);
    if (Dlss* dlss = Dlss::Get()) {
        dlss->Initialize(*instance, physical_device, *device);
    }
    if (Xess* xess = Xess::Get()) {
        xess->Initialize(*instance, physical_device, *device);
    }

    graphics_queue = device->getQueue(queue_family_index, 0);
    sparse_queue = device->getQueue(sparse_queue_family_index, 0);
    present_queue = device->getQueue(queue_family_index, 0);
    g_crash_queue = graphics_queue;
    if (side_queue_family_index != queue_family_index) {
        side_queue = device->getQueue(side_queue_family_index, 0);
    } else if (physical_device.getQueueFamilyProperties()[queue_family_index].queueCount > 1) {
        side_queue = device->getQueue(queue_family_index, 1);
    }

    if (calibrated_timestamps) {
        const auto [time_domains_result, time_domains] =
            physical_device.getCalibrateableTimeDomainsEXT();
        if (time_domains_result == vk::Result::eSuccess) {
#if _WIN64
            const bool has_host_time_domain =
                std::find(time_domains.cbegin(), time_domains.cend(),
                          vk::TimeDomainEXT::eQueryPerformanceCounter) != time_domains.cend();
#elif __linux__
            const bool has_host_time_domain =
                std::find(time_domains.cbegin(), time_domains.cend(),
                          vk::TimeDomainEXT::eClockMonotonicRaw) != time_domains.cend();
#else
            // Tracy limitation means only Windows and Linux can use host time domain.
            // https://github.com/shadps4-emu/tracy/blob/c6d779d78508514102fbe1b8eb28bda10d95bb2a/public/tracy/TracyVulkan.hpp#L384-L389
            const bool has_host_time_domain = false;
#endif
            if (has_host_time_domain) {
                static constexpr std::string_view context_name{"vk_rasterizer"};
                profiler_context = TracyVkContextHostCalibrated(
                    *instance, physical_device, *device,
                    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
                    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr);
                TracyVkContextName(profiler_context, context_name.data(), context_name.size());
            }
        } else {
            LOG_WARNING(Render_Vulkan, "Could not query calibrated time domains for profiling: {}",
                        vk::to_string(time_domains_result));
        }
    }

    CreateAllocator();
    static const Instance* vram_instance = nullptr;
    vram_instance = this;
    Vram::allows = [](u64 bytes) { return vram_instance->VramAllows(bytes); };
    return true;
}

void Instance::CreateAllocator() {
    const VmaVulkanFunctions functions = {
        .vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr,
    };

    // bbport: VMA takes device memory in 256 MiB blocks on big heaps: on a card of 6 GB or less
    // that left ~180 MiB of slack with 1.3 GB allocated. 64 MiB blocks (dedicated allocations
    // for anything big) cost nothing noticeable.
    u64 local_heap = ~u64{0};
    for (u32 i = 0; i < memory_properties.memoryHeapCount; ++i) {
        if (memory_properties.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
            local_heap = std::min<u64>(local_heap, memory_properties.memoryHeaps[i].size);
        }
    }
    if (const u64 test_limit = Vram::TestLimit()) {
        local_heap = std::min(local_heap, test_limit);
    }
    const bool small_card = !IsIntegrated() && !Vram::Legacy() && local_heap <= 6_GB;
    const VmaAllocatorCreateInfo allocator_info = {
        // bbport: with VK_EXT_memory_budget VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT (images,
        // buffers) compares with the driver's budget, which counts the buffer arenas and the
        // other processes; without it VMA only knew its own blocks.
        .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT |
                 (supports_memory_budget ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0u),
        .physicalDevice = physical_device,
        .device = *device,
        .preferredLargeHeapBlockSize = small_card ? 64_MB : 0,
        .pVulkanFunctions = &functions,
        .instance = *instance,
        .vulkanApiVersion = TargetVulkanApiVersion,
    };

    const VkResult result = vmaCreateAllocator(&allocator_info, &allocator);
    if (result != VK_SUCCESS) {
        UNREACHABLE_MSG("Failed to initialize VMA with error {}",
                        vk::to_string(vk::Result{result}));
    }
}

void Instance::CollectDeviceParameters() {
    const vk::StructureChain property_chain =
        physical_device
            .getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDriverProperties>();
    const vk::PhysicalDeviceDriverProperties driver =
        property_chain.get<vk::PhysicalDeviceDriverProperties>();

    driver_id = driver.driverID;
    vendor_name = driver.driverName.data();

    const std::string model_name{GetModelName()};
    const std::string driver_version = GetDriverVersionName();
    const std::string driver_name = fmt::format("{} {}", vendor_name, driver_version);
    const std::string api_version = GetReadableVersion(properties.apiVersion);
    const std::string extensions = fmt::format("{}", fmt::join(available_extensions, ", "));

    LOG_INFO(Render_Vulkan, "GPU_Vendor: {}", vendor_name);
    LOG_INFO(Render_Vulkan, "GPU_Model: {}", model_name);
    LOG_INFO(Render_Vulkan, "GPU_Integrated: {}", IsIntegrated() ? "Yes" : "No");
    LOG_INFO(Render_Vulkan, "GPU_Vulkan_Driver: {}", driver_name);
    LOG_INFO(Render_Vulkan, "GPU_Vulkan_Version: {}", api_version);
    LOG_INFO(Render_Vulkan, "GPU_Vulkan_Extensions: {}", extensions);
}

void Instance::CollectPhysicalMemoryInfo() {
    vk::PhysicalDeviceMemoryBudgetPropertiesEXT budget{};
    vk::PhysicalDeviceMemoryProperties2 props = {
        .pNext = supports_memory_budget ? &budget : nullptr,
    };
    physical_device.getMemoryProperties2(&props);
    const auto& memory_props = props.memoryProperties;
    const size_t num_props = memory_props.memoryHeapCount;
    total_memory_budget = 0;
    u64 device_initial_usage = 0;
    u64 local_memory = 0;
    for (size_t i = 0; i < num_props; ++i) {
        const bool is_device_local =
            (memory_props.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) !=
            vk::MemoryHeapFlags{};
        if (!IsIntegrated() && !is_device_local) {
            // Ignore non-device local memory on discrete GPUs.
            continue;
        }
        valid_heaps.push_back(i);
        if (is_device_local) {
            local_memory += memory_props.memoryHeaps[i].size;
        }
        if (supports_memory_budget) {
            device_initial_usage += budget.heapUsage[i];
            total_memory_budget += budget.heapBudget[i];
            continue;
        }
        // If memory budget is not supported, use the size of the heap as the budget.
        total_memory_budget += memory_props.memoryHeaps[i].size;
    }
    Vram::device_memory.store(Vram::TestLimit() ? Vram::TestLimit() : total_memory_budget,
                              std::memory_order_relaxed);
    if (!IsIntegrated()) {
        if (const u64 test_limit = Vram::TestLimit()) {
            // bbport: BB_TEST_VRAM_MB: a card of that size; the driver's budget is ~90% of it.
            total_memory_budget = test_limit / 10 * 9;
            LOG_INFO(Render_Vulkan, "Video memory simulated: {} MiB ({}), budget {} MiB",
                     test_limit >> 20, Vram::TestHard() ? "hard limit" : "soft", total_memory_budget >> 20);
        }
        // We reserve some memory for the system.
        const u64 system_memory = std::min<u64>(total_memory_budget / 8, 1_GB);
        total_memory_budget -= system_memory;
        return;
    }
    // Leave at least 8 GB for the system on integrated GPUs.
    const s64 available_memory = static_cast<s64>(total_memory_budget - device_initial_usage);
    total_memory_budget =
        static_cast<u64>(std::max<s64>(available_memory - 8_GB, static_cast<s64>(local_memory)));
}

void Instance::CollectImageFormatInfo() {
    // Check for block texel view support using basic image format info.
    const vk::PhysicalDeviceImageFormatInfo2 block_texel_view_info{
        .format = vk::Format::eBc1RgbaUnormBlock,
        .type = vk::ImageType::e2D,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eSampled,
        .flags = vk::ImageCreateFlagBits::eBlockTexelViewCompatible,
    };
    const auto block_texel_view_props =
        physical_device.getImageFormatProperties2(block_texel_view_info);
    supports_block_texel_view = block_texel_view_props.result == vk::Result::eSuccess;
    LOG_INFO(Render_Vulkan, "Block Texel View support: {}",
             supports_block_texel_view ? "Yes" : "No");

    // Check and log format support details.
    for (const auto& format : LiverpoolToVK::SurfaceFormats()) {
        if (!IsFormatSupported(format.vk_format, format.flags)) {
            LOG_WARNING(Render_Vulkan,
                        "Surface format data_format={}, number_format={} is not fully supported "
                        "(vk_format={}, missing features={})",
                        static_cast<u32>(format.data_format),
                        static_cast<u32>(format.number_format), vk::to_string(format.vk_format),
                        vk::to_string(format.flags & ~GetFormatFeatureFlags(format.vk_format)));
        }
    }
    for (const auto& format : LiverpoolToVK::DepthFormats()) {
        if (!IsFormatSupported(format.vk_format, format.flags)) {
            LOG_WARNING(Render_Vulkan,
                        "Depth format z_format={}, stencil_format={} is not fully supported "
                        "(vk_format={}, missing features={})",
                        static_cast<u32>(format.z_format), static_cast<u32>(format.stencil_format),
                        vk::to_string(format.vk_format),
                        vk::to_string(format.flags & ~GetFormatFeatureFlags(format.vk_format)));
        }
    }
}

void Instance::CollectToolingInfo() const {
    if (driver_id == vk::DriverId::eAmdProprietary ||
        driver_id == vk::DriverId::eIntelProprietaryWindows) {
        // AMD: Causes issues with Reshade.
        // Intel: Causes crash on start.
        return;
    }
    const auto [tools_result, tools] = physical_device.getToolProperties();
    if (tools_result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "Could not get Vulkan tool properties: {}",
                  vk::to_string(tools_result));
        return;
    }
    for (const vk::PhysicalDeviceToolProperties& tool : tools) {
        const std::string_view name = tool.name;
        LOG_INFO(Render_Vulkan, "Attached debugging tool: {}", name);
    }
}

u64 Instance::GetDeviceMemoryUsage() const {
    vk::PhysicalDeviceMemoryBudgetPropertiesEXT memory_budget_props{};
    vk::PhysicalDeviceMemoryProperties2 props = {
        .pNext = &memory_budget_props,
    };
    physical_device.getMemoryProperties2(&props);

    u64 total_usage = 0;
    for (const size_t heap : valid_heaps) {
        total_usage += memory_budget_props.heapUsage[heap];
    }
    return total_usage;
}

u64 Instance::GetDeviceMemoryBudgetNow() const {
    vk::PhysicalDeviceMemoryBudgetPropertiesEXT memory_budget_props{};
    vk::PhysicalDeviceMemoryProperties2 props = {
        .pNext = &memory_budget_props,
    };
    physical_device.getMemoryProperties2(&props);

    u64 total_budget = 0;
    for (const size_t heap : valid_heaps) {
        total_budget += memory_budget_props.heapBudget[heap];
    }
    if (const u64 test_limit = Vram::TestLimit(); test_limit && !IsIntegrated()) {
        total_budget = std::min<u64>(total_budget, test_limit / 10 * 9);
    }
    return total_budget;
}

void Instance::QueryMemory(u64& usage, u64& budget) const {
    vk::PhysicalDeviceMemoryBudgetPropertiesEXT memory_budget_props{};
    vk::PhysicalDeviceMemoryProperties2 props = {.pNext = &memory_budget_props};
    physical_device.getMemoryProperties2(&props);
    usage = 0;
    budget = 0;
    for (const size_t heap : valid_heaps) {
        usage += memory_budget_props.heapUsage[heap];
        budget += memory_budget_props.heapBudget[heap];
    }
    if (const u64 test_limit = Vram::TestLimit(); test_limit && !IsIntegrated()) {
        budget = std::min<u64>(budget, test_limit / 10 * 9);
    }
}

bool Instance::VramAllows(u64 bytes) const {
    const u64 limit = Vram::TestLimit();
    if (!limit || !Vram::TestHard() || IsIntegrated()) {
        return true;
    }
    return GetDeviceMemoryUsage() + bytes <= limit / 10 * 9;
}

void Instance::ReportMemory(const char* why) const {
    vk::PhysicalDeviceMemoryBudgetPropertiesEXT budget{};
    vk::PhysicalDeviceMemoryProperties2 props = {.pNext = &budget};
    physical_device.getMemoryProperties2(&props);
    u64 usage = 0, heap_budget = 0, size = 0;
    for (const size_t heap : valid_heaps) {
        usage += budget.heapUsage[heap];
        heap_budget += budget.heapBudget[heap];
        size += props.memoryProperties.memoryHeaps[heap].size;
    }
    VmaBudget vma[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(allocator, vma);
    u64 vma_blocks = 0, vma_allocs = 0, vma_count = 0, vma_block_count = 0;
    for (const size_t heap : valid_heaps) {
        vma_blocks += vma[heap].statistics.blockBytes;
        vma_allocs += vma[heap].statistics.allocationBytes;
        vma_count += vma[heap].statistics.allocationCount;
        vma_block_count += vma[heap].statistics.blockCount;
    }
    std::printf("VRAM%s%s: allocs %llu+%llu (limit %u); process %llu MiB, driver budget %llu of %llu MiB; VMA blocks %llu MiB, "
                "allocations %llu MiB; images %llu (%llu MiB), arenas %llu (%llu MiB, %llu MiB in system memory), buffers "
                "%llu MiB; allocation failures %llu, recovered %llu, host fallbacks %llu\n",
                why[0] ? " " : "", why, (unsigned long long)vma_block_count,
                (unsigned long long)Vram::arena_count.load(),
                properties.limits.maxMemoryAllocationCount, (unsigned long long)(usage >> 20),
                (unsigned long long)(heap_budget >> 20), (unsigned long long)(size >> 20),
                (unsigned long long)(vma_blocks >> 20), (unsigned long long)(vma_allocs >> 20),
                (unsigned long long)Vram::image_count.load(),
                (unsigned long long)(Vram::image_bytes.load() >> 20),
                (unsigned long long)Vram::arena_count.load(),
                (unsigned long long)(Vram::arena_bytes.load() >> 20),
                (unsigned long long)(Vram::arena_host_bytes.load() >> 20),
                (unsigned long long)(Vram::buffer_bytes.load() >> 20),
                (unsigned long long)Vram::failed_allocs.load(),
                (unsigned long long)Vram::recovered_allocs.load(),
                (unsigned long long)Vram::host_fallbacks.load());
}

vk::FormatFeatureFlags2 Instance::GetFormatFeatureFlags(vk::Format format) const {
    const auto it = format_properties.find(format);
    if (it == format_properties.end()) {
        UNIMPLEMENTED_MSG("Properties of format {} have not been queried.", vk::to_string(format));
    }

    return it->second.optimalTilingFeatures | it->second.bufferFeatures;
}

bool Instance::IsFormatSupported(const vk::Format format,
                                 const vk::FormatFeatureFlags2 flags) const {
    if (format == vk::Format::eUndefined) [[unlikely]] {
        return true;
    }
    return (GetFormatFeatureFlags(format) & flags) == flags;
}

vk::Format Instance::GetSupportedFormat(const vk::Format format,
                                        const vk::FormatFeatureFlags2 flags) const {
    if (!IsFormatSupported(format, flags)) [[unlikely]] {
        switch (format) {
        case vk::Format::eD16UnormS8Uint:
            if (IsFormatSupported(vk::Format::eD24UnormS8Uint, flags)) {
                return vk::Format::eD24UnormS8Uint;
            }
            if (IsFormatSupported(vk::Format::eD32SfloatS8Uint, flags)) {
                return vk::Format::eD32SfloatS8Uint;
            }
            break;
        case vk::Format::eR8Srgb:
            if (IsFormatSupported(vk::Format::eR8Unorm, flags)) {
                return vk::Format::eR8Unorm;
            }
            break;
        case vk::Format::eR8G8Srgb:
            if (IsFormatSupported(vk::Format::eR8G8Unorm, flags)) {
                return vk::Format::eR8G8Unorm;
            }
            break;
        default:
            break;
        }
    }
    return format;
}

} // namespace Vulkan
