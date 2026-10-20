# Uncharted: The Nathan Drake Collection for Windows

**Uncharted 1, 2 and 3 from the PS4 collection running natively on Windows 10 and 11, by [SuperMedo](https://github.com/Supermedo).** Mohammed Albarghouthi

Same idea as my [Bloodborne port](https://github.com/Supermedo/bloodborne_pc): the original PS4 game
code runs directly on your PC and the graphics are translated to Vulkan. Unpack the zip, start
`Uncharted.exe`, pick your game folder, choose a game and press **PLAY**.

**[Download the latest version](https://github.com/Supermedo/uncharted_pc/releases/latest)** · **[Join the Discord](https://discord.gg/s3RgH6cRfJ)**

> **No game files are included.** You need your own decrypted dump of the collection
> (CUSA02343, CUSA02320, ...: the folder with eboot.bin, big2-ps4_Shipping.elf,
> big3-ps4_Shipping.elf, sce_sys, u1data, u2data and u3data).
> This project is not affiliated with Sony Interactive Entertainment or Naughty Dog.

## Features

- All three games from one launcher: Drake's Fortune, Among Thieves and Drake's Deception.
- 60 FPS like on PS4, or 120 FPS (higher rates are experimental).
- Anti-aliasing with TAA, AMD FSR 3.1, AMD FSR 4 or NVIDIA DLSS (DLAA).
- Frame generation: AMD FSR 3 (any GPU) or NVIDIA DLSS (RTX 40 2x, RTX 50 up to 4x).
- Keyboard, mouse and controller together, all remappable. Mouse look in U3.
- Add to Steam and desktop shortcuts from the launcher, automatic updates.

## Requirements

- Windows 10 (1903 or later) or Windows 11, 64-bit.
- An Intel Core 4th generation (Haswell) or newer, an AMD Ryzen, or an AMD FX.
- A graphics card with Vulkan 1.3 and a current driver, with **6 GB of video memory or more**.
  4 GB cards (GTX 1650, RX 570) run out of video memory and crash after a while; on those, keep
  Anti-aliasing and Frame generation off.
- About 8 GB of free memory (RAM + page file).

## Known problems

- **Uncharted 3, Cartagena square (chapter 2):** at high frame rates (240 Hz setting, ~80 FPS)
  the game could freeze and close with "GPU crash: the graphics device was lost". It came from
  the speed fix for that area and is fixed in 0.3. If it still happens to you, set Frame rate to
  60 on the Display page and open an issue with your `user\last_run.log`.
- **Uncharted 3:** green or black shapes flash for a single frame on some characters (Sully's
  shirt in Cartagena). It happens in shadPS4 too, so it's a shader translation problem. Not fixed yet.
- **Crash with "ASSERTION: GetRenderFrameParams(frameToFreeInstancesFor)->m_gfxEopTick"**
  (mostly U1 after the first cutscene): the game expected the GPU side to be done with a frame it
  wanted to reuse, and stopped when it wasn't yet (shaders building on a first run). Fixed in
  0.3.2 (0.3.4 for the 1.02 versions): the game now waits for it. If you still see it, open an
  issue with your log.
- **Intel HD / UHD graphics:** a black screen on the HD 630 is fixed in 0.3.3. Older Intel chips
  (HD 520) can still crash inside Intel's own driver.
- **Uncharted 3 London bar crash** (fault at `0xd9c59b`, or device lost in the bar fight): fixed in
  0.3.4.
- **Uncharted 2 loading crashes** (`ReadSync file (req/read: ...)`, white textures then
  `iSubMesh < m_numSubMeshInstances`): file reads could come back cut short. Fixed in 0.3.5.
- **Older CPUs:** the games use instructions of the PS4's processor (AVX, BMI1, MOVBE, LZCNT).
  AMD FX runs since 0.3.6 (its missing MOVBE is emulated, a bit slower). Intel Core 3rd generation
  and older can't run the games yet; the game says so instead of crashing with 0xc000001d.
- **Uncharted 2, train wreck:** `IsFinite(deltaTransform)` in character-leg-ik.cpp, fixed in 0.3.6
  for the EU 1.00 version.
- **Uncharted 2: blur when aiming or on a turret, characters and grenades going through the
  floor or floating** (issues #33, #360): the game reads results of GPU work back on the CPU,
  among them the depth-of-field focus distance (a 1x1 render target) and physics results.
  After 0.3.6, render targets of at most 8x8 pixels go back to guest memory
  (`BB_READBACK_SMALL=0`: off), Uncharted 2 runs with accurate GPU memory (`BB_GPU_MERGE=1`
  brings back the faster merging), and textures over memory written by read backs are reloaded.
  On 0.3.6 itself, `BB_GPU_MERGE=0 BB_READBACK_LINEAR=1` under Advanced → Environment does the
  first two, but slowly: `BB_READBACK_LINEAR` waited for the whole GPU at every fence (20-40 FPS
  with the GPU half idle). Builds after 0.3.6 need neither variable.
- **Uncharted 2: dropped weapons drawn as white boxes** (issue #360; black cubes with
  `BB_READBACKS=0`): likely a colour that is finite on the PS4 and +Inf on PC, which the bloom
  spreads into blocks. Half-float colour packs now round toward zero as on the PS4 for Uncharted
  2 (`BB_F16_RTZ=1`, set by run.py; `BB_F16_RTZ=0` to compare). Not confirmed in game yet. To
  find out more: `BB_NONFINITE_MARK=1` paints non-finite colours (NaN magenta, +Inf green,
  beyond the half range yellow), `BB_CMP_UNORDERED=1` and `BB_CMP_CLASS_EXACT=1` give NaN and
  Inf compares the GCN results, `BB_OCCLUSION_LOG=1` / `BB_OCCLUSION_PIXELS=<n>` show and change
  the synthetic occlusion query counts. Each shader switch builds its own shader cache once.
- **Frame rate:** idle Naughty Dog job workers sleep instead of spinning (`BB_JOB_SLEEP=0`: off),
  and `BB_FRAME_STATS=1` prints where the GPU thread waits (in `last_run.log`, every 5 s).
- Frame rates above 120 are experimental.

## Building

MSYS2 CLANG64 on Windows: `bash build.sh` builds `out\bb-probe.exe`, and
`bash packaging/windows/package.sh` makes `dist\uncharted-windows` and the zip.

## Credits

Built on [bbport](https://github.com/deadinside28/bloodborne_pc), the native Linux port of
Bloodborne by deadinside28, and on the [shadPS4](https://github.com/shadps4-emu/shadPS4)
renderer. Video decoding (Videodec2) and the DLSS bridge come from
[IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR).
Thanks to Mindro for the shadPS4 Uncharted build that showed which problems to look for.

Also used: [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) and the AMD FidelityFX SDK (FSR),
FSR 4 assets from [FireBurn/Q2RTX](https://github.com/FireBurn/Q2RTX), the
[NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) (`nvngx_dlss.dll` under NVIDIA's license),
[LibAtrac9](https://github.com/Thealexbarney/LibAtrac9), [SDL3](https://github.com/libsdl-org/SDL),
[FFmpeg](https://ffmpeg.org), [Dear ImGui](https://github.com/ocornut/imgui),
[sirit](https://github.com/shadps4-emu/sirit), [magic_enum](https://github.com/Neargye/magic_enum),
[xbyak](https://github.com/herumi/xbyak),
[Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
[MSYS2](https://www.msys2.org) / [LLVM](https://llvm.org), [PyInstaller](https://pyinstaller.org),
[Pillow](https://python-pillow.org) and the Cinzel font (SIL Open Font License).

NVIDIA, GeForce RTX and DLSS are trademarks of NVIDIA Corporation. The default covers and the
icon are original artwork; no official art is included.

## License

GNU GPL v2 or later ([LICENSE](LICENSE)). Third-party components keep their own licenses
(see `licenses\` in the download).
