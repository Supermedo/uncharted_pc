# Uncharted on Windows

The port builds and runs natively on 64-bit Windows 10 (1903+) / 11 with the same sources as
on Linux. Players: see `README-Windows.txt` (it ships in the package).

## Build

Install [MSYS2](https://www.msys2.org) (`winget install MSYS2.MSYS2`), open the
**MSYS2 CLANG64** shell and install the toolchain and libraries (the counterpart of
`shell.nix`):

```bash
pacman -S --needed git patch \
  mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python} \
  mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} \
  mingw-w64-clang-x86_64-{sdl3,fmt,boost,robin-map,xxhash,ffmpeg,zydis} \
  mingw-w64-clang-x86_64-{spirv-tools,spirv-headers,spirv-cross} \
  mingw-w64-clang-x86_64-{protobuf,openssl,zlib,miniupnpc,nlohmann-json}
```

The CI Windows job must install this same CLANG64 package list. `magic_enum`, miniz and xbyak are submodules under `gpu/third_party/`.
Then, in the repository:

```bash
git submodule update --init --recursive
bb_game_dir=F:/path/to/CUSA02320
bash build.sh                      # out/bb-probe.exe, out/bb-gpu-capabilities.exe, out/bb-play.exe
BB_GAME_DIR="$bb_game_dir" python run.py
python launcher/uncharted_launcher.py
bash packaging/windows/package.sh  # dist/Uncharted-<version>-windows.zip
```

`run.py` is the shared Python game runner used by the Windows and Linux launchers.
The package freezes `launcher/uncharted_launcher.py` with PyInstaller into `Uncharted.exe`, which also runs `run.py` and the preparation scripts: players need no Python. `Play Uncharted.exe` (`packaging/windows/play.c`) starts the game directly with saved settings. The packager uses a native Windows Python 3.10+ from python.org or `actions/setup-python`, supplied through `WINPYTHON` when needed; MSYS2's Python is only used for source tools. It creates a private venv in `out/pyenv`, generates the missing PNG icon from `packaging/windows/make_uncharted_icon.py`, and writes a versioned ZIP plus its SHA-256 checksum. The ZIP version comes from `out/VERSION`, written by `build.sh`.

For a GitHub runner, install native Python with `actions/setup-python` and pass its `python.exe` path as `WINPYTHON` to the MSYS2 packaging step. The path may be in Windows or MSYS2 form. The release workflow fetches FSR 4 assets and NVIDIA's pinned DLSS SDK, builds the DLSS bridge, and includes their runtime files and licenses. Intel XeSS is available as an optional manual packaging step.
From a source tree it adds `C:\msys64\clang64\bin` (or `$MSYS2_ROOT`) to `PATH` for the DLLs.
`bash tools/fetch_fsr4_assets.sh` works in the CLANG64 shell as on Linux.

DLSS (optional): `DLSS_SDK_ROOT=<checkout of github.com/NVIDIA/DLSS> bash
packaging/windows/build_dlss.sh` builds `bbport_dlss.dll` (`gpu/dlss_bridge`, the only code that
uses the NVIDIA SDK, compiled with MSVC 2022) and copies it with `nvngx_dlss.dll` next to
`out/bb-probe.exe`; `package.sh` ships both when present. The renderer loads the bridge at run
time (`vk_dlss.cpp`), adds the Vulkan extensions NGX asks for, and DLSS gets the same inputs as
FSR 3 (scene color, depth, render-pixel motion vectors, jitter). `BB_DLSS=0` turns it off.

XeSS (optional, any GPU): `bash packaging/windows/fetch_xess.sh` downloads Intel's `libxess.dll` and its
license into `out/` (next to `out/bb-probe.exe`); `package.sh` ships them when present. The renderer
loads the DLL at run time (`vk_xess.cpp`, no MSVC needed) with the same inputs as DLSS. The PC needs the
Visual C++ 2022 x64 Redistributable. `BB_XESS=0` turns it off, `BB_XESS_DLL=<file>` picks another copy.

The Uncharted launcher and icon are in `launcher/uncharted_launcher.py` and `launcher/uncharted.ico`. The packager generates `out/uncharted.png` from the icon script for the portable ZIP.

## What is different from Linux

| Linux | Windows |
|---|---|
| `mov rax, fs:[0]` rewritten to `gs:[0]`, GS base set with `arch_prctl` | GS is the TEB and cannot move. The loader rewrites the displacement to `gs:[0x1480 + 8*slot]` (the TEB's `TlsSlots[slot]`, same 9-byte instruction) and each guest thread keeps its TCB in that TLS slot. The slot is claimed first thing in `main` (Vulkan drivers take many). |
| One memfd for direct + flexible memory, `mmap(MAP_FIXED)` views | A pagefile-backed section; the PS4 range 64 GiB–1 TiB is reserved as one placeholder and views replace placeholders (`VirtualAlloc2`/`MapViewOfFile3`, as in shadPS4). Partial unmaps split views and restore page protections (GPU tracking). |
| `-no-pie`, heap below 1 TiB | `--disable-high-entropy-va`: heaps and thread stacks stay low and out of the PS4 range. |
| SIGSEGV handler, `sigsetjmp` recovery | Vectored exception handler; `bb_setjmp`/`bb_longjmp` (`src/compat_win.c`) restore all Win64 callee-saved registers without SEH unwinding (guest frames have no unwind data; clang's `__builtin_longjmp` restores a wrong frame pointer on Win64). |
| Guest thread stacks from `runtime_low_map` | Windows allocates them, fully committed (`pthread_attr_setstacksize`): guest code has no stack probes and would skip the guard page. The main thread stack is 16 MiB committed. |
| glibc `nanosleep`, monotonic condition variables | High-resolution waitable timers (`compat_sleep_ns`; winpthreads' sleep rounds to 15.6 ms); semaphore deadlines use `CLOCK_REALTIME` (winpthreads rejects monotonic condition variables). |
| `libbbgpu.so` | `libbbgpu.a`, linked into `bb-probe.exe` by CMake (`gpu/CMakeLists.txt`), with a manifest for UTF-8 paths and long paths. |
| `execlp bash run.sh` (in-game restart) | `run.py` puts its command line in `BB_RESTART_COMMAND`; the new launch waits for the old process (`--after PID`). The launcher keeps the game in a job object. |
| Mod overlay with symlinks | Symlinks when allowed (Developer Mode), else junctions for folders and hard links for files. |

Diagnostics added on the way: `BB_FRAME_DUMP_TRIGGER=<file>` writes the guest display buffer
and the presented image of the next frame to `BB_DUMP_DIR` (raw 32-bit pixels), which also
works for menus and movies.

Not ported: the watchdog/`--timeout` thread dumps (Linux signals), userfaultfd tracking
(`BB_UFFD`), MangoHud, PGO (GCC flags), the GTK launcher (`launcher/bbport_launcher_win.py`, Tkinter, replaces it), and
the `tests/` programs (several use Linux-only APIs).
