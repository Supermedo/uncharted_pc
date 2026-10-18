# Architecture

How the port works, for people reading or changing the code. For players, see
[README.md](../README.md). For build steps, see
[packaging/windows/README.md](../packaging/windows/README.md).

## The short version

This is **not an emulator**. The decrypted PS4 game binary is loaded as an
ordinary x86-64 image and executed directly on the CPU. There is no CPU
translation and no dynamic binary translation. What is replaced is:

1. **The system calls.** The PS4's Orbis libc and its system libraries are
   reimplemented in C (`src/runtime_*.c`) and the guest's imports are bound to
   them.
2. **The graphics API.** The PS4's GNM graphics commands are translated to
   Vulkan by a vendored copy of the shadPS4 renderer (`gpu/shadps4/`), which is
   driven by the port's own shim (`gpu/shim/`).

The only thing that must match the PS4 exactly is the ABI those two layers
present to the guest.

## Layers

```
  Uncharted.exe (launcher, Python + PyInstaller)
      |
      | runs scripts/prepare.py, link_modules.py, content_profile.py
      v
  run.py  (builds the boot image, sets BB_* environment)
      |
      v
  bb-probe.exe  =  src/probe.c        (loader, faults, patches)
                +  src/runtime*.c     (HLE system libraries)
                +  src/compat_win.c   (POSIX shims for MinGW)
                +  gpu/bbgpu          (shim over the shadPS4 renderer)
                      |
                      v
                gpu/shadps4/         (vendored, see gpu/VENDOR.txt)
```

### `src/probe.c` — the loader

Reads `out/<exe>/boot-linked.bin`, parses the ELF program headers, maps the
image near its base address, applies compiled patches, then enters the guest.
Its main responsibilities:

- **Import resolution.** Each import is a scoped NID, `"NID#lib#mod"`.
  `runtime_resolve()` dispatches on that string. An import that cannot be
  resolved **stops the process** rather than stubbing to a fake success — a
  silent stub turns a missing feature into a crash much later.
- **Guest hooks.** `RuntimeGuestCallback` grafts a host function onto a guest
  function prologue: check the signature, relocate the leading PIC
  instructions to a thunk, then jump. `patch_eop_tick_wait` (the
  `GetRenderFrameParams` GPU-crash fix) is one of these.
- **Fault handling.** Vectored/unhandled exception handlers on Windows, signals
  on Linux, plus a speculative-read `setjmp` recovery path. `src/compat_win.c`
  hand-writes `bb_setjmp`/`bb_longjmp` in assembly because clang's builtins
  restore a wrong frame pointer on Win64 and guest frames have no unwind data.
- **A MOVBE emulator**, for AMD FX, which lacks the instruction.

### `src/runtime*.c` — the HLE runtime

One file per PS4 library. Each provides `runtime_X_resolve()`, and usually
`runtime_X_report()` and `runtime_X_configure()`. The largest:

| File | What it provides |
|---|---|
| `runtime_memory.c` | PS4 virtual memory: direct/physical pools, flexible memory, reservations, protect. On Windows, pagefile-backed sections via `VirtualAlloc2`/`MapViewOfFile3`. Guest addresses are kept below 1 TiB so GPU descriptors stay 40-bit. |
| `runtime_pad.c` | Controller, keyboard and mouse over SDL3, remappable via `input.ini`. |
| `runtime_ajm.c` | Audio decode: ATRAC9 through vendored LibAtrac9, MP3 through FFmpeg. |
| `runtime_services.c` | System services for an offline single-user console: user 1 logged in, no network, dialogs auto-complete. |
| `runtime_file.c` | The guest VFS: `/app0` and `/hostapp` to the game root, `/temp0` and `/data` to the user directory. |
| `runtime_thread.c` | Guest threads on host threads, FreeBSD-variant-II TCB. The loader rewrites the guest's `fs:[0]` accesses to `gs:[0]` on Linux or a claimed TEB TLS slot on Windows. |
| `runtime_fiber.c` | User-mode fibers, saving SysV callee-saved registers plus MXCSR and the x87 control word. Fibers may migrate across threads. |

### `gpu/` — the renderer

`gpu/shadps4/` is shadPS4 pinned to a single commit (`gpu/VENDOR.txt`).
**Local edits are marked with `bbport:` comments** — about 390 lines across 100
files. Keep that convention: it is what makes re-vendoring possible.

`gpu/shim/` is this port's own code on top of it (~3.3k lines): the C ABI in
`gpu/bbgpu.h`, the in-game ImGui settings menu, async host copy threads, and
the window. The port's own compute and fragment passes live in
`gpu/shadps4/video_core/host_shaders/` (`taa.comp`, `camera_motion.comp`,
`frame_gen_inputs.comp`, the FSR passes, and so on) — see
[docs/upscaler.md](upscaler.md).

`gpu/dlss_bridge/` and `gpu/bbnet/` are optional: the DLSS/NGX bridge is
built separately with MSVC (`packaging/windows/build_dlss.sh`), and `bbnet` is
the network/PSN glue for private co-op servers.

## Runtime configuration

Everything is switched through `BB_*` environment variables or `bbport.ini`, so
that a running game can be A/B tested without a rebuild:

