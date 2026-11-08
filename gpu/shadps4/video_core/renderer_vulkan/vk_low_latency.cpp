// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: low latency mode (vk_low_latency.h).

#include "video_core/renderer_vulkan/vk_low_latency.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

#include "core/emulator_settings.h"

namespace Vulkan::LowLatency {
namespace {

enum class Kind { Off, Reflex, AntiLag };

Kind kind = Kind::Off;
bool sleep_on = false; ///< false: markers only (BB_LOW_LATENCY=m)
bool boost = false;
bool present_id = false;

VkPhysicalDevicePresentIdFeaturesKHR present_id_features{
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
VkPhysicalDeviceAntiLagFeaturesAMD anti_lag_features{
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD};
const VkSwapchainLatencyCreateInfoNV swapchain_latency{
    .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV, .latencyModeEnable = VK_TRUE};

VkDevice device = VK_NULL_HANDLE;
PFN_vkSetLatencySleepModeNV set_sleep_mode{};
PFN_vkLatencySleepNV latency_sleep{};
PFN_vkSetLatencyMarkerNV set_marker{};
PFN_vkGetLatencyTimingsNV get_timings{};
PFN_vkAntiLagUpdateAMD anti_lag_update{};
VkSemaphore sleep_semaphore = VK_NULL_HANDLE;
u64 sleep_value = 0; ///< swapchain_mutex

std::mutex swapchain_mutex;
VkSwapchainKHR swapchain = VK_NULL_HANDLE;

std::atomic<u64> frame_id{0};  ///< the last frame whose input was read
std::atomic<u64> presented{0}; ///< game frames presented
std::atomic<u64> slept_at{0};  ///< `presented` at the last frame start
std::mutex submitted_mutex;
std::deque<u64> submitted; ///< frames handed to the GPU, not yet presented

/// BB_LOW_LATENCY: 0 off, 1 on, 2 on with boost, m markers only.
char Setting() {
    const char* env = std::getenv("BB_LOW_LATENCY");
    return env && env[0] ? env[0] : '0';
}

u32 MinimumIntervalUs() {
    const u32 limit = EmulatorSettings.GetFrameLimit();
    return limit ? 1'000'000 / limit : 0;
}

void Marker(VkLatencyMarkerNV marker, u64 id) {
    if (!id) {
        return;
    }
    std::scoped_lock lk{swapchain_mutex};
    if (!swapchain) {
        return;
    }
    const VkSetLatencyMarkerInfoNV info{
        .sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV, .presentID = id, .marker = marker};
    set_marker(device, swapchain, &info);
}

void AntiLag(VkAntiLagStageAMD stage, u64 id) {
    const VkAntiLagPresentationInfoAMD presentation{
        .sType = VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD,
        .stage = stage,
        .frameIndex = id,
    };
    const VkAntiLagDataAMD data{
        .sType = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD,
        .mode = sleep_on ? VK_ANTI_LAG_MODE_ON_AMD : VK_ANTI_LAG_MODE_OFF_AMD,
        .maxFPS = sleep_on ? EmulatorSettings.GetFrameLimit() : 0,
        .pPresentationInfo = &presentation,
    };
    anti_lag_update(device, &data);
}

/// Every 5 s: the driver's timings of the last frames, game input to the frame done.
void ReportTimings() {
    static auto window = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - window < std::chrono::seconds(5)) {
        return;
    }
    window = now;
    VkLatencyTimingsFrameReportNV reports[64];
    for (auto& report : reports) {
        report = {.sType = VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV};
    }
    VkGetLatencyMarkerInfoNV info{.sType = VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV,
                                  .timingCount = 64,
                                  .pTimings = reports};
    {
        std::scoped_lock lk{swapchain_mutex};
        if (!swapchain) {
            return;
        }
        get_timings(device, swapchain, &info);
    }
    double total = 0.0, sim_total = 0.0;
    u32 count = 0;
    for (u32 i = 0; i < std::min<u32>(info.timingCount, 64); ++i) {
        const auto& r = reports[i];
        if (!r.simStartTimeUs || r.presentEndTimeUs < r.simStartTimeUs) {
            continue;
        }
        total += double(r.presentEndTimeUs - r.simStartTimeUs);
        sim_total += double(r.simEndTimeUs > r.simStartTimeUs ? r.simEndTimeUs - r.simStartTimeUs
                                                               : 0);
        ++count;
    }
    if (!count) {
        return;
    }
    // U3 reads the controller just before it hands over the frame before: the frame marked with
    // an input is the one after it, so "input to present" is one game frame short. Good for A/B
    // (BB_LOW_LATENCY=m against 1), not as the full latency.
    std::printf("Low latency: Reflex %s, %u frames: input to present %.1f ms, input to submit "
                "%.1f ms\n",
                sleep_on ? (boost ? "on + boost" : "on") : "off (markers only)", count,
                total / count / 1000.0, sim_total / count / 1000.0);
    std::fflush(stdout);
}

/// Which threads read the controller, how often: the frame start is the first read after a
/// present, so this shows whether the reads follow the game's frames (every 30 s, twice).
void NoteRead() {
    static std::atomic<u32> reads{0};
    static std::atomic<int> reports{0};
    static auto window = std::chrono::steady_clock::now();
    reads.fetch_add(1, std::memory_order_relaxed);
    if (reports.load(std::memory_order_relaxed) >= 2) {
        return;
    }
    static std::mutex mutex;
    std::scoped_lock lk{mutex};
    const auto now = std::chrono::steady_clock::now();
    const double s = std::chrono::duration<double>(now - window).count();
    if (s < 30.0) {
        return;
    }
    static u64 last_presented = 0, last_starts = 0;
    const u64 p = presented.load(std::memory_order_relaxed);
    const u64 f = frame_id.load(std::memory_order_relaxed);
    std::printf("Low latency: %.0f controller reads/s, %.0f frames presented/s, %.0f frame "
                "starts/s\n",
                reads.exchange(0) / s, (p - last_presented) / s, (f - last_starts) / s);
    std::fflush(stdout);
    last_presented = p;
    last_starts = f;
    window = now;
    reports.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

void* PrepareDevice(vk::PhysicalDevice physical, const std::vector<std::string>& available,
                    std::vector<const char*>& extensions) {
    const char setting = Setting();
    if (setting == '0') {
        return nullptr;
    }
    const auto has = [&](const char* name) {
        return std::find(available.begin(), available.end(), name) != available.end();
    };
    if (has(VK_NV_LOW_LATENCY_2_EXTENSION_NAME)) {
        kind = Kind::Reflex;
        extensions.push_back(VK_NV_LOW_LATENCY_2_EXTENSION_NAME);
        void* next = nullptr;
        if (has(VK_KHR_PRESENT_ID_EXTENSION_NAME)) {
            VkPhysicalDevicePresentIdFeaturesKHR query{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
            VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                               .pNext = &query};
            VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFeatures2(physical, &features);
            if (query.presentId) {
                present_id = true;
                extensions.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
                present_id_features.presentId = VK_TRUE;
                next = &present_id_features;
            }
        }
        sleep_on = setting != 'm';
        boost = setting == '2';
        return next;
    }
    if (has(VK_AMD_ANTI_LAG_EXTENSION_NAME)) {
        VkPhysicalDeviceAntiLagFeaturesAMD query{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD};
        VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                           .pNext = &query};
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFeatures2(physical, &features);
        if (query.antiLag) {
            kind = Kind::AntiLag;
            sleep_on = setting != 'm';
            extensions.push_back(VK_AMD_ANTI_LAG_EXTENSION_NAME);
            anti_lag_features.antiLag = VK_TRUE;
            return &anti_lag_features;
        }
    }
    std::printf("Low latency: not supported by this GPU or driver (NVIDIA Reflex needs "
                "VK_NV_low_latency2, AMD Anti-Lag 2 VK_AMD_anti_lag)\n");
    return nullptr;
}

void OnDevice(vk::Device dev) {
    if (kind == Kind::Off) {
        return;
    }
    device = dev;
    const auto load = [&](const char* name) {
        return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(device, name);
    };
    if (kind == Kind::Reflex) {
        set_sleep_mode = reinterpret_cast<PFN_vkSetLatencySleepModeNV>(load("vkSetLatencySleepModeNV"));
        latency_sleep = reinterpret_cast<PFN_vkLatencySleepNV>(load("vkLatencySleepNV"));
        set_marker = reinterpret_cast<PFN_vkSetLatencyMarkerNV>(load("vkSetLatencyMarkerNV"));
        get_timings = reinterpret_cast<PFN_vkGetLatencyTimingsNV>(load("vkGetLatencyTimingsNV"));
        VkSemaphoreTypeCreateInfo type{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                       .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
        const VkSemaphoreCreateInfo create{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                           .pNext = &type};
        if (!set_sleep_mode || !latency_sleep || !set_marker || !get_timings ||
            VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateSemaphore(device, &create, nullptr,
                                                            &sleep_semaphore) != VK_SUCCESS) {
            std::printf("Low latency: Reflex functions missing, off\n");
            kind = Kind::Off;
            return;
        }
        std::printf("Low latency: NVIDIA Reflex %s%s\n",
                    sleep_on ? "on" : "off (markers only)", boost ? " + boost" : "");
    } else {
        anti_lag_update = reinterpret_cast<PFN_vkAntiLagUpdateAMD>(load("vkAntiLagUpdateAMD"));
        if (!anti_lag_update) {
            std::printf("Low latency: Anti-Lag function missing, off\n");
            kind = Kind::Off;
            return;
        }
        std::printf("Low latency: AMD Anti-Lag 2 %s\n", sleep_on ? "on" : "off");
    }
    std::fflush(stdout);
}

const void* SwapchainNext() {
    return kind == Kind::Reflex ? &swapchain_latency : nullptr;
}

void OnSwapchain(vk::SwapchainKHR chain) {
    if (kind != Kind::Reflex) {
        return;
    }
    std::scoped_lock lk{swapchain_mutex};
    swapchain = chain;
    const VkLatencySleepModeInfoNV mode{
        .sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV,
        .lowLatencyMode = sleep_on ? VK_TRUE : VK_FALSE,
        .lowLatencyBoost = boost ? VK_TRUE : VK_FALSE,
        .minimumIntervalUs = sleep_on ? MinimumIntervalUs() : 0,
    };
    if (set_sleep_mode(device, swapchain, &mode) != VK_SUCCESS) {
        std::printf("Low latency: the driver refused the sleep mode\n");
    }
}

void OnSwapchainDestroy() {
    if (kind != Kind::Reflex) {
        return;
    }
    std::scoped_lock lk{swapchain_mutex};
    swapchain = VK_NULL_HANDLE;
}

void InputSample() {
    if (kind == Kind::Off) {
        return;
    }
    NoteRead();
    // One frame start per presented frame: the first read after a present, on any thread.
    const u64 p = presented.load(std::memory_order_acquire);
    u64 last = slept_at.load(std::memory_order_relaxed);
    if (p == last || !slept_at.compare_exchange_strong(last, p, std::memory_order_acq_rel)) {
        return;
    }
    const u64 id = frame_id.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (kind == Kind::AntiLag) {
        AntiLag(VK_ANTI_LAG_STAGE_INPUT_AMD, id); // the driver sleeps here when it is on
        return;
    }
    u64 value = 0;
    {
        std::scoped_lock lk{swapchain_mutex};
        if (!swapchain) {
            return;
        }
        value = ++sleep_value;
        const VkLatencySleepInfoNV info{.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV,
                                        .signalSemaphore = sleep_semaphore,
                                        .value = value};
        if (latency_sleep(device, swapchain, &info) != VK_SUCCESS) {
            return;
        }
    }
    // The driver signals when the game should start; never more than 50 ms (a swapchain made
    // again meanwhile, a driver that stops pacing).
    const VkSemaphoreWaitInfo wait{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                   .semaphoreCount = 1,
                                   .pSemaphores = &sleep_semaphore,
                                   .pValues = &value};
    VULKAN_HPP_DEFAULT_DISPATCHER.vkWaitSemaphores(device, &wait, 50'000'000);
    Marker(VK_LATENCY_MARKER_SIMULATION_START_NV, id);
    Marker(VK_LATENCY_MARKER_INPUT_SAMPLE_NV, id);
}

void RenderSubmit() {
    if (kind == Kind::Off) {
        return;
    }
    const u64 id = frame_id.load(std::memory_order_acquire);
    if (!id) {
        return;
    }
    {
        std::scoped_lock lk{submitted_mutex};
        if (!submitted.empty() && submitted.back() >= id) {
            return; // no frame start since the last submit (the game reads no controller now)
        }
        submitted.push_back(id);
        while (submitted.size() > 8) {
            submitted.pop_front();
        }
    }
    if (kind == Kind::Reflex) {
        Marker(VK_LATENCY_MARKER_SIMULATION_END_NV, id);
        Marker(VK_LATENCY_MARKER_RENDERSUBMIT_START_NV, id);
        Marker(VK_LATENCY_MARKER_RENDERSUBMIT_END_NV, id);
    }
}

u64 TakeGameFrame() {
    if (kind == Kind::Off) {
        return 0;
    }
    std::scoped_lock lk{submitted_mutex};
    if (submitted.empty()) {
        return 0;
    }
    const u64 id = submitted.front();
    submitted.pop_front();
    return id;
}

void PresentStart(u64 id) {
    if (kind == Kind::Reflex) {
        Marker(VK_LATENCY_MARKER_PRESENT_START_NV, id);
    } else if (kind == Kind::AntiLag && id) {
        AntiLag(VK_ANTI_LAG_STAGE_PRESENT_AMD, id);
    }
}

void PresentEnd(u64 id) {
    if (kind == Kind::Off) {
        return;
    }
    if (kind == Kind::Reflex) {
        Marker(VK_LATENCY_MARKER_PRESENT_END_NV, id);
        ReportTimings();
    }
}

void GameFramePresented() {
    if (kind != Kind::Off) {
        presented.fetch_add(1, std::memory_order_release);
    }
}

bool UsesPresentId() {
    return kind == Kind::Reflex && present_id;
}

const char* Name() {
    if (!sleep_on) {
        return nullptr;
    }
    return kind == Kind::Reflex ? "Reflex" : kind == Kind::AntiLag ? "Anti-Lag 2" : nullptr;
}

} // namespace Vulkan::LowLatency
