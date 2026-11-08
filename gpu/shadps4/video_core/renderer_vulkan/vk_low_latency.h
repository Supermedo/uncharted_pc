// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: low latency mode. NVIDIA Reflex (VK_NV_low_latency2) or AMD Anti-Lag 2
// (VK_AMD_anti_lag), both straight through Vulkan (no vendor SDK).
//
// The game's frame starts where it reads the controller (InputSample, called by the runtime's
// pad read). On NVIDIA the driver's sleep runs there, so the game reads input as late as the GPU
// allows instead of running a frame ahead of it; AMD's driver does the same inside its update
// call. The game's submit and our present give the driver the rest of the frame.
//
// BB_LOW_LATENCY: 0 off (default), 1 on, 2 on with NVIDIA's boost (GPU clocks stay up),
// m: markers only (the driver's timings for A/B, no sleep).
#pragma once

#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::LowLatency {

/// Instance::CreateDevice, before the device is made: adds the extensions this needs and returns
/// the feature structures to chain at the end of the device's features (nullptr: none).
void* PrepareDevice(vk::PhysicalDevice physical, const std::vector<std::string>& available,
                    std::vector<const char*>& extensions);
/// After the device is made (the dispatcher holds its functions).
void OnDevice(vk::Device device);

/// Swapchain::Create: chained into the create info (nullptr: none); OnSwapchain once made,
/// OnSwapchainDestroy before it goes.
const void* SwapchainNext();
void OnSwapchain(vk::SwapchainKHR swapchain);
void OnSwapchainDestroy();

/// The guest reads the controller (any thread). The first read after a presented frame starts
/// the next frame and may sleep.
void InputSample();
/// The guest hands a frame to the GPU (submit and flip).
void RenderSubmit();

/// Presenter: the game's own frame (not a generated or repeated one) is presented next. Returns
/// its frame id for the present (0: none).
u64 TakeGameFrame();
/// Around the swapchain present of that frame (id from TakeGameFrame; 0: nothing to mark).
void PresentStart(u64 id);
void PresentEnd(u64 id);
/// Presenter: the game's frame went out (counts frames; generated and repeated ones do not).
void GameFramePresented();
/// VkPresentIdKHR is chained to the presents.
bool UsesPresentId();

/// "Reflex", "Anti-Lag 2" while active, else nullptr (FPS counter).
const char* Name();

} // namespace Vulkan::LowLatency
