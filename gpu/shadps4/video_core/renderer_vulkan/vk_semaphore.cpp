// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <vector>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

#include "common/assert.h"

namespace Vulkan {

constexpr u64 WAIT_TIMEOUT = std::numeric_limits<u64>::max();
// bbport: called once by a GPU wait that has blocked for 5 seconds (the scheduler prints what
// the queue is waiting for).
void (*g_gpu_stall_report)() = nullptr;
// bbport: more context for a GPU crash (the buffer cache's address ranges).
void (*g_gpu_crash_context)() = nullptr;
// bbport: the draws and dispatches the GPU last reached (BB_GPU_CHECKPOINTS=1).
void (*g_gpu_checkpoint_report)() = nullptr;
extern bool g_device_fault_ext;
} // namespace Vulkan
namespace VideoCore {
void DumpBuffersNear(u64 address);
}
namespace Vulkan {

// bbport: the GPU crashed (device lost). The driver's description of the fault is printed and
// the game ends: every later GPU wait would fail the same way (it used to wait forever).
// Also called where a submission or present reports the loss (vk_semaphore.h).
[[noreturn]] void DeviceLost(vk::Device device) {
    std::printf("GPU crash: the graphics device was lost\n");
    if (g_device_fault_ext && VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceFaultInfoEXT) {
        const auto get = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceFaultInfoEXT;
        VkDeviceFaultCountsEXT counts{.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
        get(device, &counts, nullptr);
        std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
        std::vector<VkDeviceFaultVendorInfoEXT> vendor(counts.vendorInfoCount);
        counts.vendorBinarySize = 0;
        VkDeviceFaultInfoEXT info{.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT,
                                  .pAddressInfos = addresses.data(),
                                  .pVendorInfos = vendor.data()};
        get(device, &counts, &info);
        std::printf("GPU crash: %s\n", info.description);
        static constexpr const char* kinds[] = {"none", "read of an invalid address",
                                                "write to an invalid address",
                                                "execution of an invalid address",
                                                "instruction pointer (unknown)",
                                                "instruction pointer (invalid)",
                                                "instruction pointer (fault)"};
        for (const auto& a : addresses) {
            std::printf("GPU crash: %s at 0x%llx (+- 0x%llx)\n",
                        a.addressType < std::size(kinds) ? kinds[a.addressType] : "?",
                        static_cast<unsigned long long>(a.reportedAddress),
                        static_cast<unsigned long long>(a.addressPrecision));
        }
        for (const auto& a : addresses) {
            if (a.addressType >= VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT &&
                a.addressType <= VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT) {
                VideoCore::DumpBuffersNear(a.reportedAddress);
                if (AddressReportEnabled()) {
                    DumpAddressBindings(a.reportedAddress);
                }
            }
        }
        for (const auto& v : vendor) {
            std::printf("GPU crash: vendor 0x%llx/0x%llx %s\n",
                        static_cast<unsigned long long>(v.vendorFaultCode),
                        static_cast<unsigned long long>(v.vendorFaultData), v.description);
        }
    } else {
        std::printf("GPU crash: the driver gives no details (VK_EXT_device_fault unavailable)\n");
    }
    if (g_gpu_checkpoint_report) {
        g_gpu_checkpoint_report();
    }
    if (g_gpu_crash_context) {
        g_gpu_crash_context();
    }
    if (g_gpu_stall_report) {
        g_gpu_stall_report();
    }
    std::fflush(stdout);
    std::_Exit(5);
}

Semaphore::Semaphore(const Instance& instance_) : instance{instance_} {
    const vk::StructureChain semaphore_chain = {
        vk::SemaphoreCreateInfo{},
        vk::SemaphoreTypeCreateInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        },
    };
    auto [semaphore_result, sem] =
        instance.GetDevice().createSemaphoreUnique(semaphore_chain.get());
    ASSERT_MSG(semaphore_result == vk::Result::eSuccess, "Failed to create master semaphore: {}",
               vk::to_string(semaphore_result));
    semaphore = std::move(sem);
}

Semaphore::~Semaphore() = default;

void Semaphore::Refresh() {
    u64 this_tick{};
    u64 counter{};
    do {
        this_tick = gpu_tick.load(std::memory_order_acquire);
        auto [counter_result, cntr] = instance.GetDevice().getSemaphoreCounterValue(*semaphore);
        ASSERT_MSG(counter_result == vk::Result::eSuccess,
                   "Failed to get master semaphore value: {}", vk::to_string(counter_result));
        counter = cntr;
        if (counter < this_tick) {
            return;
        }
    } while (!gpu_tick.compare_exchange_weak(this_tick, counter, std::memory_order_release,
                                             std::memory_order_relaxed));
}

void Semaphore::Wait(u64 tick) {
    // No need to wait if the GPU is ahead of the tick
    if (IsFree(tick)) {
        return;
    }
    // Update the GPU tick and try again
    Refresh();
    if (IsFree(tick)) {
        return;
    }

    // If none of the above is hit, fallback to a regular wait
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &semaphore.get(),
        .pValues = &tick,
    };

    constexpr u64 StallReport = 5'000'000'000ull;
    bool reported = false;
    vk::Result result;
    while ((result = instance.GetDevice().waitSemaphores(
                &wait_info, reported ? WAIT_TIMEOUT : StallReport)) != vk::Result::eSuccess) {
        if (result == vk::Result::eErrorDeviceLost) {
            DeviceLost(instance.GetDevice());
        }
        if (!reported && g_gpu_stall_report) {
            std::printf("GPU stall: waiting 5 s for tick %llu of timeline %p\n",
                        static_cast<unsigned long long>(tick),
                        reinterpret_cast<void*>(static_cast<VkSemaphore>(Handle())));
            g_gpu_stall_report();
        }
        reported = true;
    }
    Refresh();
}

} // namespace Vulkan
