// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: keyboard, mouse and Xbox pictures on the games' button icon sheets and prompts. The
// PlayStation buttons are redrawn as the keys bound in input.ini (BB_INPUT_CONFIG) when the game
// uploads them, while the keyboard or mouse was used last, or as Xbox buttons after input from an
// Xbox or other non-PlayStation controller; after PlayStation controller input the game's own
// pictures come back. BB_BUTTON_PROMPTS=0 keeps the PlayStation buttons (the other values are in
// docs/keyboard-prompts.md).

#pragma once

#include <vector>

#include "common/types.h"

namespace VideoCore::ButtonPrompts {

enum class Format { Rgba8, Bc1 };

/// New contents for a rectangle of one mip level: RGBA8 pixels or BC1 blocks.
struct Patch {
    u32 mip, x, y, width, height;
    std::vector<u8> data;
};

/// Whether an image of this size and format may be a known picture (checked before hashing).
bool Candidate(u32 width, u32 height, Format format);

/// What the pictures show: the game's own PlayStation buttons, the bound keys, or Xbox buttons.
enum class Style : u8 { Original, Keyboard, Xbox };

/// Input thread: the device the player used last: 0 keyboard and mouse, 1 a PlayStation
/// controller, 2 an Xbox or other controller.
void SetDevice(int device);

/// The style shown now (BB_BUTTON_PROMPTS can force one), and a count of the switches between
/// styles.
Style Shown();
u32 Switches();

/// The patches that draw the picture whose mip 0 guest data hashes to `hash` in the style shown
/// now: empty when that is the game's own picture, null for images that are not known pictures.
const std::vector<Patch>* Find(u64 hash, u32 width, u32 height, u32 levels, Format format);

} // namespace VideoCore::ButtonPrompts
