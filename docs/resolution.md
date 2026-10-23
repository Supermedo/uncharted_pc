# Render resolution and ultrawide

How the port renders above 1080p, how the games' size and aspect ratio are changed, and how
the speaker layout reaches the games. The launcher's Display page sets all of it; the
variables below are what it passes to the game.

## Render resolution (`BB_GAME_RES=WxH`)

The three games draw at a fixed 1920x1080. Their renderer reads one display configuration
block (width, height, aspect, a few tile counts) that a startup function fills with
constants. `src/runtime_display.c` finds that function by its code in every executable
(1.00 and 1.02 of Uncharted 1, 2 and 3) and rewrites the immediates, so the game allocates
and renders everything at the new size. Output is a real higher resolution, not an enlarged
1080p picture. Checked on Uncharted 2 1.02 at 2560x1440 against the same scene at 1080p
scaled to a 2560x1440 window: edges, rope and cloth are sharp where the scaled picture is
soft.

What had to follow the size:

- **GPU memory budget.** The game keeps a table of its memory pools in module data (entries of
  id, type, size). It checks that each type adds up to a constant. The render target heaps
  are entry 133 (497 MiB, 561 in Uncharted 3) and entry 132 (140 MiB) of type 6, inside the
  GPU pool (entry 3 of type 2, 2988 MiB), whose total is validated against `0x120000000`.
  `run.py` computes how much each render target heap grows from the pixel count
  (`BB_GAME_RT_MB`, `BB_GAME_RT2_MB`), gives up the pool's unreserved remainder
  (`BB_GAME_FREE_MB`, entry 14) if needed, and raises the direct memory the same amount
  (`BB_DMEM_MB`). `runtime_display.c` applies the amounts to the table and the constant.
- **A 4 GiB limit.** The pool is one arena whose size the game stores in 32 bits. At 1440p
  the heaps need about 720 MiB (497 plus about 215). At 4K they would need about 1400 MiB and
  250 MiB more in the second heap, and the pool cannot grow that far (see Limits).
- **Movies.** The movie player gives the decoder four 4 MiB frame buffers and the games read
  decoded frames at their display size. `BB_GAME_MOVIE_MB` enlarges the slots and the check on
  their size (a masked search finds the five places in each executable), the HLE decoder
  (`videodec2_impl.cpp`) scales each frame into a canvas of the display size with the movie's
  shape kept (bars on ultrawide), and the compose compute shader's constants (1/1920,
  1/1088) are rewritten to 1/W, 1/H when the SPIR-V is built (`vk_shader_util.cpp`). The
  chroma plane follows luma rows padded to 64.

Variables (all optional, `run.py` fills the amounts):

| Variable | Meaning |
|---|---|
| `BB_GAME_RES=WxH` | render and display size |
| `BB_GAME_RT_MB`, `BB_GAME_RT2_MB`, `BB_GAME_FREE_MB`, `BB_DMEM_MB` | memory budget (see above) |
| `BB_GAME_MOVIE_MB` | movie frame buffer slot size, a multiple of 2 |
| `BB_GAME_SKIP=0x38,0x18` | display configuration fields to leave at 1920x1080 (diagnosis) |

Tests: `tests/test_display.c` (patch on a synthetic image) and `tests/test_display.py`.

## Ultrawide

`BB_GAME_RES` with a wider shape, for example `2560x1080` or `3440x1440`, also writes the
aspect ratio the game uses for its projection. The 3D view widens (more at the sides, the
vertical field of view is kept) and does not stretch. Menus keep their 16:9 layout in the
middle with the backdrop filling the width. Movies play with bars at the sides. The launcher
picks the width from a height and a shape (16:9, 21:9, 32:9) or follows the window or screen.

Uncharted 1 1.02 (`eboot.bin`) also gets `scripts/ui_aspect.py`, byte patches written into the same patch
file as the cheats. They are applied only when the executable's SHA-256 and every site match. Camera::Setup
builds y_scale from x_scale times the aspect, so the field of view needs its own change (its numerator
becomes 1/k, k = aspect / (16/9)). The UI is laid out in a virtual 1280x720 space that the 2D vertex
transforms (`x * 2/1280 - 1`) stretch over the screen, which made everything too wide: the x scale
constants become 2/(1280 k), the x offset -1 becomes -1/k (menus and the crosshair stay a centred 16:9
area), and the safe area the game fills at start (left 50, right 1229) moves out by half the extra width
so edge-anchored HUD elements keep to the screen edges. The weapon and ammo HUD has fixed positions and
stays in the 16:9 area. Other executables keep the game's own layout.

## Limits

- Checked on Uncharted 2 1.02: 2560x1440, 2880x1620, 3200x1800 and 3440x1440 render correctly,
  3840x2160 does not start. At 4K the render target heaps need more than the 4 GiB arena
  leaves (the texture heap would have to shrink by over 500 MiB, and the movie player's
  memory check then fails). The launcher stops at 5.8 million pixels (3200x1800). Raising it
  needs the arena's 32-bit size handled in the game's code.
- Uncharted 1 and 3 were checked at 2560x1440 (U1: menus, the opening movie and cutscenes;
  U3: the pub fight; U3 also at 3200x1800 and 3840x1620) and 1.00 only at the title menu of U2.
  One white blob appeared on a character in the 3840x1620 U3 shot (a flash like the one in the
  README's known problems, not confirmed as resolution related).
- Fixed-size effects of the games (bloom radii, shadow maps) do not grow with the picture.
- Movies were played at 2560x1440, 3440x1440 and (Uncharted 1, opening movie and cutscenes) 3200x1800.
