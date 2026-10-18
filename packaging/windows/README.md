# The port on Windows

The port builds and runs natively on 64-bit Windows 10 (1903+) / 11 from the same sources as on
Linux. How the port works is in [../../docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md).
Players: see `README-Uncharted.txt`, which ships in the package as `README.txt`.

## Build

Install [MSYS2](https://www.msys2.org) (`winget install MSYS2.MSYS2`), open the
**MSYS2 CLANG64** shell and install the toolchain and libraries (the counterpart of
`shell.nix`):

```bash
pacman -S --needed git patch \
  mingw-w64-clang-x86_64-{clang,cmake,ninja,pkgconf} \
  mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} \
  mingw-w64-clang-x86_64-{sdl3,fmt,boost,robin-map,xxhash,ffmpeg,zydis} \
  mingw-w64-clang-x86_64-{spirv-tools,spirv-headers,spirv-cross} \
  mingw-w64-clang-x86_64-{protobuf,openssl,zlib,miniupnpc,nlohmann-json}
```

`magic_enum`, `miniz` and `xbyak` (not packaged by MSYS2) are submodules under
`gpu/third_party/`. **Clone recursively** (`git clone --recursive`) or run
`git submodule update --init --recursive`; a ZIP download cannot be built, because `build.sh`
uses git to check the submodules out and to apply the FSR-Vulkan patches.

`build.sh` normalises line endings before applying those patches. With git's Windows default
(`core.autocrlf=true`) the submodule is checked out with CRLF while the patch files are LF, so
`git apply` finds no context match and the build stops with *"patch does not apply"*. The script
sets `core.autocrlf=false` in the submodule and re-checks it out once. If a patch still does not
apply, it now says which one and how to reset the tree, instead of aborting on the first line of
a `git apply` error.

`launcher/uncharted.png` (and `launcher/uncharted.ico`) are committed; both are drawn by
`packaging/windows/make_uncharted_icon.py`, and `package.sh` copies them into the package.

Then, in the repository:

```bash
bash build.sh                      # out/bb-probe.exe, out/bb-gpu-capabilities.exe, out/bb-play.exe
BB_GAME_DIR=F:/path/to/CUSA02343 python run.py
python launcher/uncharted_launcher.py
bash packaging/windows/package.sh  # dist/uncharted-windows/ and .zip: Uncharted.exe + DLLs
```

`run.py` is the Windows `run.sh` (same variables; works from a plain Windows Python too).
The package freezes the launcher with PyInstaller (a private venv in `out/pyenv`, from a
python.org Python) into `Uncharted.exe`, which also runs `run.py` (`--run`), the preparation
scripts (`--script`) and the game without the window (`--play`): players need no Python.
`Play Uncharted.exe` (`packaging/windows/play.c`) is a small native program that runs
`Uncharted.exe --play` from its folder and waits for it, for players who want to start the
game directly with the saved settings.
`patches.py` compiles only writes whose app version and Title ID the database declares. The only
database in the tree is Bloodborne's, so for the collection it always declines and the games run
at their own frame rate with live resolution changes (`BB_FORCE_PATCHES=1` overrides the version
check only).
From a source tree it adds `C:\msys64\clang64\bin` (or `$MSYS2_ROOT`) to `PATH` for the DLLs.
`bash tools/fetch_fsr4_assets.sh` works in the CLANG64 shell as on Linux.

DLSS (optional): `DLSS_SDK_ROOT=<checkout of github.com/NVIDIA/DLSS> bash
packaging/windows/build_dlss.sh` builds `bbport_dlss.dll` (`gpu/dlss_bridge`, the only code that
uses the NVIDIA SDK, compiled with MSVC 2022) and copies it with `nvngx_dlss.dll` next to
`out/bb-probe.exe`; `package.sh` ships both when present. The renderer loads the bridge at run
time (`vk_dlss.cpp`), adds the Vulkan extensions NGX asks for, and DLSS gets the same inputs as
FSR 3 (scene color, depth, render-pixel motion vectors, jitter). `BB_DLSS=0` turns it off.

The icon (`launcher/uncharted.ico`/`.png`) is drawn by `packaging/windows/make_uncharted_icon.py`
and built into `Uncharted.exe`, `bb-probe.exe` (the game window) and `bb-gpu-capabilities.exe`.

## Tests

The C tests build and run here too:

```bash
bash tests/build.sh            # or: bash build.sh --test
BB_TEST_MINIMAL=1 bash tests/build.sh   # skips test_pad when SDL3/FFmpeg are absent
```

They need the submodules (LibAtrac9) plus SDL3 and FFmpeg development files, because the tests
link the whole HLE runtime — `runtime_pad.c` and `runtime_ajm.c` included. `tests/stubs.c`
stands in for the `bbgpu_*` entry points that live in the GPU library. `.github/workflows/ci.yml`
runs this on `windows-latest` as well as Linux.

## Third-party licenses

`package.sh` copies the licenses listed in `licenses/MANIFEST.txt` and **fails the build** when
one for linked code is missing, so a release cannot silently ship an incomplete set. Run
`bash packaging/windows/check_licenses.sh` on its own to report what is present. Entries marked
`pkg` are resolved from the MSYS2 tree at package time; `optional` entries (the DLSS bridge)
ship only when built.

xbyak is the one component with no license file in its own repository: the headers state
"modified new BSD license", BSD-3-Clause, so that text is reproduced in
`licenses/xbyak-BSD-3-Clause.txt`.

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
| `setenv`, `pread`/`pwrite`, `fsync`, `realpath`, `localtime_r` from glibc | Provided by `src/compat_win.h`/`compat_win.c` on top of the CRT. |

Diagnostics added on the way: `BB_FRAME_DUMP_TRIGGER=<file>` writes the guest display buffer
and the presented image of the next frame to `BB_DUMP_DIR` (raw 32-bit pixels), which also
works for menus and movies.

Not ported: the watchdog/`--timeout` thread dumps (Linux signals), userfaultfd tracking
(`BB_UFFD`), MangoHud, PGO (GCC flags), and the GTK launcher. The `tests/` programs used to be
Linux-only; they now build here as well (see above), with `tests/portable.h` covering the
differences in scratch paths and file linking.