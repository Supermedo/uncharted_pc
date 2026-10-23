#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Launcher for the Windows port of Uncharted: The Nathan Drake Collection (Tkinter).

Chooses the game folder, which of the three games to start, the display and console options,
and runs run.py with them as environment variables. Launcher options live in
%APPDATA%/uncharted-launcher/settings.json.

Frozen with PyInstaller the same Uncharted.exe also runs the game without the window
(`--play`), run.py (`--run`) and the preparation scripts (`--script`), so a packaged port
needs no Python installation.
"""
import ctypes
import json
import os
from pathlib import Path
import queue
import re
import runpy
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import webbrowser
import zipfile

FROZEN = getattr(sys, 'frozen', False)
PORT_DIR = Path(sys.executable).resolve().parent if FROZEN else Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PORT_DIR / 'scripts'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import trophies  # noqa: E402  (scripts/trophies.py: the games' trophy lists)
import uncharted_i18n

DATA_DIR = Path(os.environ.get('BB_DATA_DIR', PORT_DIR))
CONFIG_DIR = Path(os.environ.get('APPDATA', Path.home())) / 'uncharted-launcher'
CONFIG_FILE = CONFIG_DIR / 'settings.json'
NAME = 'Uncharted'
AUTHOR = 'SuperMedo'
# This build; GitHub release tags are v<VERSION> (their digits are compared).
VERSION = '0.3.8'
RELEASES_API = 'https://api.github.com/repos/Supermedo/uncharted_pc/releases/latest'
RELEASES_PAGE = 'https://github.com/Supermedo/uncharted_pc/releases/latest'
UPDATE_DIR = Path(tempfile.gettempdir()) / 'uncharted-update'
# Never copied over an installation by an update (the package does not hold them either).
USER_FILES = ('user', 'out', 'art', 'bbport.ini', 'input.ini', 'last_run.log')
MAX_LOG_LINES = 6000
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)

GAMES = [
    ('u1', "Drake's Fortune", 'Uncharted 1 · 2007', 'eboot.bin'),
    ('u2', 'Among Thieves', 'Uncharted 2 · 2009', 'big2-ps4_Shipping.elf'),
    ('u3', "Drake's Deception", 'Uncharted 3 · 2011', 'big3-ps4_Shipping.elf'),
]
# Console language (system parameter 1), as the PS4 numbers them.
LANGUAGES = [('1', 'English (United States)'), ('18', 'English (United Kingdom)'), ('2', 'French'),
             ('22', 'French (Canada)'), ('4', 'German'), ('5', 'Italian'), ('3', 'Spanish'),
             ('20', 'Spanish (Latin America)'), ('7', 'Portuguese'), ('17', 'Portuguese (Brazil)'),
             ('6', 'Dutch'), ('8', 'Russian'), ('16', 'Polish'), ('19', 'Turkish'), ('21', 'Arabic'),
             ('0', 'Japanese'), ('9', 'Korean'), ('11', 'Chinese (Simplified)'), ('10', 'Chinese (Traditional)'),
             ('12', 'Finnish'), ('13', 'Swedish'), ('14', 'Danish'), ('15', 'Norwegian')]
PRESENT_MODES = [('Mailbox', 'Mailbox (low latency, no tearing)'), ('Fifo', 'FIFO (VSync)'),
                 ('FifoRelaxed', 'FIFO Relaxed'), ('Immediate', 'Immediate (tearing)')]
# The display rate the games run at (BB_VBLANK_HZ): they time gameplay with the real clock, and
# the prerecorded cutscenes keep 60 frames per second at every setting.
FRAME_RATES = [('30', '30 FPS'), ('60', '60 FPS (as on PS4)'), ('120', '120 FPS'), ('144', '144 FPS (experimental)'),
               ('165', '165 FPS (experimental)'), ('240', '240 FPS (experimental)')]
# The window (or the screen in fullscreen) shows the game's picture at its size: FSR 1 (EASU) or
# bilinear when the picture is smaller, then RCAS sharpening (BB_RCAS_ATTENUATION, lower is sharper).
WINDOW_SIZES = [('1280x720', '1280 × 720'), ('1600x900', '1600 × 900'),
                ('1920x1080', '1920 × 1080'), ('2560x1440', '2560 × 1440'),
                ('3840x2160', '3840 × 2160 (4K)'), ('2560x1080', '2560 × 1080 (21:9)'),
                ('3440x1440', '3440 × 1440 (21:9)'), ('3840x1600', '3840 × 1600 (21:9)'),
                ('5120x1440', '5120 × 1440 (32:9)')]
# The size the games themselves render and show (BB_GAME_RES: run.py, src/runtime_display.c): a real
# higher resolution, not an enlarged 1080p picture. The height here and the shape below make the
# width; the games keep their memory for it, so the sizes stop where it runs out.
RENDER_HEIGHTS = [('1080', "1080p (the games' own)"), ('1440', '1440p'), ('1620', '1620p'), ('2160', '2160p (4K)')]
ASPECTS = [('16:9', "16:9 (the games' own)"), ('21:9', '21:9 ultrawide'), ('32:9', '32:9 super ultrawide'),
           ('window', "The shape of the window or screen")]
ASPECT_RATIOS = {'16:9': 16 / 9, '21:9': 64 / 27, '32:9': 32 / 9}
# Widths the screens of these shapes really have at each height (21:9 is 64:27 at 1080p and 43:18 at 1440p).
SCREEN_WIDTHS = {('21:9', 1080): 2560, ('21:9', 1440): 3440, ('21:9', 1600): 3840, ('21:9', 1620): 3840,
                 ('21:9', 2160): 5120, ('32:9', 1080): 3840, ('32:9', 1440): 5120, ('32:9', 2160): 7680}
MAX_RENDER_PIXELS = 5_800_000   # what the games' GPU memory holds: 3200x1800 starts, 4K does not (docs/resolution.md)
SPEAKERS = [('auto', 'Automatic (the Windows speaker setup)'), ('2', 'Stereo'), ('6', '5.1 surround'), ('8', '7.1 surround')]
SCALERS = [('fsr1', 'FSR 1 (AMD FidelityFX, sharp)'), ('bilinear', 'Smooth (bilinear)')]
SHARPENING = [('off', 'Off'), ('1000', 'Low'), ('500', 'Medium'), ('250', 'High'), ('0', 'Maximum')]
# Temporal anti-aliasing at the games' 1920x1080 (BB_UPSCALER): it takes the scene before the
# games' post-processing with their own motion vectors, so it replaces their edge smoothing.
ANTI_ALIASING = [('off', "Off (the game's own)"), ('taa', 'TAA'), ('fsr3', 'AMD FSR 3.1 (Native AA)'),
                 ('fsr4', 'AMD FSR 4 (Native AA)'), ('dlss', 'NVIDIA DLSS (DLAA, GeForce RTX)')]
FSR411 = ('fsr411', 'AMD FSR 4.1.1 (Native AA)')
XESS = ('xess', 'Intel XeSS (Native AA, any GPU)')
AA_SHARPNESS = [('0', 'Off'), ('0.2', 'Low'), ('0.4', 'Medium'), ('0.7', 'High')]
# Frames generated between two of the game's (BB_FRAME_GEN, BB_FRAME_GEN_MULTIPLIER).
FRAME_GENERATION = [('off', 'Off'), ('fsr3', 'AMD FSR 3 Frame Generation (any GPU)'),
                    ('dlss', 'NVIDIA DLSS Frame Generation (GeForce RTX 40/50)')]
BUTTON_PROMPTS = [('auto', 'Automatic (follows the device you used last)'),
                  ('keyboard', 'Keyboard and mouse keys'), ('xbox', 'Xbox buttons'),
                  ('0', "PlayStation buttons (the game's own)")]
FRAME_GEN_MULTIPLIERS = [('2', '2× (one generated frame)'), ('3', '3× (DLSS on GeForce RTX 50)'),
                         ('4', '4× (DLSS on GeForce RTX 50)')]
# Controls (input.ini, read by the game: src/runtime_pad.c). Actions with their PS4 names, the
# default keys ("A|B": two bindings) and the default controller buttons (SDL names).
BUTTON_ACTIONS = [
    ('cross', 'Cross ✕', 'Space', 'a'), ('circle', 'Circle ○', 'C', 'b'),
    ('square', 'Square □', 'F', 'x'), ('triangle', 'Triangle △', 'E', 'y'),
    ('l1', 'L1', 'Q', 'leftshoulder'), ('r1', 'R1', 'G', 'rightshoulder'),
    ('l2', 'L2 (aim)', 'Mouse Right', 'lefttrigger'), ('r2', 'R2 (fire)', 'Mouse Left', 'righttrigger'),
    ('l3', 'L3', 'Left Shift', 'leftstick'), ('r3', 'R3', 'V', 'rightstick'),
    ('options', 'Options', 'Escape|Return', 'start'), ('touchpad', 'Touchpad', 'Tab', 'back'),
    ('up', 'D-pad up', 'Up', 'dpup'), ('down', 'D-pad down', 'Down', 'dpdown'),
    ('left', 'D-pad left', 'Left|Wheel Up', 'dpleft'), ('right', 'D-pad right', 'Right|Wheel Down', 'dpright'),
]
STICK_ACTIONS = [
    ('ls_up', 'Move forward', 'W'), ('ls_down', 'Move back', 'S'),
    ('ls_left', 'Move left', 'A'), ('ls_right', 'Move right', 'D'),
    ('rs_up', 'Look up', 'I'), ('rs_down', 'Look down', 'K'),
    ('rs_left', 'Look left', 'J'), ('rs_right', 'Look right', 'L'),
]
DEFAULT_KEYS = {a: k for a, _t, k, _p in BUTTON_ACTIONS} | {a: k for a, _t, k in STICK_ACTIONS}
DEFAULT_PAD = {a: p for a, _t, _k, p in BUTTON_ACTIONS}
PAD_BUTTONS = [('a', 'A / Cross'), ('b', 'B / Circle'), ('x', 'X / Square'), ('y', 'Y / Triangle'),
               ('leftshoulder', 'LB / L1'), ('rightshoulder', 'RB / R1'),
               ('lefttrigger', 'LT / L2'), ('righttrigger', 'RT / R2'),
               ('leftstick', 'Left stick click / L3'), ('rightstick', 'Right stick click / R3'),
               ('start', 'Start / Options'), ('back', 'Back / Share'), ('touchpad', 'Touchpad click'),
               ('dpup', 'D-pad up'), ('dpdown', 'D-pad down'), ('dpleft', 'D-pad left'),
               ('dpright', 'D-pad right'), ('misc1', 'Share / Mic (misc)'), ('paddle1', 'Paddle 1'),
               ('paddle2', 'Paddle 2'), ('paddle3', 'Paddle 3'), ('paddle4', 'Paddle 4'),
               ('none', 'Not assigned')]
MOUSE_SENSITIVITY = [('0.25', '0.25'), ('0.5', '0.5'), ('0.75', '0.75'), ('1.0', '1.0 (default)'),
                     ('1.25', '1.25'), ('1.5', '1.5'), ('2.0', '2.0'), ('2.5', '2.5'), ('3.0', '3.0'),
                     ('4.0', '4.0')]
# Tk key names → SDL key names (letters and digits map to themselves, upper case).
TK_KEYS = {'space': 'Space', 'Return': 'Return', 'Escape': 'Escape', 'Tab': 'Tab', 'BackSpace': 'Backspace',
           'Shift_L': 'Left Shift', 'Shift_R': 'Right Shift', 'Control_L': 'Left Ctrl',
           'Control_R': 'Right Ctrl', 'Alt_R': 'Right Alt', 'Caps_Lock': 'CapsLock', 'Up': 'Up',
           'Down': 'Down', 'Left': 'Left', 'Right': 'Right', 'Delete': 'Delete', 'Home': 'Home',
           'End': 'End', 'Prior': 'PageUp', 'Next': 'PageDown', 'minus': '-', 'equal': '=',
           'bracketleft': '[', 'bracketright': ']', 'semicolon': ';', 'apostrophe': "'", 'comma': ',',
           'period': '.', 'slash': '/', 'backslash': '\\', 'grave': '`', 'KP_Enter': 'Keypad Enter',
           'KP_Add': 'Keypad +', 'KP_Subtract': 'Keypad -', 'KP_Multiply': 'Keypad *',
           'KP_Divide': 'Keypad /', 'KP_Decimal': 'Keypad .', 'KP_Delete': 'Keypad .',
           'KP_Insert': 'Keypad 0', 'KP_End': 'Keypad 1', 'KP_Down': 'Keypad 2', 'KP_Next': 'Keypad 3',
           'KP_Left': 'Keypad 4', 'KP_Begin': 'Keypad 5', 'KP_Right': 'Keypad 6', 'KP_Home': 'Keypad 7',
           'KP_Up': 'Keypad 8', 'KP_Prior': 'Keypad 9'}
# Tk on Windows names numpad keys like the main ones ("8", "plus"), only the keycode tells them apart (#224).
WIN_NUMPAD = {**{0x60 + n: f'Keypad {n}' for n in range(10)},
              0x6A: 'Keypad *', 0x6B: 'Keypad +', 0x6D: 'Keypad -', 0x6E: 'Keypad .', 0x6F: 'Keypad /'}
MOUSE_SENS_MIN, MOUSE_SENS_MAX = 0.1, 10.0  # the Controls page slider (the game's default pace is 1)
# Fixes page: fixes that cost speed, so they run only where they help unless the player picks.
# key -> (title, hint, {choice: {variable: value}}); 'auto' sets nothing (the game's own default).
FIXES = {
    'fix_u2_gpu_results': (
        'Exact GPU results (Uncharted 2)',
        'Explosions, vehicle damage and the convoy of chapter 21 read results of GPU work back. '
        'Automatic: on in Uncharted 2 only. Costs speed in busy scenes.',
        {'on': {'BB_GPU_MERGE': '0'}, 'off': {'BB_GPU_MERGE': '1'}}),
    'fix_u2_mesh': (
        'Mesh raycaster copies (Uncharted 2)',
        'Fixes "iSubMesh < m_numSubMeshInstances" after checkpoints. Automatic: on in Uncharted 2 only. '
        'In Uncharted 3\'s Cartagena square it cost 94 to 54 FPS.',
        {'on': {'BB_SCALAR_COPY64_SYNC': '2', 'BB_SCALAR_COPY64_DEFER': '1'},
         'off': {'BB_SCALAR_COPY64_SYNC': '0', 'BB_SCALAR_COPY64_DEFER': '0'}}),
    'fix_tiny_readback': (
        'Aim and turret focus (Uncharted 2)',
        'Enemies stay in focus when aiming. Automatic: on in Uncharted 2 only; elsewhere it costs about 13%.',
        {'on': {'BB_TINY_READBACK': '1'}, 'off': {'BB_TINY_READBACK': '0'}}),
    'fix_small_buffers': (
        'Brightness flicker fix',
        'Copies small GPU results back every frame (auto exposure). Automatic: off; it halved the frame '
        'rate in Uncharted 3\'s Cartagena square.',
        {'on': {'BB_READBACK_SMALL_BUFFERS': '1'}, 'off': {'BB_READBACK_SMALL_BUFFERS': '0'}}),
    'fix_vram': (
        'Small video memory mode',
        'Keeps 4-8 GB graphics cards inside their video memory. Automatic: on for cards up to 8 GB.',
        {'on': {'BB_VRAM_LEGACY': '0'}, 'off': {'BB_VRAM_LEGACY': '1'}}),
    'fix_job_sleep': (
        'Idle game workers sleep',
        'Frees CPU cores while the game\'s workers wait. Automatic: on for CPUs with up to 8 threads; '
        'on bigger CPUs it cost about 8%.',
        {'on': {'BB_JOB_SLEEP': '1'}, 'off': {'BB_JOB_SLEEP': '0'}}),
    'fix_thread_prio': (
        'PS4 thread priorities',
        'Game threads get the priorities they have on PS4. Automatic: on.',
        {'on': {'BB_THREAD_PRIO': '1'}, 'off': {'BB_THREAD_PRIO': '0'}}),
}
FIX_CHOICES = [('auto', 'Automatic'), ('on', 'On'), ('off', 'Off')]

# Insert opens the in-game settings menu; Left Alt frees the mouse cursor.
RESERVED_KEYS = {'Insert': 'Insert opens the in-game settings menu.',
                 'Alt_L': 'Left Alt frees the mouse cursor while held.'}

APP_DEFAULTS = {'game_dir': '', 'game_dir_u1': '', 'game_dir_u2': '', 'game_dir_u3': '', 'user_dir': '', 'start': 'u1', 'language': '1', 'ui_language': '', 'player_name': '',
                'fullscreen': False, 'present_mode': 'Mailbox', 'frame_rate': '60', 'show_fps': False,
                'window_size': '1920x1080', 'render_height': '1080', 'aspect': '16:9', 'speakers': 'auto', 'scaler': 'fsr1', 'sharpening': '500',
                'anti_aliasing': 'off', 'aa_sharpness': '0.2', 'frame_generation': 'off',
                'frame_gen_multiplier': '2', 'extra_env': '', 'close_on_play': False,
                'mouse_look': True, 'mouse_sensitivity': '1.0', 'mouse_invert_y': False,
                'pad_invert_y': False, 'art_banner': '', 'art_u1': '', 'art_u2': '', 'art_u3': '', 'steam_art': '', 'art_crops': {}, 'shortcut_name': 'Uncharted: The Nathan Drake Collection', 'shortcut_icon': '', 'trophy_game': 'u1', 'pad_vibration': True, 'pad_vibration_strength': '100', 'button_prompts': 'auto', 'key_bindings': {}, 'pad_bindings': {}, 'check_updates': True,
                **{key: 'auto' for key in FIXES},
                **{f'{game}_infinite_{cheat}': False for game, _t, _s, _e in GAMES for cheat in ('health', 'ammo')}}


UI_LANGUAGE = 'en'


def _(text):
    """Translate launcher text, keeping English as the fallback."""
    return uncharted_i18n.translate(UI_LANGUAGE, text)


def _run_problem_priority(line):
    if line.startswith(('STOP:', 'GPU crash:')):
        return 3
    if line.startswith(('ERROR', 'Fault:', 'No eboot.bin', 'No big', 'Set BB_GAME_DIR',
                        'bb-probe.exe not found')):
        return 2
    if 'failed' in line and not line.startswith('GPU ['):
        return 1
    return 0


def select_run_problem(current, output):
    """Keep the most useful diagnostic emitted before the game exits."""
    line = output.strip()
    line_priority = _run_problem_priority(line)
    return line if line_priority and line_priority >= _run_problem_priority(current) else current


# ---------------------------------------------------------------------------------------------
# Command line roles of the frozen executable.

def attach_stdio():
    """A windowed executable starts without sys.stdout; inherited pipes or a console still exist."""
    import msvcrt
    for name, std in (('stdout', -11), ('stderr', -12)):
        if getattr(sys, name) is not None:
            continue
        stream = None
        handle = ctypes.windll.kernel32.GetStdHandle(std)
        if handle and handle != ctypes.c_void_p(-1).value:
            try:
                stream = open(msvcrt.open_osfhandle(handle, os.O_WRONLY), 'w', encoding='utf-8',
                              errors='replace', buffering=1)
            except OSError:
                stream = None
        setattr(sys, name, stream or open(os.devnull, 'w'))


def run_role(argv):
    """--run [args]: run.py; --script <file> [args]: a preparation script. Returns an exit code."""
    attach_stdio()
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(line_buffering=True, errors='replace', **({} if stream.isatty() else {'encoding': 'utf-8'}))
        except (AttributeError, ValueError):
            pass
    if argv[0] == '--script':
        path, sys.argv = argv[1], argv[1:]
    else:
        path = str(PORT_DIR / 'run.py')
        sys.argv = [path, *argv[1:]]
    try:
        runpy.run_path(path, run_name='__main__')
    except SystemExit as stop:
        return stop.code if isinstance(stop.code, int) else (0 if stop.code is None else 1)
    return 0


def run_command():
    return [sys.executable, '--run'] if FROZEN else [sys.executable, str(PORT_DIR / 'run.py')]


# ---------------------------------------------------------------------------------------------
# Settings.

def load_json(path, default):
    try:
        return json.loads(Path(path).read_text(encoding='utf-8-sig'))
    except (OSError, ValueError):
        return default


def ini_path():
    return Path(os.environ.get('BB_CONFIG', DATA_DIR / 'bbport.ini'))


def set_ini(key, value):
    """bbport.ini (read by the game; the in-game menu edits it): one key rewritten in place."""
    path = ini_path()
    try:
        lines = path.read_text(encoding='utf-8').splitlines()
    except OSError:
        lines = []
    out, found = [], False
    for line in lines:
        if '=' in line and not line.lstrip().startswith('#') and line.split('=', 1)[0].strip() == key:
            out.append(f'{key}={value}')
            found = True
        else:
            out.append(line)
    if not found:
        out.append(f'{key}={value}')
    path.write_text('\n'.join(out) + '\n', encoding='utf-8')


def game_info(folder):
    """{'title', 'id', 'version', 'games': [keys present]} of a game folder, or None."""
    folder = Path(folder or '.')
    if not any((folder / exe).is_file() for _k, _t, _s, exe in GAMES):
        return None
    info = {'title': 'Uncharted', 'id': folder.name, 'version': '?',
            'games': [key for key, _n, _s, exe in GAMES if (folder / exe).is_file()]}
    try:
        from prepare import sfo
        values = sfo((folder / 'sce_sys/param.sfo').read_bytes())
        info.update(title=values.get('TITLE', info['title']).replace('™', '').strip(),
                    id=values.get('TITLE_ID', info['id']), version=values.get('APP_VER', '?'))
    except (OSError, ValueError, ImportError):
        pass
    return info


def game_folder(s, key):
    """Where one game is: its own folder (Game page, for a game dumped on its own), else the collection folder."""
    return str(s.get(f'game_dir_{key}') or '').strip() or str(s.get('game_dir') or '').strip()


def game_available(s, key):
    """The game's executable is in its folder (a game dumped on its own may have it as eboot.bin)."""
    folder = game_folder(s, key)
    if not folder:
        return False
    exe = next(e for k, _t, _s, e in GAMES if k == key)
    own = str(s.get(f'game_dir_{key}') or '').strip()
    return (Path(folder) / exe).is_file() or bool(own and (Path(folder) / 'eboot.bin').is_file())


