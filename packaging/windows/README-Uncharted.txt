Uncharted: The Nathan Drake Collection for Windows
===================================================

A native port: the original PS4 game code runs directly on Windows. No game files are
included. You need your own decrypted dump of the collection (CUSA02343, CUSA02320, ...;
the folder with eboot.bin, big2-ps4_Shipping.elf, big3-ps4_Shipping.elf, sce_sys,
u1data, u2data, u3data).

Requirements
- Windows 10 (1903 or later) or Windows 11, 64-bit.
- A Vulkan 1.3 graphics card with a current driver.
- About 8 GB of free memory (RAM + page file).
  Nothing else: Python and the libraries are inside this folder.

Starting
- Uncharted.exe opens the launcher, with a frameless window, per-game artwork and animated
  transitions. On the Game page choose your game folder, pick Drake's Fortune, Among Thieves or
  Drake's Deception on the Play page and press PLAY.
- Play Uncharted.exe starts the game straight away with the settings saved in the
  launcher, without opening it (for shortcuts and Steam). Uncharted.exe --play does the same.
- Play page -> "Add to Steam" adds the collection to your Steam library, with its cover
  from your game folder. "Desktop shortcut" puts it on the desktop.
- Artwork is in the art folder next to Uncharted.exe. Replace hero_u1.jpg, hero_u2.jpg or
  hero_u3.jpg to customize the wide banner for each game; updates keep this folder.
- Display page -> Frame rate: 60 FPS as on PS4, or 120 FPS. Gameplay keeps its normal speed
  and the cutscene videos still play at 60. Higher rates are experimental. Above your
  display's refresh rate use the Mailbox or Immediate presentation.
- Display page -> Anti-aliasing: TAA, AMD FSR 3.1, AMD FSR 4 or NVIDIA DLSS (DLAA) smooth the
  games' 1920x1080 picture with their own motion vectors, before their post-processing.
  DLSS needs a GeForce RTX. Upscaling to a larger window or screen stays with FSR 1.
- Display page -> Frame generation: AMD FSR 3 Frame Generation (any GPU, 2x) or NVIDIA DLSS
  Frame Generation (GeForce RTX 40: 2x, RTX 50: up to 4x) shows frames made between two of the
  game's: 60 FPS becomes 120 (or 240). Use the Mailbox or Immediate presentation and a display
  with that refresh rate. Menus and cutscene videos are shown as they are.
- Controls page: keyboard, mouse and controller work together, all remappable.
  Defaults: WASD move, mouse look, left button fire (R2), right button aim (L2), Space Cross,
  C Circle, F Square, E Triangle, Q/G L1/R1, Left Shift/V L3/R3, arrows and wheel d-pad,
  Escape/Enter Options, Tab touchpad. The cursor stays in the game window; hold Left Alt to
  free it. Insert opens the in-game settings menu. Bindings are saved in input.ini.
- Saves page -> Export backup (.zip) creates a portable archive of save data and trophies that
  you can upload to Google Drive. Restore backup (.zip) accepts that archive and backs up the
  current data before replacing it. Close the game before either action.

Data
- Saves and shader caches: user\ next to Uncharted.exe (the launcher can pick another folder).
- Launcher options: %APPDATA%\uncharted-launcher. Generated files (prepared game image): out\.
- The log of the last run: user\last_run.log (the launcher's Log page shows it live).

Updates
- The launcher looks for a new version when it starts and offers it on the left: Update
  downloads it, closes the launcher, installs it over this folder and opens it again.
  Saves, settings, input.ini and out\ are kept. Advanced page -> Updates turns the check off
  or runs it now.

Known problems
- Uncharted 3, Cartagena square (chapter 2): at high frame rates the game could close with
  "GPU crash: the graphics device was lost". Fixed in 0.3. If it still happens, set Frame
  rate to 60 on the Display page.
- Uncharted 3: green or black shapes can flash for one frame on some characters (also in
  shadPS4). Not fixed yet.

Port by SuperMedo.
