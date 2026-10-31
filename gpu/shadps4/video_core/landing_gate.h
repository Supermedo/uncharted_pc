// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: deferred raycaster copies (BufferCache::DeferGuestCopy). The 64-dword copy of the game's
// mesh raycasters moves GPU-written results into a record array the guest CPU reads. The guest
// learns that the GPU is past the copy from the labels the port writes (end-of-pipe, end-of-shader,
// write data); those must not appear before the GPU-written bytes are in guest memory. Instead
// of waiting for the GPU at the copy, the bytes land on a worker thread when the GPU work that
// wrote them has completed, and the labels written meanwhile wait for it (a real GPU's labels
// do not appear before its work is done either).
#pragma once

#include <functional>

#include "common/types.h"

namespace VideoCore::LandingGate {

/// The highest sequence number of a deferred copy not yet landed; 0 when none is pending.
/// Taken when a guest-visible write is executed: it follows the copies registered before it.
u64 Pending();

/// Whether guest-visible signals must go through Run (a copy is pending or labels are waiting
/// behind one): signals that skip it would overtake the ones waiting.
bool Active();

/// Blocks until every deferred copy up to `seq` has landed.
void Wait(u64 seq);

/// Runs `fn` once every deferred copy up to `seq` has landed, in order with the other functions
/// passed here, without blocking the caller (inline when nothing is waiting).
void Run(u64 seq, std::function<void()> fn);

/// Like Run, but `fn` never runs on the calling thread: it always goes to the landing thread,
/// in order with the functions passed to Run. For signals whose handlers record Vulkan commands
/// (the flip interrupt: an EOP flip prepares the frame and waits for the Vulkan recording thread)
/// when the caller is the recording thread or a copy thread, which must not wait for themselves.
void Post(u64 seq, std::function<void()> fn);

/// True once the buffer cache is going down: the landing thread no longer lands anything, the
/// labels run at once and the waits for a landing return. Loops that wait for a label to run on
/// the landing thread leave when it is set.
bool Stopped();

} // namespace VideoCore::LandingGate