def mouse_sensitivity(s):
    """The saved sensitivity as the game reads it: a number in the slider's range."""
    try:
        return f"{min(MOUSE_SENS_MAX, max(MOUSE_SENS_MIN, float(s.get('mouse_sensitivity')))):.2f}"
    except (TypeError, ValueError):
        return '1.00'


def vibration_strength(s):
    """Saved vibration percentage, clamped at the launcher/runtime boundary."""
    try:
        return min(100, max(0, int(float(s.get('pad_vibration_strength', 100)))))
    except (TypeError, ValueError):
        return 100


def controller_probe_path():
    """The packaged or development loader, which also provides the lightweight controller probe."""
    configured = os.environ.get('BB_PROBE')
    candidates = ([Path(configured)] if configured else []) + [PORT_DIR / 'bin' / 'bb-probe.exe',
                                                               PORT_DIR / 'out' / 'bb-probe.exe']
    return next((path for path in candidates if path.is_file()), None)


def controller_probe_environment(probe):
    """Development builds find SDL beside MSYS2; packages already keep it beside bb-probe."""
    env = dict(os.environ)
    if not (probe.parent / 'SDL3.dll').is_file():
        clang64 = Path(env.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
        if clang64.is_dir():
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'
    return env


def parse_controller_probe(text):
    """Parse bb-probe's deliberately small key=value controller response."""
    values = {}
    for line in text.splitlines():
        if '=' in line:
            key, value = line.split('=', 1)
            values[key.strip()] = value.strip()
    return {'connected': values.get('connected') == '1',
            'name': values.get('name', ''),
            'rumble': values.get('rumble') == '1',
            'tested': values.get('tested') == '1'}


def side_buttons():
    """(Mouse 4, Mouse 5) held right now: the system's XBUTTON1 / XBUTTON2 (virtual keys 5 and 6)."""
    try:
        return tuple(bool(ctypes.windll.user32.GetAsyncKeyState(vk) & 0x8000) for vk in (5, 6))
    except (AttributeError, OSError):
        return (False, False)


def display_refresh_rate():
    """Refresh rate of the main display in Hz, or None."""
    try:
        class DevMode(ctypes.Structure):
            _fields_ = [('name', ctypes.c_wchar * 32), ('spec', ctypes.c_ushort), ('driver', ctypes.c_ushort),
                        ('size', ctypes.c_ushort), ('extra', ctypes.c_ushort), ('fields', ctypes.c_ulong),
                        ('position', ctypes.c_long * 2), ('orientation', ctypes.c_ulong),
                        ('fixed', ctypes.c_ulong), ('color', ctypes.c_short * 5), ('form', ctypes.c_wchar * 32),
                        ('logpixels', ctypes.c_ushort), ('bits', ctypes.c_ulong), ('width', ctypes.c_ulong),
                        ('height', ctypes.c_ulong), ('flags', ctypes.c_ulong), ('frequency', ctypes.c_ulong),
                        ('rest', ctypes.c_ulong * 8)]
        mode = DevMode()
        mode.size = ctypes.sizeof(DevMode)
        if ctypes.windll.user32.EnumDisplaySettingsW(None, -1, ctypes.byref(mode)) and mode.frequency > 1:
            return int(mode.frequency)
    except (AttributeError, OSError):
        pass
    return None


def fsr411_assets():
    """FSR 4.1.1 needs its model assets, built from AMD's DLL (docs/upscaler.md)."""
    return bool(os.environ.get('BB_FSR411_DIR')) or any(
        (folder / 'fsr4_411').is_dir() for folder in (PORT_DIR, DATA_DIR))


def xess_available():
    """Intel XeSS needs Intel's libxess.dll next to bb-probe.exe (packaging/windows/fetch_xess.sh)."""
    override = os.environ.get('BB_XESS_DLL')
    if override:
        return Path(override).is_file()
    return any((PORT_DIR / folder / 'libxess.dll').is_file() for folder in ('bin', 'out'))


def input_ini_path():
    return Path(os.environ.get('BB_INPUT_CONFIG', DATA_DIR / 'input.ini'))


def write_input_ini(s):
    """input.ini: the keyboard, mouse and controller bindings the game reads at start."""
    keys = {**DEFAULT_KEYS, **(s.get('key_bindings') or {})}
    pad = {**DEFAULT_PAD, **(s.get('pad_bindings') or {})}
    lines = ['# Uncharted controls (written by the launcher, Controls page)',
             f"mouse_look={1 if s.get('mouse_look', True) else 0}",
             f"mouse_sensitivity={mouse_sensitivity(s)}",
             f"mouse_invert_y={1 if s.get('mouse_invert_y') else 0}",
             f"pad_invert_y={1 if s.get('pad_invert_y') else 0}",
             f"pad_vibration={1 if s.get('pad_vibration', True) else 0}",
             f"pad_vibration_strength={vibration_strength(s)}"]
    lines += [f'key.{action}={keys.get(action, "")}' for action in DEFAULT_KEYS]
    lines += [f'pad.{action}={pad.get(action, "none")}' for action in DEFAULT_PAD]
    path = input_ini_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return path


def screen_size():
    """(width, height) of the main display in pixels, or None."""
    try:
        user32 = ctypes.windll.user32
        return int(user32.GetSystemMetrics(0)), int(user32.GetSystemMetrics(1))
    except (AttributeError, OSError):
        return None


def parse_size(text):
    try:
        width, height = (int(part) for part in str(text).lower().replace('×', 'x').split('x'))
    except ValueError:
        return None
    return (width, height) if width > 0 and height > 0 else None


def game_resolution(s, screen=None):
    """The size the games render at, 'WxH' (even numbers, the pixel count within what their GPU memory holds), or ''
    for their own 1920x1080."""
    height = int(s.get('render_height') or 1080) if str(s.get('render_height') or '1080').isdigit() else 1080
    shape = s.get('aspect') or '16:9'
    if shape == 'window':
        size = screen if s.get('fullscreen') else parse_size(s.get('window_size'))
        size = size or screen or (1920, 1080)
        ratio = size[0] / size[1]
    else:
        ratio = ASPECT_RATIOS.get(shape, 16 / 9)
    ratio = min(max(ratio, 1.0), 4.0)
    for height in sorted({height, 2160, 1800, 1620, 1440, 1260, 1080, 900, 720}, reverse=True):
        if height > int(s.get('render_height') or 1080) if str(s.get('render_height') or '1080').isdigit() else height > 1080:
            continue
        width = SCREEN_WIDTHS.get((shape, height)) or int(round(height * ratio / 2)) * 2
        if width * height <= MAX_RENDER_PIXELS:   # the next smaller height of the same shape
            break
    return '' if (width, height) == (1920, 1080) else f'{width}x{height}'


def game_environment(s, start=None):
    env = dict(os.environ)
    env['BB_INPUT_CONFIG'] = str(write_input_ini(s))
    start = start or s['start']
    # The collection folder; with none, the folder of the game started. A game with a folder of its own
    # (Game page) is taken from it, also when another game of the collection starts it.
    env['BB_GAME_DIR'] = s['game_dir'] or game_folder(s, start)
    for key, _t, _s, _e in GAMES:
        if str(s.get(f'game_dir_{key}') or '').strip():
            env[f'BB_GAME_DIR_{key.upper()}'] = game_folder(s, key)
    env['BB_EXE'] = start
    if s['user_dir']:
        env['BB_USER_DIR'] = s['user_dir']
    env['BB_LANGUAGE'] = s['language']
    if str(s['player_name']).strip():
        env['BB_USER_NAME'] = str(s['player_name']).strip()
    env['BB_FULLSCREEN'] = '1' if s['fullscreen'] else '0'
    env['BB_PRESENT_MODE'] = s['present_mode']
    env['BB_VBLANK_HZ'] = str(s.get('frame_rate') or '60')
    env['BB_WINDOW_SIZE'] = str(s.get('window_size') or '1920x1080')
    if str(s.get('speakers') or 'auto') != 'auto':
        env['BB_SPEAKERS'] = str(s['speakers'])
    resolution = game_resolution(s, screen_size())
    if resolution:
        env['BB_GAME_RES'] = resolution
    window = parse_size(s.get('window_size'))
    if not s.get('fullscreen') and resolution and window and s.get('aspect', '16:9') != '16:9':
        shape = parse_size(resolution)
        # A window of another shape than the picture would show bars: keep its height, take the picture's width.
        env['BB_WINDOW_SIZE'] = f'{round(window[1] * shape[0] / shape[1] / 2) * 2}x{window[1]}'
    env['BB_FSR1'] = '1' if s.get('scaler', 'fsr1') == 'fsr1' else '0'
    sharpening = str(s.get('sharpening') or 'off')
    env['BB_RCAS'] = '0' if sharpening == 'off' else '1'
    if sharpening != 'off':
        env['BB_RCAS_ATTENUATION'] = sharpening
    env['BB_UPSCALER'] = str(s.get('anti_aliasing') or 'off')
    env['BB_FSR_SHARPNESS'] = str(s.get('aa_sharpness') or '0')
    env['BB_FRAME_GEN'] = str(s.get('frame_generation') or 'off')
    env['BB_FRAME_GEN_MULTIPLIER'] = str(s.get('frame_gen_multiplier') or '2')
    for key, (_title, _hint, choices) in FIXES.items():
        env.update(choices.get(str(s.get(key) or 'auto'), {}))
    env['BB_BUTTON_PROMPTS'] = str(s.get('button_prompts') or 'auto')
    env['BB_CHEATS'] = ','.join(f'{game}:{cheat}' for game, _t, _s, _e in GAMES for cheat in ('health', 'ammo')
                                if s.get(f'{game}_infinite_{cheat}'))
    for item in str(s['extra_env']).split():
        if '=' in item:
            key, value = item.split('=', 1)
            env[key] = value
    env['PYTHONUNBUFFERED'] = '1'
    env['PYTHONIOENCODING'] = 'utf-8'
    env['PYTHONUTF8'] = '1'  # open() and the pipes must not fall back to the ANSI code page (GBK, ...)
    return env


# ---------------------------------------------------------------------------------------------
# Surfaces: near-black base, raised panels, Uncharted gold accent.

BG, PANEL, SURFACE, CARD, LINE = '#08090a', '#0c0d0f', '#0f1012', '#17181b', '#26282c'
TEXT, MUTED, GOLD, GOLD_HI, GOLD_DIM = '#f3efe6', '#8e8a82', '#e7c36a', '#f3d78a', '#a88942'
CARD_HI = '#202226'
COLORS = {'surface': SURFACE, 'card': CARD, 'card_hi': CARD_HI, 'line': LINE, 'text': TEXT, 'muted': MUTED,
          'gold': GOLD, 'gold_hi': GOLD_HI, 'track': '#2c2e33', 'knob': '#9a958b', 'knob_on': '#f7f1e2',
          'button': '#25272b', 'button_hi': '#30333a'}
# Sidebar icons (Segoe Fluent Icons on Windows 11, Segoe MDL2 Assets on Windows 10).
NAV_ICONS = {'play': '\ue768', 'game': '\ue7fc', 'display': '\ue7f4', 'controls': '\ue765',
             'cheats': '\ue945', 'fixes': '\ue90f', 'trophies': '\ue734', 'advanced': '\ue713', 'log': '\ue8a5'}
# Trophy grades as the console colours them (the platinum cool white-blue, the rest metal).
TROPHY_COLORS = {'P': '#9fd0f5', 'G': GOLD, 'S': '#cfd3da', 'B': '#cf8548'}
RUST, RUST_HI = GOLD, GOLD_HI


class Launcher:
    def __init__(self, root, tk, ttk, filedialog, messagebox):
        global UI_LANGUAGE
        self.tk, self.ttk, self.filedialog, self.messagebox = tk, ttk, filedialog, messagebox
        self.root = root
        self.app = {**APP_DEFAULTS, **load_json(CONFIG_FILE, {})}
        UI_LANGUAGE = uncharted_i18n.language_code(self.app.get('ui_language', ''))
        self.configure_art()
        self.vars = {}
        self.process = self.job = None
        self.entered, self.last_problem, self.stopping = False, '', False
        self.output = queue.Queue()
        self.ui_calls = queue.Queue()
        self.banner_key = self.banner_scaled = self.cover_image = None
        self.card_hot = None
        self.info, self.available = None, {}
        self.update_box = None
        root.title(f'{NAME} — The Nathan Drake Collection for Windows · by {AUTHOR}')
        root.configure(bg=BG)
        self.dpi = root.winfo_fpixels('1i') / 96.0
        root.geometry(f'{self.px(1180)}x{self.px(860)}')
        root.minsize(self.px(1040), self.px(760))
        self.set_icon()
        self.load_fonts()
        self.style()
        self.build()
        self.show('play')
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(100, self.drain_output)
        root.after(150, self.detect_controller)
        if self.app.get('check_updates', True):
            threading.Thread(target=self.check_update, daemon=True).start()

    def px(self, size):
        return int(size * self.dpi)

    def set_icon(self):
        try:
            self.root.iconbitmap(default=str(PORT_DIR / 'launcher' / 'uncharted.ico'))
        except self.tk.TclError:
            pass

    def load_fonts(self):
        """Cinzel for headings (Uncharted-style capitals); Segoe UI for body text."""
        from uncharted_fonts import font_path

        self.brand_family = 'Cinzel'
        for name in ('Cinzel-Regular.ttf', 'Cinzel-SemiBold.ttf', 'Cinzel-Bold.ttf'):
            path = font_path(name)
            if path:
                try:
                    ctypes.windll.gdi32.AddFontResourceExW(str(path), 0x10, 0)
                except (AttributeError, OSError):
                    pass
        if not font_path('Cinzel-Bold.ttf'):
            self.brand_family = 'Georgia'
        self.font_brand_xl = (self.brand_family, 24, 'bold')
        self.font_brand_lg = (self.brand_family, 18, 'bold')
        self.font_brand_md = (self.brand_family, 13, 'bold')
        self.font_brand_sm = (self.brand_family, 10)
        self.font_ui = ('Segoe UI', 10)
        self.font_ui_sm = ('Segoe UI', 9)
        self.font_ui_md = ('Segoe UI Semibold', 10)
        self.font_button = ('Segoe UI Semibold', 10)
        self.font_play = ('Segoe UI Semibold', 13)
        self.font_title = ('Segoe UI Semibold', 22)
        self.font_card = ('Segoe UI Semibold', 9)
        import tkinter.font as tkfont
        families = set(tkfont.families(self.root))
        icons = next((f for f in ('Segoe Fluent Icons', 'Segoe MDL2 Assets') if f in families), None)
        self.font_icon = (icons, 13) if icons else None
        self.font_icon_sm = (icons, 11) if icons else None

    # ---- look --------------------------------------------------------------------------------
    def check_images(self):
        """16 px (DPI-scaled) check box images: the clam theme draws a cross."""
        tk = self.tk
        n = max(14, int(16 * self.dpi))
        images = []
        for checked in (False, True):
            image = tk.PhotoImage(width=n + self.px(8), height=n)
            image.put(GOLD if checked else '#6a5c40', to=(0, 0, n, n))
            image.put('#1a1408' if checked else CARD, to=(1, 1, n - 1, n - 1))
            if checked:
                x0, y0, x1, y1, x2, y2 = .22 * n, .52 * n, .42 * n, .72 * n, .78 * n, .28 * n
                width = max(1, round(n / 9))
                for t in range(40):
                    f = t / 39
                    for x, y in ((x0 + (x1 - x0) * f, y0 + (y1 - y0) * f), (x1 + (x2 - x1) * f, y1 + (y2 - y1) * f)):
                        image.put(GOLD, to=(int(x), int(y), int(x) + width, int(y) + width))
            images.append(image)
        self.check_off, self.check_on = images

    def style(self):
        s = self.ttk.Style(self.root)
        s.theme_use('clam')
        self.check_images()
        s.element_create('Nd.indicator', 'image', self.check_off, ('selected', self.check_on), sticky='')
        s.layout('TCheckbutton', [('Checkbutton.padding', {'sticky': 'nswe', 'children': [
            ('Nd.indicator', {'side': 'left', 'sticky': ''}),
            ('Checkbutton.focus', {'side': 'left', 'sticky': 'w', 'children': [
                ('Checkbutton.label', {'sticky': 'nswe'})]})]})])
        base = self.font_ui
        self.root.option_add('*TCombobox*Listbox.background', CARD)
        self.root.option_add('*TCombobox*Listbox.foreground', TEXT)
        self.root.option_add('*TCombobox*Listbox.selectBackground', RUST)
        self.root.option_add('*TCombobox*Listbox.font', base)
        s.configure('.', background=SURFACE, foreground=TEXT, fieldbackground=CARD, bordercolor=LINE,
                    lightcolor=LINE, darkcolor=LINE, troughcolor=CARD, focuscolor=GOLD, font=base)
        s.configure('TFrame', background=SURFACE)
        s.configure('Shell.TFrame', background=SURFACE)
        s.configure('Card.TFrame', background=CARD)
        s.configure('TLabel', background=CARD, foreground=TEXT)
        s.configure('Shell.TLabel', background=SURFACE, foreground=TEXT)
        s.configure('Muted.TLabel', background=CARD, foreground=MUTED, font=('Segoe UI', 9))
        s.configure('ShellMuted.TLabel', background=SURFACE, foreground=MUTED, font=('Segoe UI', 9))
        s.configure('Section.TLabel', background=SURFACE, foreground=GOLD_DIM,
                    font=(self.brand_family, 10, 'bold'))
        s.configure('TCheckbutton', background=SURFACE, foreground=TEXT, padding=(0, 3))
        s.map('TCheckbutton', background=[('active', SURFACE)], foreground=[('disabled', MUTED)])
        s.configure('Ghost.TButton', background=CARD, foreground=MUTED, padding=(16, 8), borderwidth=0)
        s.map('Ghost.TButton', background=[('active', CARD)], foreground=[('active', TEXT)])
        s.configure('TCombobox', arrowcolor=GOLD, foreground=TEXT, padding=(8, 5), bordercolor=CARD_HI,
                    lightcolor=CARD_HI, darkcolor=CARD_HI, background=CARD_HI, arrowsize=14)
        s.map('TCombobox', fieldbackground=[('readonly', CARD_HI)], foreground=[('readonly', TEXT)],
              selectbackground=[('readonly', CARD_HI)], selectforeground=[('readonly', TEXT)],
              bordercolor=[('focus', GOLD_DIM)], lightcolor=[('focus', GOLD_DIM)], background=[('active', '#2a2c31')])
        s.configure('TEntry', foreground=TEXT, insertcolor=TEXT, padding=(8, 5), fieldbackground=CARD_HI,
                    bordercolor=CARD_HI, lightcolor=CARD_HI, darkcolor=CARD_HI)
        s.map('TEntry', bordercolor=[('focus', GOLD_DIM)], lightcolor=[('focus', GOLD_DIM)])
        s.configure('TButton', background='#221f1a', foreground=TEXT, padding=(14, 7), borderwidth=0)
        s.map('TButton', background=[('active', '#322c24'), ('disabled', PANEL)], foreground=[('disabled', MUTED)])
        s.configure('Play.TButton', background=GOLD, foreground='#1a1408', font=('Segoe UI', 13, 'bold'),
                    padding=(42, 12), borderwidth=0)
        s.map('Play.TButton', background=[('active', '#f3d78a'), ('disabled', '#3a3428')],
              foreground=[('disabled', '#8a8070'), ('active', '#1a1408')])
        s.configure('Vertical.TScrollbar', background='#2a2c31', arrowcolor=MUTED, troughcolor=SURFACE,
                    bordercolor=SURFACE, lightcolor='#2a2c31', darkcolor='#2a2c31', gripcount=0, arrowsize=12)
        s.map('Vertical.TScrollbar', background=[('active', '#3a3d44')])

    # ---- widget helpers ----------------------------------------------------------------------
    def var(self, key):
        if key not in self.vars:
            value = self.app.get(key, APP_DEFAULTS[key])
            v = self.tk.BooleanVar(value=bool(value)) if isinstance(APP_DEFAULTS[key], bool) \
                else self.tk.StringVar(value=str(value))
            self.vars[key] = v
        return self.vars[key]

    def choice(self, parent, key, options, width=36, on_change=None):
        var = self.var(key)
        values = [v for v, _t in options]
        box = self.ttk.Combobox(parent, values=[_(t) for _v, t in options], state='readonly', width=width)
        if var.get() not in values:
            var.set(values[0])

        def show(*_args):
            box.current(values.index(var.get()) if var.get() in values else 0)
        show()
        def selected(_event):
            var.set(values[box.current()])
            if on_change:
                on_change()
        box.bind('<<ComboboxSelected>>', selected)
        var.trace_add('write', show)
        return box

    def next_row(self, parent):
        return parent.grid_size()[1]

    def target(self, parent):
        """Where a page's rows go: its current card (one is opened when rows come before a section)."""
        if parent in self.page_cards:
            if self.page_cards[parent] is None:
                self.new_card(parent, None)
            self.page_cards[parent].refit()
            return self.page_cards[parent].inner
        return parent

    def new_card(self, page, title):
        from uncharted_widgets import Card
        card = Card(self.tk, page, COLORS, self.px, title, self.font_card)
        card.canvas.grid(row=self.next_row(page), column=0, columnspan=2, sticky='ew', pady=(0, self.px(14)))
        self.page_cards[page] = card
        return card

    def toggle(self, parent, variable, background=CARD):
        from uncharted_widgets import Toggle
        return Toggle(self.tk, parent, variable, COLORS, self.px, background)

    def pill(self, parent, text, command, kind='secondary', background=CARD, padding=(16, 8), icon=None):
        from uncharted_widgets import PillButton
        return PillButton(self.tk, parent, text, command, COLORS, self.px,
                          self.font_play if kind == 'primary' and icon else self.font_button, kind,
                          background, padding, icon, self.font_icon_sm)

    def row(self, parent, title, widget, hint=None):
        parent = self.target(parent)
        r = self.next_row(parent)
        self.ttk.Label(parent, text=title).grid(row=r, column=0, sticky='nw', padx=(0, 24), pady=(9, 0))
        widget.grid(row=r, column=1, sticky='w', pady=(5, 0), in_=parent)
        widget.lift()
        if hint:
            self.ttk.Label(parent, text=hint, style='Muted.TLabel', wraplength=self.px(560), justify='left').grid(
                row=r + 1, column=1, sticky='w', pady=(3, 4))
        return widget

    def check(self, parent, key, title, hint=None):
        parent = self.target(parent)
        r = self.next_row(parent)
        line = self.tk.Frame(parent, bg=CARD)
        line.grid(row=r, column=0, columnspan=2, sticky='w', pady=(7, 0))
        self.toggle(line, self.var(key)).pack(side='left')
        label = self.tk.Label(line, text=title, bg=CARD, fg=TEXT, font=self.font_ui, cursor='hand2')
        label.pack(side='left', padx=(self.px(12), 0))
        label.bind('<Button-1>', lambda _e: self.var(key).set(not self.var(key).get()))
        if hint:
            self.ttk.Label(parent, text=hint, style='Muted.TLabel', wraplength=self.px(640), justify='left').grid(
                row=r + 1, column=0, columnspan=2, sticky='w', padx=(self.px(52), 0), pady=(2, 2))

    def note(self, parent, text, top=12):
        parent = self.target(parent)
        self.ttk.Label(parent, text=text, style='Muted.TLabel', wraplength=self.px(680), justify='left').grid(
            row=self.next_row(parent), column=0, columnspan=2, sticky='w', pady=(top, 0))

    def section(self, parent, title, top=18):
        if parent in self.page_cards:
            return self.new_card(parent, title).inner
        self.ttk.Label(parent, text=title, style='Section.TLabel').grid(
            row=self.next_row(parent), column=0, columnspan=2, sticky='w', pady=(top, 2))
        return parent

    def folder(self, parent, key, title, prompt, hint=None, on_change=None):
        var = self.var(key)
        holder = self.tk.Frame(self.target(parent), bg=CARD)
        self.ttk.Entry(holder, textvariable=var, width=54).pack(side='left')

        def browse():
            chosen = self.filedialog.askdirectory(title=prompt, initialdir=var.get() or str(PORT_DIR))
            if chosen:
                var.set(str(Path(chosen)))
        self.pill(holder, _('Browse…'), browse, padding=(14, 6)).pack(side='left', padx=(8, 0))
        self.pill(holder, _('Open'), lambda: self.open_path(var.get(), key), kind='ghost',
                  padding=(12, 6)).pack(side='left', padx=(4, 0))
        if on_change:
            var.trace_add('write', lambda *_a: on_change())
        return self.row(parent, title, holder, hint)

    def image_path(self, title, current=''):
        """Choose and validate a local image."""
        chosen = self.filedialog.askopenfilename(
            title=title, initialdir=str(Path(current).parent) if current else str(PORT_DIR),
            filetypes=[(_('Image files'), '*.png *.jpg *.jpeg *.webp *.ico'), (_('All files'), '*.*')])
        if not chosen:
            return ''
        try:
            from PIL import Image
            with Image.open(chosen) as image:
                image.verify()
        except (OSError, ValueError) as error:
            self.messagebox.showerror(NAME, _('This image could not be opened: {}').format(error))
            return ''
        return str(Path(chosen))

    def enable_image_drop(self, widget, callback):
        """Accept Explorer file drops when tkinterdnd2 is bundled; clicking always remains available."""
        try:
            from tkinterdnd2 import DND_FILES
            widget.drop_target_register(DND_FILES)
            widget.dnd_bind('<<Drop>>', lambda event: callback(self.root.tk.splitlist(event.data)[0]))
        except (ImportError, self.tk.TclError, AttributeError, IndexError):
            pass

    def edit_art(self, key, title, size, crop_key, initial_path=None, initial_crop=None):
        """Preview, position and save one local image without copying it into the project."""
        from PIL import Image, ImageTk
        import uncharted_art

        window = self.tk.Toplevel(self.root)
        window.title(title)
        window.configure(bg=PANEL)
        window.transient(self.root)
        window.grab_set()
        window.resizable(False, False)
        path = self.tk.StringVar(value=self.app.get(key, '') if initial_path is None else initial_path)
        saved = (self.app.setdefault('art_crops', {}).get(crop_key, {})
                 if initial_crop is None else initial_crop)
        result = {'value': None}
        zoom = self.tk.DoubleVar(value=float(saved.get('zoom', 1.0)))
        focus_x = self.tk.DoubleVar(value=float(saved.get('x', 0.5)))
        focus_y = self.tk.DoubleVar(value=float(saved.get('y', 0.5)))
        preview_w = self.px(560)
        preview_h = max(self.px(180), round(preview_w * size[1] / size[0]))
        if preview_h > self.px(390):
            preview_h = self.px(390)
            preview_w = round(preview_h * size[0] / size[1])
        shell = self.tk.Frame(window, bg=PANEL)
        shell.pack(padx=self.px(24), pady=self.px(22))
        self.tk.Label(shell, text=title, bg=PANEL, fg=TEXT, font=self.font_title).pack(anchor='w')
        self.tk.Label(shell, text=_('Drag an image here or click Choose image.'), bg=PANEL, fg=MUTED,
                      font=self.font_ui).pack(anchor='w', pady=(2, self.px(12)))
        canvas = self.tk.Canvas(shell, width=preview_w, height=preview_h, bg=BG, highlightthickness=2,
                                highlightbackground=GOLD_DIM, bd=0, cursor='hand2')
        canvas.pack()
        preview = {'photo': None}

        def redraw(*_args):
            canvas.delete('all')
            try:
                with Image.open(path.get()) as source:
                    crop = {'zoom': zoom.get(), 'x': focus_x.get(), 'y': focus_y.get()}
                    image = uncharted_art.crop_image(source.convert('RGB'), (preview_w, preview_h), crop)
                preview['photo'] = ImageTk.PhotoImage(image)
                canvas.create_image(0, 0, image=preview['photo'], anchor='nw')
            except (OSError, ValueError, self.tk.TclError):
                preview['photo'] = None
                canvas.create_text(preview_w // 2, preview_h // 2, text=_('Choose image'), fill=MUTED,
                                   font=self.font_ui_md)
            canvas.create_rectangle(1, 1, preview_w - 2, preview_h - 2, outline=GOLD, width=2)

        def choose(candidate=''):
            chosen = candidate or self.image_path(title, path.get())
            if chosen:
                try:
                    with Image.open(chosen) as image:
                        image.verify()
                except (OSError, ValueError) as error:
                    self.messagebox.showerror(NAME, _('This image could not be opened: {}').format(error))
                    return
                path.set(str(Path(chosen)))
                redraw()

        controls = self.tk.Frame(shell, bg=PANEL)
        controls.pack(fill='x', pady=(self.px(14), 0))
        for row, (label, variable, start, end, step) in enumerate((
                (_('Zoom'), zoom, 1.0, 3.0, 0.01), (_('Horizontal position'), focus_x, 0.0, 1.0, 0.01),
                (_('Vertical position'), focus_y, 0.0, 1.0, 0.01))):
            self.tk.Label(controls, text=label, bg=PANEL, fg=TEXT, font=self.font_ui,
                          width=20, anchor='w').grid(row=row, column=0, sticky='w', pady=2)
            scale = self.tk.Scale(controls, variable=variable, from_=start, to=end, resolution=step,
                                  orient='horizontal', showvalue=False, length=self.px(380), bg=PANEL,
                                  fg=TEXT, troughcolor=CARD_HI, activebackground=GOLD, highlightthickness=0,
                                  command=redraw)
            scale.grid(row=row, column=1, sticky='ew', pady=2)
        buttons = self.tk.Frame(shell, bg=PANEL)
        buttons.pack(fill='x', pady=(self.px(16), 0))
        self.pill(buttons, _('Choose image'), choose, background=PANEL).pack(side='left')

        def restore():
            path.set('')
            zoom.set(1.0)
            focus_x.set(0.5)
            focus_y.set(0.5)
            redraw()

        self.pill(buttons, _('Restore'), restore, kind='ghost', background=PANEL).pack(side='left', padx=(6, 0))
        self.pill(buttons, _('Cancel'), window.destroy, kind='ghost', background=PANEL).pack(side='right')

        def save():
            result['value'] = (path.get(), {'zoom': zoom.get(), 'x': focus_x.get(), 'y': focus_y.get()})
            window.destroy()

        self.pill(buttons, _('Save changes'), save, kind='primary', background=PANEL).pack(side='right', padx=(0, 6))
        canvas.bind('<Button-1>', lambda _event: choose())
        self.enable_image_drop(canvas, choose)
        for variable in (path, zoom, focus_x, focus_y):
            variable.trace_add('write', redraw)
        redraw()
        window.bind('<Escape>', lambda _event: window.destroy())
        window.wait_window()
        return result['value']

    def edit_launcher_art(self, key, title, size, crop_key, initial_path=None):
        result = self.edit_art(key, title, size, crop_key, initial_path=initial_path)
        if not result:
            return
        self.app[key], self.app.setdefault('art_crops', {})[crop_key] = result
        self.change_art()

    def page(self, name, title, subtitle):
        tk = self.tk
        outer = tk.Frame(self.content, bg=SURFACE)
        shell = tk.Frame(outer, bg=SURFACE)
        shell.pack(fill='both', expand=True, padx=self.px(18), pady=(self.px(14), 0))
        head = tk.Frame(shell, bg=SURFACE)
        head.pack(fill='x', padx=self.px(26), pady=(self.px(18), self.px(14)))
        tk.Label(head, text=title, bg=SURFACE, fg=TEXT, font=self.font_title).pack(anchor='w')
        tk.Label(head, text=subtitle, bg=SURFACE, fg=MUTED, font=self.font_ui).pack(anchor='w', pady=(2, 0))
        # The page body scrolls when it is taller than the window (Display holds many options).
        body = tk.Frame(shell, bg=SURFACE)
        body.pack(fill='both', expand=True)
        canvas = tk.Canvas(body, bg=SURFACE, highlightthickness=0, bd=0)
        bar = self.ttk.Scrollbar(body, orient='vertical', command=canvas.yview)
        canvas.configure(yscrollcommand=bar.set)
        canvas.pack(side='left', fill='both', expand=True)
        inner = self.ttk.Frame(canvas, style='Shell.TFrame', padding=(26, 0, 26, 12))
        inner.columnconfigure(1, weight=1)
        self.page_cards[inner] = None
        window = canvas.create_window(0, 0, window=inner, anchor='nw')

        def fit(_event=None):
            canvas.itemconfigure(window, width=canvas.winfo_width())
            canvas.configure(scrollregion=(0, 0, canvas.winfo_width(), inner.winfo_reqheight()))
            if inner.winfo_reqheight() > canvas.winfo_height():
                bar.pack(side='right', fill='y')
            else:
                bar.pack_forget()
                canvas.yview_moveto(0)
        inner.bind('<Configure>', fit)
        canvas.bind('<Configure>', fit)

        self.scrollers[name] = (canvas, bar)
        self.pages[name] = outer
        return inner

    def on_wheel(self, event):
        """The mouse wheel scrolls the page under the pointer (also over its buttons and lists;
        lists do not change their choice); while a control is being bound it is the binding."""
        if getattr(self, 'capturing', None):
            return self.captured_wheel(event)
        scroller = self.scrollers.get(self.current_page)
        if scroller:
            canvas, bar = scroller
            x, y = self.root.winfo_pointerxy()
            inside = (canvas.winfo_rootx() <= x < canvas.winfo_rootx() + canvas.winfo_width() and
                      canvas.winfo_rooty() <= y < canvas.winfo_rooty() + canvas.winfo_height())
            if inside and bar.winfo_ismapped():
                canvas.yview_scroll(-1 * (event.delta // 120), 'units')
        return 'break'

    # ---- layout ------------------------------------------------------------------------------
    def build(self):
        tk, ttk = self.tk, self.ttk
        side_w = self.px(248)
        side = tk.Frame(self.root, bg=BG, width=side_w)
        side.pack(side='left', fill='y')
        side.pack_propagate(False)
        brand_wrap = side_w - 36
        tk.Label(side, text='UNCHARTED', bg=BG, fg=GOLD, font=self.font_brand_lg, anchor='w').pack(
            anchor='w', padx=18, pady=(28, 0))
        tk.Label(side, text='NATHAN DRAKE', bg=BG, fg=MUTED, font=self.font_brand_sm, anchor='w').pack(
            anchor='w', padx=18, pady=(8, 0))
        tk.Label(side, text='COLLECTION', bg=BG, fg=MUTED, font=self.font_brand_sm, anchor='w').pack(
            anchor='w', padx=18, pady=(1, 0))
        tk.Label(side, text=_('by {}').format(AUTHOR), bg=BG, fg=TEXT, font=self.font_ui_md,
                 anchor='w', wraplength=brand_wrap, justify='left').pack(anchor='w', padx=18, pady=(10, 0))
        tk.Label(side, text=_('version {}').format(VERSION), bg=BG, fg='#5c564c', font=self.font_ui_sm,
                 anchor='w').pack(anchor='w', padx=18, pady=(1, 0))
        tk.Frame(side, bg=LINE, height=1).pack(fill='x', padx=18, pady=(16, 14))
        self.nav, self.nav_pills, self.nav_icons, self.nav_marks, self.current_page = {}, {}, {}, {}, None
        for name, title in (('play', _('Play')), ('game', _('Game')), ('cheats', _('Cheats')), ('display', _('Display')),
                            ('controls', _('Controls')), ('fixes', _('Fixes')), ('trophies', _('Trophies')), ('advanced', _('Advanced')),
                            ('log', _('Log'))):
            row = tk.Frame(side, bg=BG, cursor='hand2')
            row.pack(fill='x', padx=(0, 12), pady=2)
            mark = tk.Frame(row, bg=BG, width=self.px(3))
            mark.pack(side='left', fill='y')
            pill = tk.Frame(row, bg=BG, cursor='hand2')
            pill.pack(side='left', fill='x', expand=True, padx=(self.px(9), 0))
            widgets = [row, pill]
            if self.font_icon:
                icon = tk.Label(pill, text=NAV_ICONS[name], bg=BG, fg=MUTED, font=self.font_icon,
                                padx=self.px(12), pady=self.px(9), cursor='hand2')
                icon.pack(side='left')
                self.nav_icons[name] = icon
                widgets.append(icon)
            item = tk.Label(pill, text=title, bg=BG, fg=TEXT, anchor='w', font=self.font_ui_md,
                            padx=self.px(2 if self.font_icon else 14), pady=self.px(9), cursor='hand2')
            item.pack(side='left', fill='x', expand=True)
            widgets.append(item)
            for widget in widgets:
                widget.bind('<Button-1>', lambda _e, n=name: self.show(n))
                widget.bind('<Enter>', lambda _e, n=name: self._nav_hover(n, True))
                widget.bind('<Leave>', lambda _e, n=name: self._nav_hover(n, False))
            self.nav[name], self.nav_pills[name], self.nav_marks[name] = item, pill, mark
        tk.Frame(side, bg=BG).pack(fill='both', expand=True)
        self.side = side
        self.side_note = tk.Label(side, text=_('Native port: the original PS4 game code runs directly on Windows.'),
                                  bg=BG, fg=MUTED, font=('Segoe UI', 9), justify='left',
                                  wraplength=side_w - 36)
        self.side_note.pack(anchor='w', padx=18, pady=(0, 18))

        tk.Frame(self.root, bg=LINE, width=1).pack(side='left', fill='y')
        right = tk.Frame(self.root, bg=SURFACE)
        right.pack(side='left', fill='both', expand=True)
        bar = tk.Frame(right, bg=PANEL, height=self.px(78))
        bar.pack(fill='x', side='bottom')
        bar.pack_propagate(False)
        tk.Frame(bar, bg=LINE, height=1).pack(fill='x')
        dock = tk.Frame(bar, bg=PANEL)
        dock.pack(fill='both', expand=True)
        self.status = tk.Label(dock, text='', bg=PANEL, fg=MUTED, font=('Segoe UI', 10), anchor='w', justify='left')
        self.status.pack(side='left', padx=24, pady=14)
        self.play_button = self.pill(dock, _('Play'), self.play, kind='primary', background=PANEL,
                                     padding=(30, 9), icon='\ue768' if self.font_icon else None)
        self.play_button.pack(side='right', padx=(10, 22), pady=12)
        self.stop_button = self.pill(dock, _('Stop'), self.stop, background=PANEL, padding=(24, 9),
                                     icon='\ue71a' if self.font_icon else None)
        self.stop_button.configure(state='disabled')
        self.stop_button.pack(side='right', pady=12)
        self.content = tk.Frame(right, bg=SURFACE)
        self.content.pack(fill='both', expand=True)

        self.pages, self.scrollers, self.page_cards = {}, {}, {}
        self.root.bind_all('<MouseWheel>', self.on_wheel)
        self.root.bind_class('TCombobox', '<MouseWheel>', self.on_wheel)
        self.build_play()
        self.build_game()
        self.build_display()
        self.build_controls()
        self.build_fixes()
        self.build_trophies()
        self.build_cheats()
        self.build_advanced()
        self.build_log()
        self.refresh()

    def _nav_style(self, name, active=False, hover=False):
        if active:
            bg, fg, icon = '#17181b', TEXT, GOLD
        elif hover:
            bg, fg, icon = '#111214', TEXT, TEXT
        else:
            bg, fg, icon = BG, MUTED, MUTED
        self.nav_pills[name].configure(bg=bg)
        self.nav[name].configure(bg=bg, fg=fg)
        if name in self.nav_icons:
            self.nav_icons[name].configure(bg=bg, fg=icon)
        self.nav_marks[name].configure(bg=GOLD if active else BG)

    def _nav_hover(self, name, inside):
        if name == self.current_page:
            return
        self._nav_style(name, hover=inside)

    def _card_hover(self, key):
        if self.card_hot == key:
            return
        self.card_hot = key
        for name in self.cards:
            self.draw_card(name)

    def show(self, name):
        for page in self.pages.values():
            page.pack_forget()
        self.pages[name].pack(fill='both', expand=True)
        self.current_page = name
        for n in self.nav:
            self._nav_style(n, active=(n == name))
        if name == 'play':
            self.refresh()
        elif name == 'trophies':
            self.trophy_info.clear()  # the game folder or the language may have changed
            self.trophy_state = None
            self.render_trophies()

    def build_play(self):
        tk, ttk = self.tk, self.ttk
        page = tk.Frame(self.content, bg=SURFACE)
        self.pages['play'] = page
        self.hero = tk.Canvas(page, height=self.px(260), bg=SURFACE, highlightthickness=0, bd=0)
        self.hero.pack(fill='x')
        self.hero.bind('<Configure>', self._sync_hero_height)
        self.hero.bind('<Button-1>', lambda _e: self.edit_launcher_art(
            'art_banner', _('Collection banner'), (1600, 450), 'banner'))
        self.enable_image_drop(self.hero, lambda path: self.quick_art('art_banner', 'banner', path))
        shell = tk.Frame(page, bg=SURFACE)
        shell.pack(fill='both', expand=True, padx=self.px(22), pady=(self.px(6), self.px(10)))
        head = tk.Frame(shell, bg=SURFACE)
        head.pack(fill='x', padx=self.px(22), pady=(self.px(14), self.px(8)))
        tk.Label(head, text=_('Library'), bg=SURFACE, fg=TEXT,
                 font=('Segoe UI Semibold', 14)).pack(side='left')
        tk.Label(head, text=_('Double-click a cover to launch'), bg=SURFACE, fg=MUTED,
                 font=self.font_ui_sm).pack(side='right')
        cards = tk.Frame(shell, bg=SURFACE)
        cards.pack(fill='x', padx=self.px(14), pady=(0, self.px(6)))
        self.cards, self.card_images = {}, {}
        for column, (key, _title, _subtitle, _exe) in enumerate(GAMES):
            cards.columnconfigure(column, weight=1, uniform='games')
            card = tk.Canvas(cards, height=self.px(460), bg=SURFACE, highlightthickness=0, bd=0, cursor='hand2')
            card.grid(row=0, column=column, sticky='nsew', padx=(0 if column == 0 else 18, 0))
            card.bind('<Configure>', lambda _e, k=key: self.draw_card(k))
            card.bind('<Button-1>', lambda _e, k=key: self.var('start').set(k))
            card.bind('<Double-Button-1>', lambda _e, k=key: (self.var('start').set(k), self.play()))
            card.bind('<Enter>', lambda _e, k=key: self._card_hover(k))
            card.bind('<Leave>', lambda _e, k=key: self._card_hover(None))
            self.enable_image_drop(card, lambda path, k=key: self.quick_art(f'art_{k}', k, path))
            self.cards[key] = card
        cards.bind('<Configure>', self.fit_posters)
        tk.Frame(shell, bg=SURFACE, height=1).pack(fill='x', padx=self.px(22), pady=(self.px(4), self.px(8)))
        row = tk.Frame(shell, bg=SURFACE)
        row.pack(fill='x', padx=self.px(22), pady=(0, self.px(8)))
        self.toggle(row, self.var('fullscreen'), SURFACE).pack(side='left')
        fullscreen = tk.Label(row, text=_('Fullscreen'), bg=SURFACE, fg=TEXT, font=self.font_ui, cursor='hand2')
        fullscreen.pack(side='left', padx=(self.px(10), 0))
        fullscreen.bind('<Button-1>', lambda _e: self.var('fullscreen').set(not self.var('fullscreen').get()))
        self.pill(row, _('Add to Steam'), self.add_to_steam, background=SURFACE, padding=(14, 6)).pack(
            side='left', padx=(self.px(24), 0))
        self.pill(row, _('Desktop shortcut'), self.shortcut, background=SURFACE, padding=(14, 6)).pack(
            side='left', padx=(self.px(8), 0))
        self.ready = tk.Label(shell, text='', bg=SURFACE, fg=MUTED, font=self.font_ui_sm,
                              wraplength=self.px(820), justify='left')
        self.ready.pack(anchor='w', padx=self.px(22), pady=(0, self.px(16)))
        self.var('start').trace_add('write', lambda *_a: self.refresh())

    def fit_posters(self, _event=None):
        """Poster tiles keep the cover shape, but stay short enough that the caption is on screen."""
        sample = self.cards.get('u1')
        if sample is None:
            return
        width = sample.winfo_width()
        if width < 80:
            return
        page = self.pages.get('play')
        natural = int(round(width * 365 / 273)) + self.px(78)
        avail = natural
        if page is not None and page.winfo_height() > 240:
            # Leave the library heading, the fullscreen row, and the status line on screen.
            avail = page.winfo_height() - self.hero.winfo_height() - self.px(196)
        total = max(self.px(168), min(natural, avail))
        for card in self.cards.values():
            if int(card['height']) != total:
                card.configure(height=total)

    def _tile_content_size(self, canvas_w, canvas_h):
        """Size passed to poster_tile so the bitmap, including its shadow pad, stays inside the card."""
        inner_w = max(48, canvas_w - 8)
        inner_h = max(80, canvas_h - 8)
        while inner_w > 48 and inner_w + 2 * max(10, inner_w // 28) > canvas_w - 2:
            inner_w -= 2
        pad = max(10, inner_w // 28)
        while inner_h > 80 and inner_h + 2 * pad > canvas_h - 2:
            inner_h -= 2
        return inner_w, inner_h

    def draw_card(self, key):
        """Rounded cover tile with shadow; gold ring when selected."""
        card = self.cards[key]
        w, h = card.winfo_width(), card.winfo_height()
        if w < 48 or h < 48:
            return
        title, subtitle = next((t, sub) for k, t, sub, _e in GAMES if k == key)
        present = self.available.get(key, True)
        chosen = key == self.var('start').get()
        hot = key == self.card_hot
        inner_w, inner_h = self._tile_content_size(w, h)
        page = tuple(int(SURFACE[i:i + 2], 16) for i in (1, 3, 5))
        wanted = (inner_w, inner_h, present, chosen, hot)
        if self.card_images.get(key, (None,))[0] != wanted:
            image = None
            try:
                from PIL import ImageTk
                import uncharted_art
                scene = uncharted_art.poster_tile(key, inner_w, inner_h, title, subtitle, selected=chosen,
                                                  hover=hot and not chosen, dimmed=not present, page=page)
                image = ImageTk.PhotoImage(scene)
            except Exception:
                image = None
            self.card_images[key] = (wanted, image)
        card.delete('all')
        image = self.card_images[key][1]
        if image:
            x = max(0, (w - image.width()) // 2)
            card.create_image(x, 0, image=image, anchor='nw')
        else:
            card.create_rectangle(0, 0, w, h, fill=CARD, outline=LINE)
        if hot:
            label = _('Change image')
            box_w = max(self.px(108), len(label) * self.px(7))
            card.create_rectangle(w - box_w - self.px(12), self.px(12), w - self.px(12), self.px(42),
                                  fill='#111214', outline=GOLD_DIM, tags='edit_art')
            card.create_text(w - box_w // 2 - self.px(12), self.px(27), text=label, fill=TEXT,
                             font=self.font_ui_sm, tags='edit_art')
            card.tag_bind('edit_art', '<Button-1>', lambda _e, k=key: self.edit_launcher_art(
                f'art_{k}', _('Change image'), (600, 900), k))

    def _sync_hero_height(self, _event=None):
        """Keep the header at the banner's 16:9 aspect ratio."""
        c = self.hero
        w = max(c.winfo_width(), 400)
        h = max(self.px(160), min(self.px(230), int(w * 450 / 1600)))
        if int(c['height']) != h:
            c.configure(height=h)
        self.draw_hero()

    def draw_hero(self):
        """Collection banner up top; selected game and folder status along the bottom edge."""
        c = self.hero
        c.delete('all')
        w, h = max(c.winfo_width(), 400), int(c['height'])
        start = self.var('start').get()
        title, subtitle = next((t, sub) for k, t, sub, _e in GAMES if k == start)
        game_dir = game_folder(self.current(), start)
        key = (w, h, start, game_dir, self.app.get('art_banner', ''))
        if self.banner_key != key:
            self.banner_key = key
            # The collection banner when this machine has it (launcher art is not distributed),
            # otherwise the background art of the player's own dump.
            self.banner_scaled = self.photo_collection_banner(w, h) or \
                self.scaled_art(Path(game_dir or '.') / 'sce_sys' / 'pic1.png', w) or \
                self.photo_hero(start, w, h)
            self.cover_image = None
        if self.banner_scaled:
            c.create_image(w // 2, h // 2, image=self.banner_scaled)
        c.create_rectangle(0, h - 1, w, h, fill=GOLD_DIM, outline='')
        info = self.info
        sub = (f"{info['id']}  ·  {_('version')} {info['version']}" if info
               else _('Set your game folder on the Game page'))
        c.create_text(self.px(24), h - self.px(14), text=sub, anchor='sw', fill=GOLD,
                      font=self.font_brand_sm)
        c.create_text(w - self.px(24), h - self.px(14), text=f'{_(title)}  ·  {_(subtitle)}',
                      anchor='se', fill='#f6f1e6', font=self.font_brand_md)
        label = _('Change image')
        c.create_text(w - self.px(24), self.px(18), text=label, anchor='ne', fill='#d6c9ae',
                      font=self.font_ui_sm)

    def photo_collection_banner(self, width, height):
        try:
            from PIL import ImageTk
            import uncharted_art
            page = tuple(int(BG[i:i + 2], 16) for i in (1, 3, 5))
            scene = uncharted_art.collection_banner(width, height, page)
            return ImageTk.PhotoImage(scene) if scene is not None else None
        except Exception:
            return None

    def draw_banner(self):
        self.draw_hero()

    def photo_hero(self, key, width, height):
        """Blurred cover backdrop when the dump has no pic1.png."""
        try:
            from PIL import ImageTk
            import uncharted_art
            page = tuple(int(BG[i:i + 2], 16) for i in (1, 3, 5))
            scene = uncharted_art.hero_backdrop(key, width, height, page)
            return ImageTk.PhotoImage(scene) if scene is not None else None
        except Exception:
            return None

    def photo_banner(self, key, width, height):
        return self.photo_hero(key, width, height)

    def cover_art(self, game_dir, side):
        try:
            from PIL import Image, ImageTk
            import uncharted_art
            art = uncharted_art.cover(game_dir)
            return ImageTk.PhotoImage(art.resize((side, side), Image.LANCZOS)) if art else None
        except Exception:
            return None

    def scaled_art(self, path, width):
        """The art cut to the banner, darkened (the parchment is light) and faded into the page."""
        try:
            from PIL import Image, ImageTk
            height = int(self.hero['height'])
            with Image.open(path) as image:
                scaled = image.convert('RGB').resize((width, round(image.height * width / image.width)), Image.LANCZOS)
            top = max(0, (scaled.height - height) // 10)  # pic1's figure has its face near the top
            art = scaled.crop((0, top, width, top + height))
            art = Image.blend(art, Image.new('RGB', art.size, (6, 5, 4)), 0.55)
            page = tuple(int(BG[i:i + 2], 16) for i in (1, 3, 5))
            fade = Image.linear_gradient('L').resize(art.size)
            fade = fade.point(lambda v: 0 if v < 90 else min(255, (v - 90) * 255 // 130))
            return ImageTk.PhotoImage(Image.composite(Image.new('RGB', art.size, page), art, fade))
        except (ImportError, OSError):
            return None

    def build_game(self):
        f = self.page('game', _('Game'), _('Your own decrypted dump of the collection (CUSA02343, CUSA02320, …).'))
        self.folder(f, 'game_dir', _('Game folder'), _('The folder with eboot.bin'),
                    _('The folder with eboot.bin, big2-ps4_Shipping.elf and big3-ps4_Shipping.elf. '
                      'Leave it empty if every game has a folder of its own, below.'),
                    on_change=self.refresh)
        self.folder(f, 'user_dir', _('Saves folder'), _('Where saves and the shader cache are kept'),
                    _('Empty: the user folder next to the launcher.'))
        self.section(f, _('Separate game folders'))
        self.note(f, _('Only when the games are not in one collection folder (each dumped on its own). A game with '
                       'a folder here is taken from it; the others from the collection folder above.'), top=2)
        for key, _title, subtitle, _exe in GAMES:
            self.folder(f, f'game_dir_{key}', _(subtitle.split(' · ')[0]), _('The folder of this game'),
                        _('Empty: the collection folder.'), on_change=self.refresh)
        self.section(f, _('Console'))
        self.row(f, _('Language'), self.choice(f, 'language', LANGUAGES, 30),
                 _('The language the PS4 would be set to; the games use it for text and speech.'))
        name = self.ttk.Entry(f, textvariable=self.var('player_name'), width=30)
        self.row(f, _('Player name'), name, _('Shown where the game uses the PSN name.'))

    def build_display(self):
        f = self.page('display', _('Display'), _('How the game window is shown.'))
        self.check(f, 'fullscreen', _('Fullscreen'))
        self.row(f, _('Presentation'), self.choice(f, 'present_mode', PRESENT_MODES, 34),
                 _('Mailbox shows each frame as soon as it is ready without tearing; FIFO waits for the display.'))
        refresh = display_refresh_rate()
        hint = _('Gameplay keeps its normal speed at every setting; the cutscene videos always play at '
                 '60 FPS. Above your display\'s refresh rate use Mailbox or Immediate.')
        if refresh:
            hint += ' ' + _('Your display runs at {} Hz.').format(refresh)
        self.row(f, _('Frame rate'), self.choice(f, 'frame_rate', FRAME_RATES, 34), hint)
        self.section(f, _('Resolution'))
        self.row(f, _('Render resolution'), self.choice(f, 'render_height', RENDER_HEIGHTS, 34),
                 _('The size the games draw at, so a 1440p or 4K screen gets a sharper picture, not an enlarged '
                   '1080p one. It needs more graphics memory and a faster GPU; the games\' GPU memory limits the '
                   'largest size.'))
        self.row(f, _('Aspect ratio'), self.choice(f, 'aspect', ASPECTS, 34),
                 _('Ultrawide screens: the games draw a wider view (more at the sides) instead of stretching the '
                   '16:9 picture. "The shape of the window or screen" follows the window size below, or the '
                   'screen in fullscreen.'))
        self.row(f, _('Window size'), self.choice(f, 'window_size', WINDOW_SIZES, 34),
                 _('The window the picture is shown in; fullscreen uses the screen. A picture of another size is '
                   'scaled to fit (FSR 1 below when it is enlarged).'))
        self.row(f, _('Upscaling'), self.choice(f, 'scaler', SCALERS, 34),
                 _('FSR 1 keeps edges sharp when the image is enlarged; bilinear is softer.'))
        self.row(f, _('Sharpening'), self.choice(f, 'sharpening', SHARPENING, 34),
                 _('AMD RCAS after FSR 1 (only while the image is enlarged).'))
        self.section(f, _('Anti-aliasing'))
        modes = ANTI_ALIASING + ([FSR411] if fsr411_assets() else []) + ([XESS] if xess_available() else [])
        self.row(f, _('Anti-aliasing'), self.choice(f, 'anti_aliasing', modes, 34),
                 _('Temporal anti-aliasing at the games\' 1920 × 1080, with their own motion vectors: '
                   'smoother edges and less shimmering than the games\' own. DLSS needs a GeForce RTX; '
                   'FSR 4 runs best on a Radeon RX 7000/9000 or a GeForce RTX 40/50. '
                   'Intel XeSS runs on any GPU (best on Intel Arc).'))
        self.row(f, _('Anti-aliasing sharpness'), self.choice(f, 'aa_sharpness', AA_SHARPNESS, 34))
        self.section(f, _('Frame generation'))
        hint = _('Frames made between two of the game\'s, for smoother motion: 60 FPS shows as 120. '
                 'Use Mailbox or Immediate presentation and a display of at least that refresh rate. '
                 'Adds a little input latency; menus and cutscene videos are not generated. Use it '
                 'with the 60 or 120 FPS frame rate: above about 165 FPS it pauses.')
        if refresh:
            hint += ' ' + _('Your display runs at {} Hz.').format(refresh)
        self.row(f, _('Frame generation'), self.choice(f, 'frame_generation', FRAME_GENERATION, 34), hint)
        self.row(f, _('Multiplier'), self.choice(f, 'frame_gen_multiplier', FRAME_GEN_MULTIPLIERS, 34),
                 _('FSR 3 makes one frame (2×). DLSS makes up to three on a GeForce RTX 50 (4×), '
                   'one on an RTX 40.'))
        self.check(f, 'show_fps', _('Show the FPS counter'))
        self.section(f, _('Audio'))
        self.row(f, _('Speakers'), self.choice(f, 'speakers', SPEAKERS, 34),
                 _('What the games are told the speakers are, which sets the choices in their Audio Output '
                   'menu (stereo, 5.1, 7.1). Automatic follows the Windows speaker configuration of the '
                   'default playback device; set it to 5.1 or 7.1 when the games only offer stereo.'))

    def build_controls(self):
        f = self.page('controls', _('Controls'),
                      _('Keyboard, mouse and controller. They work together; changes apply when the game starts.'))
        self.key_buttons = {}
        self.capturing = None
        self.section(f, _('Mouse'), top=6)
        self.check(f, 'mouse_look', _('Look around with the mouse'),
                   _('The cursor stays in the game window; hold Left Alt (or switch windows) to free it. '
                     'Insert opens the in-game settings menu.'))
        from uncharted_widgets import Slider
        self.row(f, _('Mouse sensitivity'),
                 Slider(self.tk, self.target(f), self.var('mouse_sensitivity'), MOUSE_SENS_MIN, MOUSE_SENS_MAX, 0.05,
                        COLORS, self.px, self.font_ui_md, width=300, log=True, entry=True),
                 _('How far the camera turns for a mouse movement; 1.00 is the default. Drag the slider or '
                   'type a number (0.1 to 10) for a faster turn.'))
        self.check(f, 'mouse_invert_y', _('Invert the mouse vertically'))

        self.section(f, _('Button pictures'))
        self.row(f, _('Show in the game'), self.choice(f, 'button_prompts', BUTTON_PROMPTS, 40),
                 _('The pictures of buttons in menus and hints. Automatic shows your keys while you use '
                   'the keyboard or mouse, Xbox buttons after an Xbox (or other) controller, and the '
                   'PlayStation buttons after a PlayStation controller.'))

        self.section(f, _('Keyboard and mouse buttons'))
        self.note(f, _('Click a box, then press a key or a mouse button (the wheel counts too). '
                       '✕ removes a binding. Each action can have two.'), top=2)
        card = self.target(f)
        table = self.tk.Frame(card, bg=CARD)
        table.grid(row=self.next_row(card), column=0, columnspan=2, sticky='w', pady=(10, 0))
        rows = [(a, t) for a, t, _k, _p in BUTTON_ACTIONS] + [(a, t) for a, t, _k in STICK_ACTIONS]
        for r, (action, title) in enumerate(rows):
            self.ttk.Label(table, text=_(title)).grid(row=r, column=0, sticky='w', padx=(0, 24), pady=3)
            for slot in (0, 1):
                button = self.tk.Button(table, width=16, relief='flat', bg=CARD_HI, fg=TEXT,
                                        activebackground='#2a2c31', activeforeground=TEXT,
                                        font=self.font_ui, cursor='hand2', bd=0, padx=8, pady=4,
                                        command=lambda a=action, i=slot: self.capture_key(a, i))
                button.grid(row=r, column=1 + slot * 2, sticky='w', pady=3)
                clear = self.tk.Label(table, text='\ue711' if self.font_icon else '✕', bg=CARD, fg=MUTED,
                                      font=self.font_icon_sm or self.font_ui, cursor='hand2', padx=8)
                clear.grid(row=r, column=2 + slot * 2, sticky='w', padx=(2, 10))
                clear.bind('<Button-1>', lambda _e, a=action, i=slot: self.set_key(a, i, ''))
                clear.bind('<Enter>', lambda _e, w=clear: w.configure(fg=TEXT))
                clear.bind('<Leave>', lambda _e, w=clear: w.configure(fg=MUTED))
                self.key_buttons[(action, slot)] = button
        self.show_keys()
        holder = self.tk.Frame(card, bg=CARD)
        holder.grid(row=self.next_row(card), column=0, columnspan=2, sticky='w', pady=(12, 0))
        self.pill(holder, _('Default keys'), self.reset_keys).pack(side='left')

        self.section(f, _('Controller'))
        self.note(f, _('Which controller button gives each PS4 button (Xbox / PlayStation names).'), top=2)
        self.pad_vars = {}
        for action, title, _k, _p in BUTTON_ACTIONS:
            var = self.tk.StringVar(value=(self.app.get('pad_bindings') or {}).get(action, DEFAULT_PAD[action]))
            self.pad_vars[action] = var
            values = [v for v, _t in PAD_BUTTONS]
            box = self.ttk.Combobox(f, values=[_(t) for _v, t in PAD_BUTTONS], state='readonly', width=26)
            box.current(values.index(var.get()) if var.get() in values else values.index('none'))
            box.bind('<<ComboboxSelected>>', lambda _e, b=box, v=var: v.set(values[b.current()]))
            var.trace_add('write', lambda *_a, b=box, v=var: b.current(values.index(v.get())))
            self.row(f, _(title), box)
        self.check(f, 'pad_invert_y', _('Invert the right stick vertically'))
        card = self.target(f)
        holder = self.tk.Frame(card, bg=CARD)
        holder.grid(row=self.next_row(card), column=0, columnspan=2, sticky='w', pady=(12, 0))
        self.pill(holder, _('Default controller buttons'), self.reset_pad).pack(side='left')

        self.section(f, _('Vibration'))
        self.controller_name = self.ttk.Label(self.target(f), text=_('Detecting…'))
        self.row(f, _('Controller'), self.controller_name)
        self.controller_status = self.ttk.Label(self.target(f), text=_('Checking vibration support…'),
                                                style='Muted.TLabel')
        self.row(f, _('Status'), self.controller_status)
        self.check(f, 'pad_vibration', _('Controller vibration'),
                   _('The setting applies the next time the game starts.'))
        self.row(f, _('Vibration intensity'),
                 Slider(self.tk, self.target(f), self.var('pad_vibration_strength'), 0, 100, 5,
                        COLORS, self.px, self.font_ui_md, fmt='{:.0f}', width=300, suffix='%'))
        card = self.target(f)
        holder = self.tk.Frame(card, bg=CARD)
        holder.grid(row=self.next_row(card), column=0, columnspan=2, sticky='w', pady=(12, 0))
        self.controller_refresh_button = self.pill(holder, _('Detect controller'), self.detect_controller)
        self.controller_refresh_button.pack(side='left')
        self.controller_test_button = self.pill(holder, _('Test vibration'),
                                                lambda: self.detect_controller(test=True))
        self.controller_test_button.pack(side='left', padx=(8, 0))
        self.controller_test_button.configure(state='disabled')

    def key_list(self, action):
        value = (self.app.get('key_bindings') or {}).get(action, DEFAULT_KEYS[action])
        parts = (value.split('|') + ['', ''])[:2]
        return [p.strip() for p in parts]

    def show_keys(self):
        for (action, slot), button in self.key_buttons.items():
            if self.capturing == (action, slot):
                button.configure(text=_('Press a key…'), bg=GOLD, fg='#141008')
            else:
                name = self.key_list(action)[slot]
                button.configure(text=name or '—', bg=CARD_HI, fg=TEXT if name else MUTED)

    def set_key(self, action, slot, name):
        keys = self.key_list(action)
        keys[slot] = name
        if name:  # one binding per key: taken from the action that had it
            bindings = dict(self.app.get('key_bindings') or {})
            for other in DEFAULT_KEYS:
                if other == action:
                    continue
                others = self.key_list(other)
                if name in others:
                    others = ['' if k == name else k for k in others]
                    bindings[other] = '|'.join(others).strip('|') if any(others) else ''
            self.app['key_bindings'] = bindings
        if not keys[0] and keys[1]:
            keys = [keys[1], '']
        bindings = dict(self.app.get('key_bindings') or {})
        bindings[action] = '|'.join(k for k in keys if k)
        self.app['key_bindings'] = bindings
        self.capturing = None
        self.release_capture()
        self.show_keys()

    def capture_key(self, action, slot):
        self.release_capture()
        self.capturing = (action, slot)
        self.show_keys()
        # The click that opened the capture must not be taken as the binding.
        self.root.after(200, self.arm_capture)

    def arm_capture(self):
        if not self.capturing:
            return
        self.root.bind_all('<KeyPress>', self.captured_key)
        self.root.bind_all('<ButtonPress>', self.captured_button)
        self.poll_side_buttons(side_buttons())

    def poll_side_buttons(self, held):
        """Tk on Windows never reports the side buttons (XBUTTON1/2: no <Button-4/5> event), so they
        are read from the system while a binding is being captured. `held`: their state a moment ago."""
        if not self.capturing:
            return
        down = side_buttons()
        if self.root.focus_displayof():  # only while the launcher is the active window
            for index, name in enumerate(('Mouse 4', 'Mouse 5')):
                if down[index] and not held[index]:
                    self.set_key(*self.capturing, name)
                    return
        self.side_poll = self.root.after(15, self.poll_side_buttons, down)

    def release_capture(self):
        for sequence in ('<KeyPress>', '<ButtonPress>'):
            self.root.unbind_all(sequence)
        if getattr(self, 'side_poll', None):
            self.root.after_cancel(self.side_poll)
            self.side_poll = None

    def captured_key(self, event):
        sym = event.keysym
        if sym in RESERVED_KEYS:
            self.status.configure(text=_(RESERVED_KEYS[sym]), fg=GOLD)
            return 'break'
        name = WIN_NUMPAD.get(event.keycode) if sys.platform == 'win32' else None
        name = name or TK_KEYS.get(sym)
        if not name and len(sym) == 1 and sym.isalnum():
            name = sym.upper()
        if not name and sym.startswith('F') and sym[1:].isdigit():
            name = sym
        if not name and sym.startswith('KP_') and sym[3:].isdigit():
            name = f'Keypad {sym[3:]}'
        if name:
            self.set_key(*self.capturing, name)
        return 'break'

    def captured_button(self, event):
        # 4 and 5 as other toolkits number the side buttons; 8 and 9 as X11 does
        name = {1: 'Mouse Left', 2: 'Mouse Middle', 3: 'Mouse Right', 4: 'Mouse 4', 5: 'Mouse 5',
                8: 'Mouse 4', 9: 'Mouse 5'}.get(event.num)
        if name:
            self.set_key(*self.capturing, name)
        return 'break'

    def captured_wheel(self, event):
        self.set_key(*self.capturing, 'Wheel Up' if event.delta > 0 else 'Wheel Down')
        return 'break'

    def reset_keys(self):
        self.app['key_bindings'] = {}
        self.capturing = None
        self.release_capture()
        self.show_keys()

    def reset_pad(self):
        for action, var in self.pad_vars.items():
            var.set(DEFAULT_PAD[action])

    def build_fixes(self):
        f = self.page('fixes', _('Fixes'), _('Fixes that cost speed. Automatic turns each one on only where it '
                                             'helps; change one only when you see its problem.'))
        for key, (title, hint, _choices) in FIXES.items():
            self.row(f, _(title), self.choice(f, key, FIX_CHOICES, 16), _(hint))

    # ---- trophies ----------------------------------------------------------------------------
    def build_trophies(self):
        tk = self.tk
        f = self.page('trophies', _('Trophies'),
                      _("The games' own trophy lists. A trophy shows here, and as a notice in the game, "
                        'when you earn it.'))
        self.trophy_game = self.app.get('trophy_game') if self.app.get('trophy_game') in dict(
            (k, 1) for k, _t, _s, _e in GAMES) else GAMES[0][0]
        self.trophy_filter = 'all'
        self.trophy_icons, self.trophy_state, self.trophy_info = {}, None, {}
        bar = tk.Frame(f, bg=SURFACE)
        bar.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(0, self.px(10)))
        self.trophy_tabs = {}
        for key, title, _sub, _exe in GAMES:
            button = self.pill(bar, title, lambda k=key: self.set_trophy_game(k), background=SURFACE,
                               padding=(16, 7))
            button.pack(side='left', padx=(0, 8))
            self.trophy_tabs[key] = button
        self.trophy_filters = {}
        for name, title in (('locked', _('Locked')), ('earned', _('Earned')), ('all', _('All'))):
            button = self.pill(bar, title, lambda n=name: self.set_trophy_filter(n), background=SURFACE,
                               padding=(14, 7))
            button.pack(side='right', padx=(8, 0))
            self.trophy_filters[name] = button
        self.trophy_summary = tk.Label(f, text='', bg=SURFACE, fg=MUTED, font=self.font_ui, anchor='w',
                                       justify='left')
        self.trophy_summary.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(0, self.px(10)))
        self.trophy_body = tk.Frame(f, bg=SURFACE)
        self.trophy_body.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we')
        self.root.after(1500, self.poll_trophies)

    def trophy_user_dir(self):
        return Path(self.var('user_dir').get().strip() or DATA_DIR / 'user')

    def trophy_info_for(self, key):
        """The trophy list of one game (texts in the console language), made from its trophy pack in
        the game folder when it is not there yet; None without a pack."""
        s = self.current()
        folder = game_folder(s, key)
        if not folder:
            return None
        stem = next(exe for k, _t, _s, exe in GAMES if k == key)
        out = DATA_DIR / 'out' / Path(stem).stem.split('-')[0] / 'trophy'
        own = bool(str(s.get(f'game_dir_{key}') or '').strip())
        language = str(s.get('language') or '1')
        try:
            trophies.prepare(folder, key, out, int(language) if language.isdigit() else 1, own)
        except Exception:  # a broken pack must not break the launcher
            return None
        info = trophies.read_cache(out)
        if info:
            info['folder'] = out
        return info

    def trophy_icon(self, info, number, size, earned):
        """The trophy's icon from the cache folder (dimmed while it is locked)."""
        name = (info['npcommid'], number, size, earned)
        if name not in self.trophy_icons:
            image = None
            try:
                from PIL import Image, ImageEnhance, ImageTk
                with Image.open(info['folder'] / f'TROP{number:03d}.PNG') as source:
                    icon = source.convert('RGBA').resize((size, size), Image.LANCZOS)
                if not earned:
                    icon = ImageEnhance.Brightness(ImageEnhance.Color(icon).enhance(0.15)).enhance(0.55)
                image = ImageTk.PhotoImage(icon)
            except (ImportError, OSError):
                pass
            self.trophy_icons[name] = image
        return self.trophy_icons[name]

    @staticmethod
    def mark_pill(button, on):
        kind = 'primary' if on else 'secondary'
        if button.kind != kind:
            button.kind = kind
            button.draw()

    def set_trophy_game(self, key):
        self.trophy_game, self.trophy_state = key, None
        self.app['trophy_game'] = key
        self.render_trophies()

    def set_trophy_filter(self, name):
        self.trophy_filter, self.trophy_state = name, None
        self.render_trophies()

    def render_trophies(self, force=False):
        """Redraws the list of the chosen game when what it shows changed."""
        key = self.trophy_game
        info = self.trophy_info.get(key)
        if info is None or force:
            info = self.trophy_info[key] = self.trophy_info_for(key) or False
        unlocked = trophies.read_unlocks(self.trophy_user_dir(), info['npcommid']) if info else {}
        state = (key, self.trophy_filter, tuple(sorted(unlocked.items())), bool(info))
        for k, button in self.trophy_tabs.items():
            self.mark_pill(button, k == key)
        for name, button in self.trophy_filters.items():
            self.mark_pill(button, name == self.trophy_filter)
        if state == self.trophy_state:
            return
        self.trophy_state = state
        tk = self.tk
        for child in self.trophy_body.winfo_children():
            child.destroy()
        if not info:
            self.trophy_summary.configure(text='')
            self.note_in(self.trophy_body, _('No trophy list for this game. Set the game folder on the Game page '
                                             '(it needs sce_sys/trophy/trophy0N.trp).'))
            return
        progress = trophies.progress(info, unlocked)
        grades = '   '.join(f"{_(trophies.GRADE_NAMES[g])} {progress['earned'][g]}/{progress['total'][g]}"
                            for g in 'PGSB' if progress['total'][g])
        self.trophy_summary.configure(
            text=f"{info['title']}\n{progress['count']} of {len(info['trophies'])} earned · {progress['percent']}%"
                 f'   ·   {grades}')
        shown = 0
        size = self.px(52)
        for number, grade, hidden, name, detail in info['trophies']:
            when = unlocked.get(number)
            if (self.trophy_filter == 'earned') != (when is not None) and self.trophy_filter != 'all':
                continue
            shown += 1
            if hidden and when is None:
                name, detail = _('Hidden trophy'), _('Keep playing to find out how to earn it.')
            card = tk.Frame(self.trophy_body, bg=CARD, highlightthickness=1, highlightbackground=LINE)
            card.pack(fill='x', pady=(0, 6))
            icon = self.trophy_icon(info, number, size, when is not None)
            if icon:
                tk.Label(card, image=icon, bg=CARD).pack(side='left', padx=(10, 12), pady=8)
            side = tk.Frame(card, bg=CARD)
            side.pack(side='right', padx=(10, 16))
            color = TROPHY_COLORS.get(grade, MUTED) if when is not None else MUTED
            tk.Label(side, text=_(trophies.GRADE_NAMES.get(grade, 'Bronze')), bg=CARD, fg=color,
                     font=('Segoe UI Semibold', 9)).pack(anchor='e')
            tk.Label(side, text=time.strftime('%d %b %Y', time.localtime(when)) if when is not None else _('Locked'),
                     bg=CARD, fg=MUTED, font=self.font_ui_sm).pack(anchor='e', pady=(2, 0))
            middle = tk.Frame(card, bg=CARD)
            middle.pack(side='left', fill='x', expand=True, pady=8)
            tk.Label(middle, text=name, bg=CARD, fg=TEXT if when is not None else MUTED, anchor='w',
                     font=self.font_ui_md).pack(anchor='w')
            tk.Label(middle, text=detail, bg=CARD, fg=MUTED, font=self.font_ui_sm, justify='left', anchor='w',
                     wraplength=self.px(520)).pack(anchor='w', pady=(1, 0))
        if not shown:
            self.note_in(self.trophy_body, _('Nothing here yet.') if self.trophy_filter == 'earned'
                         else _('Every trophy is earned.'))

    def note_in(self, parent, text):
        self.tk.Label(parent, text=text, bg=SURFACE, fg=MUTED, font=self.font_ui, anchor='w', justify='left',
                      wraplength=self.px(640)).pack(anchor='w', pady=(8, 0))

    def poll_trophies(self):
        """Every 1.5 s while the page is open: the game writes an unlock as it happens."""
        if self.current_page == 'trophies':
            try:
                self.render_trophies()
            except Exception:
                pass
        self.root.after(1500, self.poll_trophies)

    def build_cheats(self):
        f = self.page('cheats', _('Cheats'), _('Each cheat only affects its own game; changes apply when the game starts.'))
        for game, title, subtitle, _exe in GAMES:
            self.section(f, f'{title} · {subtitle}')
            self.check(f, f'{game}_infinite_health', _('Infinite Health'))
            self.check(f, f'{game}_infinite_ammo', _('Infinite Ammo'))

    def build_advanced(self):
        f = self.page('advanced', _('Advanced'), _('Maintenance and options for testing.'))
        self.section(f, _('Launcher language'), top=2)
        self.row(f, _('Interface language'),
                 self.choice(f, 'ui_language', uncharted_i18n.LANGUAGES, 24, self.change_ui_language),
                 _('This changes only the launcher. The game language is selected on the Game page.'))
        self.check(f, 'close_on_play', _('Close the launcher when the game starts'))
        extra = self.ttk.Entry(f, textvariable=self.var('extra_env'), width=60)
        self.row(f, _('Environment'), extra, _('Extra variables for the game, NAME=value separated by spaces.'))
        self.section(f, _('Shader cache'))
        self.note(f, _('Compiled shaders are kept so the game does not stutter again. Clear the cache only '
                       'when the picture is broken after an update.'), top=2)
        card = self.target(f)
        holder = self.tk.Frame(card, bg=CARD)
        holder.grid(row=self.next_row(card), column=0, columnspan=2, sticky='w', pady=(12, 0))
        self.pill(holder, _('Clear shader cache'), self.clear_cache).pack(side='left')
        self.pill(holder, _('Open the saves folder'),
                  lambda: self.open_path(self.var('user_dir').get(), 'user_dir')).pack(side='left', padx=(8, 0))
        self.section(f, _('Updates'))
        self.check(f, 'check_updates', _('Look for a new version when the launcher starts'))
        card = self.target(f)
        holder = self.tk.Frame(card, bg=CARD)
        holder.grid(row=self.next_row(card), column=0, columnspan=2, sticky='w', pady=(12, 0))
        self.pill(holder, _('Check for updates'),
                  lambda: threading.Thread(target=self.check_update, args=(True,), daemon=True).start()
                  ).pack(side='left')
        self.pill(holder, _('Releases page'), lambda: webbrowser.open(RELEASES_PAGE)).pack(side='left', padx=(8, 0))

    def build_log(self):
        tk = self.tk
        page = tk.Frame(self.content, bg=SURFACE)
        self.pages['log'] = page
        shell = tk.Frame(page, bg=SURFACE)
        shell.pack(fill='both', expand=True, padx=self.px(18), pady=(self.px(14), self.px(14)))
        head = tk.Frame(shell, bg=SURFACE)
        head.pack(fill='x', padx=self.px(26), pady=(self.px(18), self.px(14)))
        tk.Label(head, text=_('Log'), bg=SURFACE, fg=TEXT, font=self.font_title).pack(side='left')
        self.pill(head, _('Copy'), self.copy_log, background=SURFACE, padding=(16, 6)).pack(side='right')
        body = tk.Frame(shell, bg=SURFACE)
        body.pack(fill='both', expand=True, padx=self.px(26), pady=(0, self.px(8)))
        self.log = tk.Text(body, bg=CARD, fg='#d9d4ca', insertbackground=TEXT, font=('Cascadia Mono', 9), wrap='none',
                           relief='flat', state='disabled', highlightbackground=LINE, highlightthickness=1,
                           padx=self.px(12), pady=self.px(10))
        bar = self.ttk.Scrollbar(body, orient='vertical', command=self.log.yview)
        self.log.configure(yscrollcommand=bar.set)
        bar.pack(side='right', fill='y')
        self.log.pack(fill='both', expand=True)

    def current(self):
        """The settings as they stand on screen (not yet saved)."""
        return {**self.app, **{key: v.get() for key, v in self.vars.items()}}

    def refresh(self):
        """Game cards, the ready line and the status for the current choices."""
        s = self.current()
        start = s['start']
        self.info = game_info(game_folder(s, start))
        info = self.info
        # Nothing set yet: no cover is dimmed.
        unset = not any(game_folder(s, k) for k, _t, _s, _e in GAMES)
        self.available = {key: unset or game_available(s, key) for key, _t, _s, _e in GAMES}
        for key in self.cards:
            self.draw_card(key)
        if not info:
            ready = _('Choose the game folder (the one with eboot.bin) on the Game page.')
        else:
            missing = [title for key, title, _s, _e in GAMES if not self.available[key]]
            ready = f"{info['title']} · {info['id']} · {_('version')} {info['version']}"
            if missing:
                ready += '\n' + _('Not in this folder: ') + ', '.join(missing)
        self.ready.configure(text=ready)
        if not self.process:
            title = next(t for k, t, _s, _e in GAMES if k == start)
            self.status.configure(text=_('Ready: {}').format(title) if info else _('No game folder yet.'),
                                  fg=MUTED)
        self.draw_hero()

    # ---- actions -----------------------------------------------------------------------------
    def change_ui_language(self):
        """Save and rebuild every label as soon as the interface language changes."""
        global UI_LANGUAGE
        page = self.current_page or 'advanced'
        self.collect()
        UI_LANGUAGE = uncharted_i18n.language_code(self.app['ui_language'])
        for child in self.root.winfo_children():
            child.destroy()
        self.vars = {}
        self.build()
        self.show(page)
        self.refresh()
        if self.process:
            self.play_button.configure(state='disabled')
            self.stop_button.configure(state='normal')

    def configure_art(self):
        import uncharted_art
        uncharted_art.set_custom_art({
            'collection_banner.jpg': self.app.get('art_banner', ''),
            'u1.jpg': self.app.get('art_u1', ''),
            'u2.jpg': self.app.get('art_u2', ''),
            'u3.jpg': self.app.get('art_u3', ''),
        }, {
            'collection_banner.jpg': self.app.get('art_crops', {}).get('banner', {}),
            'u1.jpg': self.app.get('art_crops', {}).get('u1', {}),
            'u2.jpg': self.app.get('art_crops', {}).get('u2', {}),
            'u3.jpg': self.app.get('art_crops', {}).get('u3', {}),
        })

    def quick_art(self, key, crop_key, path):
        """Open a dropped image in the same crop editor used by click selection."""
        try:
            from PIL import Image
            with Image.open(path) as image:
                image.verify()
        except (OSError, ValueError) as error:
            self.messagebox.showerror(NAME, _('This image could not be opened: {}').format(error))
            return
        size = (1600, 450) if key == 'art_banner' else (600, 900)
        title = _('Collection banner') if key == 'art_banner' else _('Change image')
        self.edit_launcher_art(key, title, size, crop_key, str(Path(path)))

    def change_art(self):
        """Apply local artwork immediately while keeping the original as fallback."""
        self.collect()
        self.configure_art()
        self.banner_key = self.banner_scaled = None
        self.card_images = {}
        self.refresh()

    def collect(self):
        for key, v in self.vars.items():
            self.app[key] = v.get()
        if getattr(self, 'pad_vars', None):
            self.app['pad_bindings'] = {a: v.get() for a, v in self.pad_vars.items()}
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        CONFIG_FILE.write_text(json.dumps(self.app, indent=2) + '\n', encoding='utf-8')
        try:
            set_ini('show_fps', '1' if self.app['show_fps'] else '0')
        except OSError:
            pass

    def detect_controller(self, test=False):
        """Detect or briefly rumble the first SDL gamepad without starting the game renderer."""
        if self.process:
            self.controller_status.configure(text=_('Unavailable while the game is running.'))
            return
        probe = controller_probe_path()
        if not probe:
            self.controller_name.configure(text=_('Not available'))
            self.controller_status.configure(text=_('bb-probe.exe was not found.'))
            return
        self.controller_refresh_button.configure(state='disabled')
        self.controller_test_button.configure(state='disabled')
        self.controller_status.configure(text=_('Testing…') if test else _('Detecting…'))
        strength = vibration_strength(self.current())

        def work():
            command = [str(probe), '--controller-test', str(strength)] if test \
                else [str(probe), '--controller-info']
            try:
                result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8',
                                        errors='replace', timeout=5, env=controller_probe_environment(probe),
                                        creationflags=NO_WINDOW)
                if result.returncode:
                    raise OSError(result.stderr.strip() or _('controller probe failed'))
                info, error = parse_controller_probe(result.stdout), ''
            except (OSError, subprocess.TimeoutExpired) as failure:
                info, error = None, str(failure)
            self.ui_calls.put(lambda: self.show_controller(info, error, test))
        threading.Thread(target=work, daemon=True).start()

    def show_controller(self, info, error, test):
        if self.process:
            self.controller_refresh_button.configure(state='disabled')
            self.controller_test_button.configure(state='disabled')
            self.controller_status.configure(text=_('Unavailable while the game is running.'))
            return
        self.controller_refresh_button.configure(state='normal')
        if error:
            self.controller_name.configure(text=_('Not available'))
            self.controller_status.configure(text=_('Could not detect the controller: {}').format(error[:120]))
            return
        if not info['connected']:
            self.controller_name.configure(text=_('No controller detected'))
            self.controller_status.configure(text=_('Connect a controller, then detect again.'))
            return
        self.controller_name.configure(text=info['name'] or _('Game controller'))
        self.controller_test_button.configure(state='normal' if info['rumble'] else 'disabled')
        if not info['rumble']:
            status = _('Connected · Vibration not supported')
        elif test:
            status = _('Connected · Vibration test completed') if info['tested'] \
                else _('Connected · Vibration test failed')
        else:
            status = _('Connected · Vibration supported')
        self.controller_status.configure(text=status)

    def play(self):
        if self.process:
            return
        self.collect()
        folder = game_folder(self.app, self.app['start'])
        if not folder or not Path(folder).is_dir():
            self.messagebox.showerror(NAME, _('Choose the game folder with eboot.bin.'))
            self.show('game')
            return
        if not game_available(self.app, self.app['start']):
            self.messagebox.showerror(NAME, _('This game is not in the chosen folder.'))
            return
        self.set_log('')
        try:
            env = game_environment(self.app)
        except OSError as error:
            self.messagebox.showerror(
                NAME, _('Could not save the input configuration: {}').format(error))
            return
        try:
            self.process = subprocess.Popen(run_command(), cwd=PORT_DIR, env=env,
                                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        except OSError as error:
            self.append(_('Could not start: {}').format(error) + '\n')
            self.process = None
            return
        self.job = GameJob(self.process)
        self.entered, self.last_problem, self.stopping = False, '', False
        threading.Thread(target=self.read_output, args=(self.process,), daemon=True).start()
        self.play_button.configure(state='disabled')
        self.stop_button.configure(state='normal')
        self.controller_refresh_button.configure(state='disabled')
        self.controller_test_button.configure(state='disabled')
        self.status.configure(text=_('Preparing the game; it opens in its own window…'), fg=GOLD)
        if self.app.get('close_on_play'):
            self.root.after(5000, self.root.destroy)

    def read_output(self, process):
        """Helper thread: the game's output to the Log page and to <saves folder>/last_run.log."""
        log = None
        try:
            log_dir = Path(self.app.get('user_dir') or DATA_DIR / 'user')
            log_dir.mkdir(parents=True, exist_ok=True)
            log = open(log_dir / 'last_run.log', 'w', encoding='utf-8', buffering=1)
        except OSError:
            pass
        for raw in iter(process.stdout.readline, b''):
            text = raw.decode('utf-8', errors='replace')
            self.output.put(text)
            if log:
                log.write(text.replace('\r\n', '\n'))
        if log:
            log.close()
        self.output.put((process.wait(),))

    def drain_output(self):
        while not self.ui_calls.empty():
            self.ui_calls.get_nowait()()
        try:
            for _i in range(500):
                item = self.output.get_nowait()
                if isinstance(item, tuple):
                    code = item[0]
                    self.append(_('\n— the game exited (code {}) —\n').format(code))
                    self.process = None
                    if self.job:
                        self.job.close()
                    self.play_button.configure(state='normal')
                    self.stop_button.configure(state='disabled')
                    self.refresh()
                    self.detect_controller()
                    # It failed to start or crashed (not Stop, not closing the window): say why.
                    if not self.stopping and (code != 0 or not self.entered):
                        reason = self.last_problem or _('see the Log page')
                        self.status.configure(text=_('The game stopped: {}').format(reason[:110]),
                                              fg='#e07a4f')
                        self.show('log')
                else:
                    stripped = item.strip()
                    self.last_problem = select_run_problem(self.last_problem, stripped)
                    if 'Entering original x86-64 code' in item:
                        self.entered = True
                        self.status.configure(text=_('The game is running.'), fg=GOLD)
                    elif item.startswith('Switching to '):
                        self.status.configure(text=_('Starting the next game of the collection…'), fg=GOLD)
                    self.append(item)
        except queue.Empty:
            pass
        self.root.after(100, self.drain_output)

    def stop(self):
        if not self.process:
            return
        self.stop_button.configure(state='disabled')
        try:
            if not self.job or not self.job.terminate():
                terminate_process_tree(self.process)
        except (OSError, subprocess.SubprocessError) as error:
            self.stop_button.configure(state='normal')
            self.messagebox.showerror(NAME, _('Could not stop the game: {}').format(error))
            return
        self.stopping = True
        self.status.configure(text=_('Stopping the game…'), fg=GOLD)

    def append(self, text):
        self.log.configure(state='normal')
        self.log.insert('end', text)
        lines = int(self.log.index('end-1c').split('.')[0])
        if lines > MAX_LOG_LINES:
            self.log.delete('1.0', f'{lines - MAX_LOG_LINES}.0')
        self.log.see('end')
        self.log.configure(state='disabled')

    def set_log(self, text):
        self.log.configure(state='normal')
        self.log.delete('1.0', 'end')
        self.log.insert('end', text)
        self.log.configure(state='disabled')

    def copy_log(self):
        self.root.clipboard_clear()
        self.root.clipboard_append(self.log.get('1.0', 'end'))

    def open_path(self, path, key=None):
        if key == 'user_dir' and not path:
            path = DATA_DIR / 'user'
        path = Path(path or DATA_DIR)
        if not path.exists():
            path.mkdir(parents=True, exist_ok=True)
        os.startfile(str(path))

    # ---- updates -------------------------------------------------------------------------------
    def check_update(self, manual=False):
        """Helper thread: asks GitHub for the newest release and offers it when it is newer."""
        try:
            version, url, page = latest_release()
        except (OSError, ValueError, KeyError) as failure:
            if manual:
                self.ui_calls.put(lambda error=failure: self.messagebox.showerror(
                    NAME, _('Could not check for updates: {}').format(error)))
            return

        def show():
            if version_tuple(version) > version_tuple(VERSION):
                self.offer_update(version, url, page)
            elif manual:
                self.messagebox.showinfo(NAME, _('You have the latest version ({}).').format(VERSION))
        self.ui_calls.put(show)

    def offer_update(self, version, url, page):
        if self.update_box:
            return
        tk = self.tk
        text = _('Version {} is available.').format(version)
        box = tk.Frame(self.side, bg=CARD, highlightthickness=1, highlightbackground=GOLD_DIM)
        self.update_label = tk.Label(box, text=text, bg=CARD, fg=GOLD, font=self.font_ui_md,
                                     wraplength=self.px(190), justify='left')
        self.update_label.pack(anchor='w', padx=12, pady=(10, 8))
        buttons = tk.Frame(box, bg=CARD)
        buttons.pack(anchor='w', padx=12, pady=(0, 12))
        self.update_button = self.pill(buttons, _('Update'), lambda: self.install_update(version, url, page),
                                       kind='primary', padding=(14, 6))
        self.update_button.pack(side='left')
        self.pill(buttons, _("What's new"), lambda: webbrowser.open(page), padding=(12, 6)).pack(
            side='left', padx=(6, 0))
        box.pack(fill='x', padx=18, pady=(0, 14), before=self.side_note)
        self.update_box = box
        if not self.process:
            self.status.configure(text=text, fg=GOLD)

    def install_update(self, version, url, page):
        if self.process:
            self.messagebox.showinfo(NAME, _('Close the game before updating.'))
            return
        if not FROZEN or not url:  # a source tree updates with git
            webbrowser.open(page)
            return
        if not self.messagebox.askyesno(NAME, _(
                'Install version {} now? The launcher closes, installs it and opens again. Saves and settings '
                'are kept.').format(version)):
            return
        self.update_button.configure(state='disabled')

        def progress(percent):
            self.update_label.configure(text=_('Downloading version {}… {}%').format(version, percent))

        def work():
            try:
                shutil.rmtree(UPDATE_DIR, ignore_errors=True)
                UPDATE_DIR.mkdir(parents=True, exist_ok=True)
                archive = UPDATE_DIR / 'update.zip'
                request = urllib.request.Request(url, headers={'User-Agent': 'uncharted-launcher'})
                with urllib.request.urlopen(request, timeout=60) as response, open(archive, 'wb') as out:
                    total, done, shown = int(response.headers.get('Content-Length') or 0), 0, -1
                    while chunk := response.read(1 << 20):
                        out.write(chunk)
                        done += len(chunk)
                        percent = done * 100 // total if total else 0
                        if percent != shown:
                            shown = percent
                            self.ui_calls.put(lambda p=percent: progress(p))
                with zipfile.ZipFile(archive) as package:
                    package.extractall(UPDATE_DIR / 'new')
                archive.unlink()
                new = next((p.parent for p in (UPDATE_DIR / 'new').rglob('Uncharted.exe')), None)
                if not new:
                    raise OSError('Uncharted.exe is missing from the download')
                # The new launcher copies itself over this installation once this one has closed.
                subprocess.Popen([str(new / 'Uncharted.exe'), '--install-update', str(PORT_DIR), str(os.getpid())],
                                 cwd=str(new), stdin=subprocess.DEVNULL, creationflags=NO_WINDOW)
                self.ui_calls.put(self.close)
            except (OSError, zipfile.BadZipFile) as failure:
                def failed(error=failure):
                    self.update_button.configure(state='normal')
                    self.update_label.configure(text=_('Version {} is available.').format(version))
                    self.messagebox.showerror(NAME, _('Update failed: {}').format(error))
                self.ui_calls.put(failed)
        threading.Thread(target=work, daemon=True).start()

    def clear_cache(self):
        if self.process:
            return
        cache = Path(self.app['user_dir'] or DATA_DIR / 'user') / 'cache'
        try:
            shutil.rmtree(cache)
        except FileNotFoundError:
            pass
        except OSError as error:
            self.messagebox.showerror(NAME, _('Could not clear the shader cache: {}').format(error))
            return
        self.messagebox.showinfo(NAME, _('Shader cache cleared. The next start stutters for a while as it is rebuilt.'))

    def shortcut(self):
        """Configure the collection shortcut where the action happens."""
        from PIL import Image, ImageTk

        window = self.tk.Toplevel(self.root)
        window.title(_('Desktop shortcut'))
        window.configure(bg=PANEL)
        window.transient(self.root)
        window.grab_set()
        window.resizable(False, False)
        shell = self.tk.Frame(window, bg=PANEL)
        shell.pack(padx=self.px(24), pady=self.px(22))
        self.tk.Label(shell, text=_('Desktop shortcut'), bg=PANEL, fg=TEXT,
                      font=self.font_title).grid(row=0, column=0, columnspan=2, sticky='w')
        self.tk.Label(shell, text=_('Shortcut name'), bg=PANEL, fg=TEXT,
                      font=self.font_ui).grid(row=1, column=0, sticky='w', pady=(18, 4))
        name = self.tk.StringVar(value=self.app.get('shortcut_name', APP_DEFAULTS['shortcut_name']))
        self.ttk.Entry(shell, textvariable=name, width=48).grid(row=2, column=0, columnspan=2, sticky='ew')
        self.tk.Label(shell, text=_('Shortcut icon'), bg=PANEL, fg=TEXT,
                      font=self.font_ui).grid(row=3, column=0, sticky='w', pady=(16, 4))
        icon = self.tk.StringVar(value=self.app.get('shortcut_icon', ''))
        preview = self.tk.Canvas(shell, width=self.px(112), height=self.px(112), bg=BG,
                                 highlightthickness=2, highlightbackground=GOLD_DIM, cursor='hand2')
        preview.grid(row=4, column=0, sticky='w')
        photo = {'image': None}

        def draw_icon(*_args):
            preview.delete('all')
            try:
                with Image.open(icon.get()) as source:
                    image = source.convert('RGBA')
                    image.thumbnail((self.px(96), self.px(96)), Image.LANCZOS)
                photo['image'] = ImageTk.PhotoImage(image)
                preview.create_image(self.px(56), self.px(56), image=photo['image'])
            except (OSError, ValueError, self.tk.TclError):
                photo['image'] = None
                preview.create_text(self.px(56), self.px(56), text=_('Original icon'), fill=MUTED,
                                    font=self.font_ui_sm, width=self.px(86))

        def choose_icon(candidate=''):
            chosen = candidate or self.image_path(_('Shortcut icon'), icon.get())
            if chosen:
                try:
                    with Image.open(chosen) as image:
                        image.verify()
                except (OSError, ValueError) as error:
                    self.messagebox.showerror(NAME, _('This image could not be opened: {}').format(error))
                    return
                icon.set(chosen)

        actions = self.tk.Frame(shell, bg=PANEL)
        actions.grid(row=4, column=1, sticky='w', padx=(16, 0))
        self.pill(actions, _('Choose image'), choose_icon, background=PANEL).pack(anchor='w')
        self.pill(actions, _('Restore'), lambda: icon.set(''), kind='ghost', background=PANEL).pack(
            anchor='w', pady=(6, 0))
        buttons = self.tk.Frame(shell, bg=PANEL)
        buttons.grid(row=5, column=0, columnspan=2, sticky='ew', pady=(18, 0))
        self.pill(buttons, _('Cancel'), window.destroy, kind='ghost', background=PANEL).pack(side='right')

        def create():
            self.app['shortcut_name'] = name.get().strip() or APP_DEFAULTS['shortcut_name']
            self.app['shortcut_icon'] = icon.get()
            self.collect()
            window.destroy()
            self.create_shortcut()

        self.pill(buttons, _('Create shortcut'), create, kind='primary', background=PANEL).pack(
            side='right', padx=(0, 6))
        preview.bind('<Button-1>', lambda _event: choose_icon())
        self.enable_image_drop(preview, choose_icon)
        icon.trace_add('write', draw_icon)
        draw_icon()
        window.bind('<Escape>', lambda _event: window.destroy())

    def create_shortcut(self):
        """Create the configured .lnk on the desktop."""
        if FROZEN:
            target, arguments, icon = sys.executable, '', f'{sys.executable},0'
        else:
            pythonw = Path(sys.executable).with_name('pythonw.exe')
            target = str(pythonw if pythonw.exists() else sys.executable)
            arguments, icon = Path(__file__).resolve(), PORT_DIR / 'launcher' / 'uncharted.ico'
        custom_icon = self.app.get('shortcut_icon', '')
        if custom_icon:
            try:
                from PIL import Image
                CONFIG_DIR.mkdir(parents=True, exist_ok=True)
                icon = CONFIG_DIR / 'custom-shortcut.ico'
                with Image.open(custom_icon) as image:
                    image.convert('RGBA').save(icon, sizes=[(16, 16), (32, 32), (48, 48), (256, 256)])
            except OSError as error:
                self.messagebox.showerror(NAME, _('This image could not be opened: {}').format(error))
                return
        name = re.sub(r'[<>:"/\\|?*]', '', self.app.get('shortcut_name', '')).strip() or 'Uncharted'
        ps = lambda value: "'" + str(value).replace("'", "''") + "'"
        script = ('$desktop=[Environment]::GetFolderPath("Desktop");'
                  f'$s=(New-Object -ComObject WScript.Shell).CreateShortcut($desktop+{ps(chr(92) + name + ".lnk")});'
                  f'$s.TargetPath={ps(target)};$s.WorkingDirectory={ps(PORT_DIR)};$s.IconLocation={ps(icon)};'
                  + (f'$s.Arguments={ps(chr(34) + str(arguments) + chr(34))};' if arguments else '') + '$s.Save()')
        result = subprocess.run(['powershell', '-NoProfile', '-Command', script], capture_output=True,
                                creationflags=NO_WINDOW)
        if result.returncode == 0:
            self.messagebox.showinfo(NAME, _('Shortcut created on the desktop.'))
        else:
            self.messagebox.showerror(NAME, result.stderr.decode(errors='replace')[:400])

    def add_to_steam(self):
        """Preview the collection art before writing the non-Steam shortcut."""
        from PIL import Image, ImageTk
        import uncharted_art

        window = self.tk.Toplevel(self.root)
        window.title(_('Add to Steam'))
        window.configure(bg=PANEL)
        window.transient(self.root)
        window.grab_set()
        window.resizable(False, False)
        shell = self.tk.Frame(window, bg=PANEL)
        shell.pack(padx=self.px(24), pady=self.px(22))
        self.tk.Label(shell, text=_('Add collection to Steam'), bg=PANEL, fg=TEXT,
                      font=self.font_title).grid(row=0, column=0, columnspan=3, sticky='w')
        self.tk.Label(shell, text=_('Use one image and adjust each Steam format separately.'), bg=PANEL,
                      fg=MUTED, font=self.font_ui).grid(row=1, column=0, columnspan=3, sticky='w', pady=(2, 14))
        formats = [('portrait', _('Portrait'), (600, 900), (150, 225)),
                   ('header', _('Header'), (920, 430), (230, 108)),
                   ('hero', _('Hero'), (1920, 620), (300, 97))]
        previews = {}
        photos = {}
        selected = {'path': self.app.get('steam_art', ''),
                    'crops': {name: dict(self.app.get('art_crops', {}).get(f'steam_{name}', {}))
                              for name, _label, _size, _preview in formats}}

        def redraw():
            for column, (name, label, size, preview_size) in enumerate(formats):
                canvas = previews[name]
                canvas.delete('all')
                try:
                    with Image.open(selected['path']) as source:
                        image = uncharted_art.crop_image(
                            source.convert('RGB'), preview_size, selected['crops'][name])
                    photos[name] = ImageTk.PhotoImage(image)
                    canvas.create_image(0, 0, image=photos[name], anchor='nw')
                except (OSError, ValueError, self.tk.TclError):
                    canvas.create_text(preview_size[0] // 2, preview_size[1] // 2,
                                       text=_('Choose image'), fill=MUTED, font=self.font_ui_sm)
                canvas.create_rectangle(1, 1, preview_size[0] - 2, preview_size[1] - 2,
                                        outline=GOLD_DIM, width=2)

        def choose(candidate=''):
            chosen = candidate or self.image_path(_('Collection image'), selected['path'])
            if chosen:
                try:
                    with Image.open(chosen) as image:
                        image.verify()
                except (OSError, ValueError) as error:
                    self.messagebox.showerror(NAME, _('This image could not be opened: {}').format(error))
                    return
                selected['path'] = chosen
                redraw()

        def adjust(name, label, size):
            result = self.edit_art('steam_art', label, size, f'steam_{name}',
                                   initial_path=selected['path'], initial_crop=selected['crops'][name])
            if result:
                selected['path'], selected['crops'][name] = result
                redraw()

        for column, (name, label, size, preview_size) in enumerate(formats):
            cell = self.tk.Frame(shell, bg=PANEL)
            cell.grid(row=2, column=column, sticky='n', padx=(0 if column == 0 else 16, 0))
            self.tk.Label(cell, text=label, bg=PANEL, fg=TEXT, font=self.font_ui_md).pack(anchor='w')
            canvas = self.tk.Canvas(cell, width=preview_size[0], height=preview_size[1], bg=BG,
                                    highlightthickness=0, cursor='hand2')
            canvas.pack(pady=(6, 8))
            previews[name] = canvas
            canvas.bind('<Button-1>', lambda _e, n=name, l=label, s=size: adjust(n, l, s))
            self.enable_image_drop(canvas, choose)
            self.pill(cell, _('Adjust crop'), lambda n=name, l=label, s=size: adjust(n, l, s),
                      background=PANEL, padding=(12, 6)).pack(anchor='w')
        footer = self.tk.Frame(shell, bg=PANEL)
        footer.grid(row=3, column=0, columnspan=3, sticky='ew', pady=(18, 0))
        self.pill(footer, _('Choose image'), choose, background=PANEL).pack(side='left')
        self.pill(footer, _('Cancel'), window.destroy, kind='ghost', background=PANEL).pack(side='right')

        def add():
            self.app['steam_art'] = selected['path']
            self.app.setdefault('art_crops', {}).update(
                {f'steam_{name}': crop for name, crop in selected['crops'].items()})
            self.collect()
            window.destroy()
            self.perform_add_to_steam()

        self.pill(footer, _('Add to Steam'), add, kind='primary', background=PANEL).pack(side='right', padx=(0, 6))
        redraw()
        window.bind('<Escape>', lambda _event: window.destroy())

    def perform_add_to_steam(self):
        """Write the collection shortcut and its user-selected artwork to Steam."""
        import bbport_steam
        steam = bbport_steam.steam_folder()
        configs = bbport_steam.user_configs(steam) if steam else []
        if not configs:
            self.messagebox.showerror(NAME, _('Steam was not found on this PC (or nobody has signed in to it yet).'))
            return
        if FROZEN:
            exe, options = Path(sys.executable), '--play'
        else:
            pythonw = Path(sys.executable).with_name('pythonw.exe')
            exe, options = (pythonw if pythonw.exists() else Path(sys.executable)), f'"{Path(__file__).resolve()}" --play'
        self.collect()
        restart = False
        if bbport_steam.steam_running():
            if not self.messagebox.askyesno(NAME, _('Steam has to close while the game is added; it starts '
                                                    'again afterwards. Close Steam now?')):
                return
            subprocess.Popen([str(steam / 'steam.exe'), '-shutdown'], creationflags=NO_WINDOW)
            for _i in range(60):
                time.sleep(0.5)
                if not bbport_steam.steam_running():
                    break
            else:
                self.messagebox.showerror(NAME, _('Steam did not close. Close it yourself and try again.'))
                return
            time.sleep(1)
            restart = True
        title = 'Uncharted: The Nathan Drake Collection'
        try:
            for config in configs:
                bbport_steam.add_shortcut(config / 'shortcuts.vdf', title, str(exe), str(PORT_DIR), str(exe), options)
                # Library pictures (cover, header, hero) from the box art of the player's game folder.
                try:
                    import uncharted_art
                    uncharted_art.steam_artwork(
                        game_folder(self.app, 'u1') or game_folder(self.app, 'u2'), config / 'grid',
                        bbport_steam.shortcut_id(f'"{exe}"', title), self.app.get('steam_art', ''), {
                            name: self.app.get('art_crops', {}).get(f'steam_{name}', {})
                            for name in ('portrait', 'header', 'hero')})
                except Exception:  # pictures are a nicety: the shortcut works without them
                    pass
        except (OSError, ValueError) as error:
            self.messagebox.showerror(NAME, _('Could not add the game to Steam: {}').format(error))
            return
        if restart:
            os.startfile('steam://open/games')
        self.messagebox.showinfo(NAME, _('The collection is in your Steam library. It starts with the settings '
                                         'saved in this launcher.'))

    def close(self):
        try:
            self.collect()
        except Exception:  # never keep the window open over a settings problem
            pass
        self.root.destroy()


class GameJob:
    """A Windows job holding run.py and everything it starts (the game, the next game of the
    collection, an in-game restart), so Stop ends all of them."""

    def __init__(self, process):
        self.kernel32 = ctypes.windll.kernel32
        self.kernel32.CreateJobObjectW.restype = ctypes.c_void_p
        self.handle = self.kernel32.CreateJobObjectW(None, None)
        if self.handle:
            assigned = self.kernel32.AssignProcessToJobObject(
                ctypes.c_void_p(self.handle), ctypes.c_void_p(int(process._handle)))
            if not assigned:
                self.kernel32.CloseHandle(ctypes.c_void_p(self.handle))
                self.handle = None

    def terminate(self):
        return bool(self.handle and self.kernel32.TerminateJobObject(ctypes.c_void_p(self.handle), 1))

    def close(self):
        if self.handle:
            self.kernel32.CloseHandle(ctypes.c_void_p(self.handle))
            self.handle = None


def terminate_process_tree(process):
    """Stop a launcher process and every child when its Windows job is unavailable."""
    if process.poll() is not None:
        return
    taskkill = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32/taskkill.exe'
    result = subprocess.run([str(taskkill), '/PID', str(process.pid), '/T', '/F'],
                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, creationflags=NO_WINDOW, timeout=10)
    if result.returncode and process.poll() is None:
        detail = result.stdout.decode(errors='replace').strip()
        raise OSError(detail or _('taskkill exited with code {}').format(result.returncode))


def version_tuple(text):
    return tuple(int(number) for number in re.findall(r'\d+', text or ''))


def latest_release():
    """(version, zip URL, page URL) of the newest GitHub release."""
    request = urllib.request.Request(RELEASES_API, headers={'Accept': 'application/vnd.github+json',
                                                            'User-Agent': 'uncharted-launcher'})
    with urllib.request.urlopen(request, timeout=15) as response:
        release = json.load(response)
    version = '.'.join(re.findall(r'\d+', release['tag_name']))
    url = next((asset['browser_download_url'] for asset in release.get('assets', [])
                if asset.get('name', '').lower().endswith('.zip')), None)
    return version, url, release.get('html_url') or RELEASES_PAGE


def install_update(target, wait_pid):
    """--install-update TARGET PID, run by the downloaded version from its temporary folder:
    waits for the old launcher to close, copies this version over TARGET (never the saves,
    settings or art) and starts it."""
    target = Path(target)
    kernel = ctypes.windll.kernel32
    handle = kernel.OpenProcess(0x00100000, False, int(wait_pid))  # SYNCHRONIZE
    if handle:
        kernel.WaitForSingleObject(handle, 60000)
        kernel.CloseHandle(handle)
    ignore = shutil.ignore_patterns(*USER_FILES)
    for attempt in range(30):
        try:
            shutil.copytree(PORT_DIR, target, dirs_exist_ok=True, ignore=ignore)
            break
        except OSError:  # a file still in use: the old launcher is closing
            time.sleep(1)
    else:
        ctypes.windll.user32.MessageBoxW(None, _('Could not install the update. Download it from the '
                                                 'releases page.'), NAME, 0x10)
        webbrowser.open(RELEASES_PAGE)
        return 1
    subprocess.Popen([str(target / 'Uncharted.exe')], cwd=str(target))
    return 0


def play_without_window(settings):
    """--play: the game with the saved settings (shortcuts, Steam); output to the console when
    there is one and to <saves folder>/last_run.log."""
    attach_stdio()
    log_dir = Path(settings.get('user_dir') or DATA_DIR / 'user')
    log_dir.mkdir(parents=True, exist_ok=True)
    with open(log_dir / 'last_run.log', 'w', encoding='utf-8', buffering=1) as log:
        try:
            env = game_environment(settings)
        except OSError as error:
            text = _('Could not save the input configuration: {}').format(error) + '\n'
            log.write(text)
            try:
                sys.stderr.write(text)
            except (OSError, ValueError):
                pass
            return 1
        process = subprocess.Popen(run_command(), cwd=PORT_DIR, env=env,
                                   stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        for raw in iter(process.stdout.readline, b''):
            text = raw.decode('utf-8', errors='replace')
            log.write(text)
            try:
                sys.stdout.write(text)
            except (OSError, ValueError):
                pass
        return process.wait()


def requested_game(args):
    """The game named on the command line (--game u2, --game=u2 or --u2), or None."""
    keys = [key for key, _title, _sub, _exe in GAMES]
    for index, arg in enumerate(args):
        if arg in ('--game', '-g') and index + 1 < len(args):
            name = args[index + 1]
        elif arg.startswith('--game='):
            name = arg[len('--game='):]
        elif arg.startswith('--'):
            name = arg[2:]
        else:
            continue
        if name.lower() in keys:
            return name.lower()
    return None


def main():
    args = sys.argv[1:]
    if args and args[0] in ('--run', '--script'):
        sys.exit(run_role(args))
    if args[:1] == ['--install-update'] and len(args) == 3:
        sys.exit(install_update(args[1], args[2]))
    if FROZEN and UPDATE_DIR not in PORT_DIR.parents:
        shutil.rmtree(UPDATE_DIR, ignore_errors=True)  # what a finished update left behind
    settings = {**APP_DEFAULTS, **load_json(CONFIG_FILE, {})}
    game = requested_game(args)
    if game:  # Uncharted.exe --u2: that game, no window (the same as --play with it as the start game)
        settings['start'] = game
    if ('--play' in args or game) and game_available(settings, settings['start']):
        sys.exit(play_without_window(settings))
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (AttributeError, OSError):
        pass
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    try:
        from tkinterdnd2 import TkinterDnD
        root = TkinterDnD.Tk()
    except (ImportError, tk.TclError):
        root = tk.Tk()
    Launcher(root, tk, ttk, filedialog, messagebox)
    root.mainloop()


if __name__ == '__main__':
    main()
