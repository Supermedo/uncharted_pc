// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: in-game settings menu (Dear ImGui), drawn by the presenter into the swapchain image
// after the game frame, at display resolution. Insert (keyboard) or L3+R3 (gamepad) opens it;
// while it is open the game gets no pad/keyboard input. Settings live in bbport_settings.h.

#pragma once

#include <string>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

union SDL_Event;
struct SDL_Window;

namespace Vulkan {
class Instance;
}

namespace BbOverlay {

/// Present thread, once: the ImGui context and its Vulkan backend.
void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count);

/// Window thread, for every SDL event: true when the menu consumed it.
bool HandleEvent(const SDL_Event& event);
/// Turns SDL text input on while the menu edits a value (window thread, once per poll).
void UpdateTextInput(SDL_Window* window);

/// Whether anything is drawn this frame (menu open or FPS counter on).
bool Visible();

/// Present thread: draws into `view` (layout ColorAttachmentOptimal).
void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent);
/// Every presented frame (FPS readout; BB_FPS_LOG=1 prints the rate every 5 seconds).
void NotePresent();

/// Any thread: queues the notice of an earned trophy (grade 1 platinum .. 4 bronze, `png` the
/// trophy's icon file or empty). Notices are shown at the top of the screen one after the other.
void NotifyTrophy(int grade, std::string game, std::string name, std::vector<unsigned char> png);

/// The menu is open or a text entry is shown: the game's input is held neutral.
bool CapturesInput();

/// Window thread: the game's text dialog (sceImeDialog). Shown as a box on screen until
/// `active` is false; typing goes to the window (sdl_window), the box only displays it.
void SetTextEntry(bool active, const std::string& prompt, const std::string& text);

} // namespace BbOverlay