- `BB_TOGGLE_FILE` — a bitmask toggled at runtime, used by `tools/ab.sh` to
  compare two builds of the same scene.
- `BB_FRAME_STATS=1` — frame timing.
- `BB_CAPTURE_TRIGGER=<file>` — dump one frame's passes, targets, shaders and
  constants. This is the main tool for understanding what the game is doing.
- `BB_WATCH`, `BB_POKE` — loader debugging.

Undocumented ones are found in `src/probe.c` and `docs/upscaler.md`.

## Game patches

`patches/Bloodborne.xml` is a shadPS4/GoldHEN community patch database in the
guest's address space. It is **not** applied at runtime directly:
`scripts/patches.py` compiles selected patches into `out/patches.bin` (literal
writes only — pattern/mask patches are rejected), the presets used by
`bbport.ini`, the in-game menu and the launcher map onto them, and the loader
applies the result via `--patches`.

**That database is Bloodborne's.** All 60 of its metadata blocks are
`Title="Bloodborne"`, and its eight `<TitleID>` values are all Bloodborne
(`CUSA03173` and its DLC variants). Its `AppVer` is `01.09`, which is
Bloodborne's version. There is no Uncharted patch set in this tree, so for
Uncharted the compiler always declines and the games run at their own frame rate.

The collection's own Title IDs, and the versions Sony ever shipped for them
([orbispatches.com](https://orbispatches.com/), checked 2026-10-18):

| Title ID | Region | Versions |
|---|---|---|
| CUSA01399 | AS | 01.02 |
| CUSA02320 | US | 01.01, 01.02 |
| CUSA02343 | EU | 01.02 |
| CUSA02344 | EU | 01.02 |
| CUSA02826 | EU | 01.01, 01.02 |

**There is no 01.09 for this collection.** `01.02` (2015-11-04) is the final
version of all five, so the version check can never be satisfied by an Uncharted
build and no update will change that. A 60/120 FPS patch set for it would have to
be written for `01.01`/`01.02` and is not in this tree.

Two guards stop a patch set from being written into the wrong executable, because
a patch is a raw byte write at a fixed address and two games can share a version
string while having entirely different code there:

- **App version.** A game whose `APP_VER` is not the one the database was written
  for is declined, and the run continues unpatched.
- **Title ID.** `check_patch_target()` in `patches.py` additionally refuses when
  the game's `TITLE_ID` is not one the database declares. This is the guard that
  matters: without it, Bloodborne 1.09 and a hypothetical Uncharted 1.09 pass the
  version check together, and 19 of the database's writes fall inside Uncharted's
  10 MB `eboot.bin` and would corrupt it. The `<TitleID>` element was in the XML
  all along but nothing read it.

`BB_FORCE_PATCHES=1` overrides the version check only; `patches.py`'s bounds
check then rejects writes outside the image before any file is written.

Both `run.sh` and `run.py` compile the patches and pass `--patches` to the loader.

## Two platforms, one codebase

The C runtime is built for both Linux and MSYS2/CLANG64 (MinGW + winpthreads),
with `_WIN32` branches throughout and `src/compat_win.c` covering the POSIX
calls that MinGW lacks. The differences are documented in a table in
[packaging/windows/README.md](../packaging/windows/README.md). Notable ones:

- `libbbgpu.so` on Linux, but a **static** library on Windows, so the loader can
  resolve the `runtime_*` symbols the renderer calls.
- VEH instead of `SIGSEGV` for guest faults.
- The launcher is GTK4 on Linux and Tkinter on Windows.

The Linux tree retains the Bloodborne originals (`launcher/bbport_*.py`,
`packaging/appimage.sh`); the Windows tree is Uncharted-specific. The
`bb-probe` binary name is inherited from `bloodborne_pc` and is used on both.

## Tests

The Python suite and the C tests run in CI (`.github/workflows/ci.yml`); the C++
renderer targets do not, so those are run by hand.

- **Python** (`tests/test_*.py`) — preparation, linking, patches, mods and
  launcher settings. `python -m unittest discover -s tests`.
- **C** (`tests/test_*.c`) — runtime contracts: semaphores, threads, memory,
  the file layer. `bash build.sh --test` on Linux; the MSYS2 CLANG64 shell
  builds the same set through CMake.
- **C++** (`tests/test_*.cpp`) — renderer and shader behaviour. Each is an
  `EXCLUDE_FROM_ALL` target: `ninja -C out/gpu motion-history-test taa-shader-test ...`.

## Where to look for things

| Question | Start at |
|---|---|
| A PS4 system call misbehaves | `src/runtime_<subsystem>.c`, the `resolve` table at the bottom |
| The game crashes on load | `src/probe.c` fault handlers; `user/last_run.log` |
| A shader renders wrong | `gpu/shadps4/video_core/renderer_vulkan/`, `BB_CAPTURE_TRIGGER` |
| Upscaling or frame generation | `gpu/shim/`, `host_shaders/`, [docs/upscaler.md](upscaler.md) |
| FPS, resolution or cheat patches | `patches/Bloodborne.xml`, `scripts/patches.py` |
| Performance regressions | [docs/parallel_gpu.md](parallel_gpu.md), `BB_FRAME_STATS`, `tools/ab.sh` |
| What ships to players | `packaging/windows/package.sh`, `licenses/MANIFEST.txt` |