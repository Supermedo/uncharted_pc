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
DATA_DIR = Path(os.environ.get('BB_DATA_DIR', PORT_DIR))
CONFIG_DIR = Path(os.environ.get('APPDATA', Path.home())) / 'uncharted-launcher'
CONFIG_FILE = CONFIG_DIR / 'settings.json'
NAME = 'Uncharted'
AUTHOR = 'SuperMedo'
# This build; GitHub release tags are v<VERSION> (their digits are compared).
VERSION = '0.3.6'
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
# Each game's accent, from its world: Drake's Fortune's jungle, Among Thieves' Himalayan ice,
# Drake's Deception's Rub' al Khali sand.
GAME_ACCENTS = {'u1': '#a9cb86', 'u2': '#a6cbe2', 'u3': '#e8b06a'}
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
FRAME_RATES = [('60', '60 FPS (as on PS4)'), ('120', '120 FPS'), ('144', '144 FPS (experimental)'),
               ('165', '165 FPS (experimental)'), ('240', '240 FPS (experimental)')]
# A cap below the frame rate (BB_FPS_LIMIT, with the display-rate wait off: BB_VBLANK_HZ=0): an
# even, steady rate on PCs that can't keep up.
FPS_LIMITS = [('', 'Off'), ('30', '30 FPS'), ('40', '40 FPS'), ('45', '45 FPS'), ('50', '50 FPS')]
# The games render 1920x1080; the window (or the screen in fullscreen) gets that image scaled
# to its size: FSR 1 (EASU) or bilinear, then RCAS sharpening (BB_RCAS_ATTENUATION, lower
# is sharper).
WINDOW_SIZES = [('1280x720', '1280 × 720'), ('1600x900', '1600 × 900'),
                ('1920x1080', "1920 × 1080 (the games' own)"), ('2560x1440', '2560 × 1440'),
                ('3840x2160', '3840 × 2160 (4K)')]
SCALERS = [('fsr1', 'FSR 1 (AMD FidelityFX, sharp)'), ('bilinear', 'Smooth (bilinear)')]
SHARPENING = [('off', 'Off'), ('1000', 'Low'), ('500', 'Medium'), ('250', 'High'), ('0', 'Maximum')]
# Temporal anti-aliasing at the games' 1920x1080 (BB_UPSCALER): it takes the scene before the
# games' post-processing with their own motion vectors, so it replaces their edge smoothing.
ANTI_ALIASING = [('off', "Off (the game's own)"), ('taa', 'TAA'), ('fsr3', 'AMD FSR 3.1 (Native AA)'),
                 ('fsr4', 'AMD FSR 4 (Native AA)'), ('dlss', 'NVIDIA DLSS (DLAA, GeForce RTX)')]
FSR411 = ('fsr411', 'AMD FSR 4.1.1 (Native AA)')
AA_SHARPNESS = [('0', 'Off'), ('0.2', 'Low'), ('0.4', 'Medium'), ('0.7', 'High')]
# Frames generated between two of the game's (BB_FRAME_GEN, BB_FRAME_GEN_MULTIPLIER).
FRAME_GENERATION = [('off', 'Off'), ('fsr3', 'AMD FSR 3 Frame Generation (any GPU)'),
                    ('dlss', 'NVIDIA DLSS Frame Generation (GeForce RTX 40/50)')]
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
MOUSE_SENS_MIN, MOUSE_SENS_MAX = 0.1, 10.0  # the Controls page slider (the game's default pace is 1)
# Insert opens the in-game settings menu; Left Alt frees the mouse cursor.
RESERVED_KEYS = {'Insert': 'Insert opens the in-game settings menu.',
                 'Alt_L': 'Left Alt frees the mouse cursor while held.'}

APP_DEFAULTS = {'game_dir': '', 'game_dir_u1': '', 'game_dir_u2': '', 'game_dir_u3': '', 'user_dir': '', 'start': 'u1', 'language': '1', 'player_name': '',
                'fullscreen': False, 'present_mode': 'Mailbox', 'frame_rate': '60', 'fps_limit': '', 'show_fps': False,
                'window_size': '1920x1080', 'scaler': 'fsr1', 'sharpening': '500',
                'anti_aliasing': 'off', 'aa_sharpness': '0.2', 'frame_generation': 'off',
                'frame_gen_multiplier': '2', 'extra_env': '', 'close_on_play': False,
                'mouse_look': True, 'mouse_sensitivity': '1.0', 'mouse_invert_y': False,
                'pad_invert_y': False, 'key_bindings': {}, 'pad_bindings': {}, 'check_updates': True}


def _(text):
    """Translations come later; every text goes through here."""
    return text


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
             f"pad_invert_y={1 if s.get('pad_invert_y') else 0}"]
    lines += [f'key.{action}={keys.get(action, "")}' for action in DEFAULT_KEYS]
    lines += [f'pad.{action}={pad.get(action, "none")}' for action in DEFAULT_PAD]
    path = input_ini_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return path


def game_environment(s, start=None):
    env = dict(os.environ)
    try:
        env['BB_INPUT_CONFIG'] = str(write_input_ini(s))
    except OSError:
        pass
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
    if str(s.get('fps_limit') or '').isdigit():
        env['BB_VBLANK_HZ'] = '0'
        env['BB_FPS_LIMIT'] = str(s['fps_limit'])
    env['BB_WINDOW_SIZE'] = str(s.get('window_size') or '1920x1080')
    env['BB_FSR1'] = '1' if s.get('scaler', 'fsr1') == 'fsr1' else '0'
    sharpening = str(s.get('sharpening') or 'off')
    env['BB_RCAS'] = '0' if sharpening == 'off' else '1'
    if sharpening != 'off':
        env['BB_RCAS_ATTENUATION'] = sharpening
    env['BB_UPSCALER'] = str(s.get('anti_aliasing') or 'off')
    env['BB_FSR_SHARPNESS'] = str(s.get('aa_sharpness') or '0')
    env['BB_FRAME_GEN'] = str(s.get('frame_generation') or 'off')
    env['BB_FRAME_GEN_MULTIPLIER'] = str(s.get('frame_gen_multiplier') or '2')
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
          'gold': GOLD, 'gold_hi': GOLD_HI, 'gold_dim': GOLD_DIM, 'track': '#2c2e33', 'knob': '#9a958b', 'knob_on': '#f7f1e2',
          'button': '#25272b', 'button_hi': '#30333a'}
# Menu row icons (Segoe Fluent Icons on Windows 11, Segoe MDL2 Assets on Windows 10).
NAV_ICONS = {'play': '\ue80f', 'game': '\ue8f1', 'display': '\ue7f4', 'controls': '\ue7fc',
             'advanced': '\ue90f', 'log': '\ue9d9'}
RUST, RUST_HI = GOLD, GOLD_HI
# The menu row on every page (home and the settings pages) and the home screen's tile row.
HOME_MENU = [('play', 'Home'), ('game', 'Game folder'), ('display', 'Display and graphics'),
             ('controls', 'Controls'), ('advanced', 'Advanced'), ('log', 'Log')]
HOME_TILES = [key for key, _t, _s, _e in GAMES] + ['settings']


def rgb(color):
    """'#rrggbb' as an (r, g, b) tuple for Pillow."""
    return tuple(int(color[i:i + 2], 16) for i in (1, 3, 5))


class Launcher:
    def __init__(self, root, tk, ttk, filedialog, messagebox):
        self.tk, self.ttk, self.filedialog, self.messagebox = tk, ttk, filedialog, messagebox
        self.root = root
        self.app = {**APP_DEFAULTS, **load_json(CONFIG_FILE, {})}
        self.vars = {}
        self.process = self.job = None
        self.entered, self.last_problem, self.stopping = False, '', False
        self.output = queue.Queue()
        self.ui_calls = queue.Queue()
        self.banner_key = self.banner_scaled = self.cover_image = None
        self.hero_video = self.video_photo = None
        self.tile_clip = self.tile_clip_key = self.tile_photo = None
        self.video_masks, self.clip_masks = {}, {}
        try:
            from uncharted_media import HoverMusic
            self.music = HoverMusic(root, CONFIG_DIR / 'media-cache')
        except Exception:
            self.music = None
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
        """Cinzel for the brand (Uncharted-style capitals); M PLUS 1p (close to the PS3 system
        font) for game names and headings; Segoe UI for body text."""
        from uncharted_fonts import font_path

        self.brand_family = 'Cinzel'
        for name in ('Cinzel-Regular.ttf', 'Cinzel-SemiBold.ttf', 'Cinzel-Bold.ttf',
                     'MPLUS1p-Regular.ttf', 'MPLUS1p-Medium.ttf'):
            path = font_path(name)
            if path:
                try:
                    ctypes.windll.gdi32.AddFontResourceExW(str(path), 0x10, 0)
                except (AttributeError, OSError):
                    pass
        if not font_path('Cinzel-Bold.ttf'):
            self.brand_family = 'Georgia'
        # GDI lists the medium weight as a family of its own.
        self.ps_family = 'M PLUS 1p' if font_path('MPLUS1p-Regular.ttf') else 'Segoe UI'
        self.ps_medium = 'M PLUS 1p Medium' if font_path('MPLUS1p-Medium.ttf') else 'Segoe UI Semibold'
        self.font_brand_xl = (self.brand_family, 24, 'bold')
        self.font_brand_lg = (self.brand_family, 18, 'bold')
        self.font_brand_md = (self.brand_family, 13, 'bold')
        self.font_brand_sm = (self.brand_family, 10)
        self.font_game = (self.ps_medium, 13)
        self.font_game_sub = (self.ps_family, 9)
        self.font_hero = (self.ps_medium, 21)
        self.font_hero_sub = (self.ps_family, 10)
        self.font_eyebrow = (self.ps_medium, 9)
        self.font_ui = ('Segoe UI', 10)
        self.font_ui_sm = ('Segoe UI', 9)
        self.font_ui_md = ('Segoe UI Semibold', 10)
        self.font_button = ('Segoe UI Semibold', 10)
        self.font_play = ('Segoe UI Semibold', 13)
        self.font_title = (self.ps_medium, 22)
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
        s.configure('Section.TLabel', background=SURFACE, foreground=GOLD_DIM, font=(self.ps_medium, 11))
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

    def choice(self, parent, key, options, title, hint=None, variable=None):
        """A setting with a list of values (`variable`, else the setting `key`)."""
        from uncharted_settings import ChoiceRow
        lst = self.lists[parent]
        return lst.add(ChoiceRow(lst, title, variable or self.var(key), [(v, _(t)) for v, t in options], hint))

    def toggle(self, parent, variable, background=CARD):
        from uncharted_widgets import Toggle
        return Toggle(self.tk, parent, variable, COLORS, self.px, background)

    def pill(self, parent, text, command, kind='secondary', background=CARD, padding=(16, 8), icon=None):
        from uncharted_widgets import PillButton
        return PillButton(self.tk, parent, text, command, COLORS, self.px,
                          self.font_play if kind == 'primary' and icon else self.font_button, kind,
                          background, padding, icon, self.font_icon_sm)

    def row(self, parent, title, widget, hint=None):
        """A row with its own controls (made as children of `parent`, the page's list)."""
        from uncharted_settings import WidgetRow
        lst = self.lists[parent]
        return lst.add(WidgetRow(lst, title, widget, hint))

    def check(self, parent, key, title, hint=None):
        from uncharted_settings import ToggleRow
        lst = self.lists[parent]
        return lst.add(ToggleRow(lst, title, self.var(key), hint,
                                 lambda master, var, bg: self.toggle(master, var, bg)))

    def action(self, parent, title, command, hint=None, value=''):
        from uncharted_settings import ActionRow
        lst = self.lists[parent]
        return lst.add(ActionRow(lst, title, command, hint, value))

    def note(self, parent, text):
        self.lists[parent].note(text)

    def section(self, parent, title, top=22):
        self.lists[parent].heading(title, top)

    def folder(self, parent, key, title, prompt, hint=None, on_change=None):
        """A folder setting: the row shows it; Enter or a click chooses another."""
        var = self.var(key)

        def browse():
            chosen = self.filedialog.askdirectory(title=prompt, initialdir=var.get() or str(PORT_DIR))
            if chosen:
                var.set(str(Path(chosen)))
        row = self.action(parent, title, browse, hint)

        def show(*_args):
            path = var.get()
            text = path if len(path) <= 56 else '…' + path[-55:]
            row.value.configure(text=text or _('Not set'), fg='#b9b2a5' if path else MUTED)
        show()
        var.trace_add('write', show)
        if on_change:
            var.trace_add('write', lambda *_a: on_change())
        return row

    def page(self, name, title, subtitle):
        """A settings page: its title, then a console-style list (uncharted_settings). Returns the
        list's frame, the parent for rows and the controls placed into them."""
        from uncharted_settings import SettingsList
        tk = self.tk
        outer = tk.Frame(self.content, bg=SURFACE)
        head = tk.Frame(outer, bg=SURFACE)
        head.pack(fill='x', padx=self.px(20), pady=(self.px(22), self.px(12)))
        tk.Label(head, text=title, bg=SURFACE, fg=TEXT, font=self.font_title).pack(anchor='w')
        tk.Label(head, text=subtitle, bg=SURFACE, fg=MUTED, font=self.font_ui).pack(anchor='w', pady=(2, 0))
        fonts = {'row': (self.ps_family, 11), 'value': (self.ps_family, 10), 'hint': ('Segoe UI', 9),
                 'heading': (self.ps_medium, 10), 'icon': self.font_icon_sm}
        lst = SettingsList(tk, self.ttk, outer, COLORS, self.px, fonts)
        lst.outer.pack(fill='both', expand=True)
        self.lists[lst.inner] = self.lists[name] = lst
        self.scrollers[name] = (lst.canvas, lst.bar)
        self.pages[name] = outer
        return lst.inner

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
        """One canvas over the whole window (build_play) holds every page: the home screen, or a
        settings page placed on it under the same menu row, over the same (darker) background."""
        tk = self.tk
        self.current_page, self.brand_image = None, None
        try:
            # The logo-style heading (home screen): spaced gold capitals over the collection's name.
            from PIL import ImageTk
            import uncharted_art
            self.brand_image = ImageTk.PhotoImage(uncharted_art.brand_heading(self.px(240), self.dpi))
        except Exception:
            pass
        self.pages, self.scrollers, self.lists = {}, {}, {}
        self.root.bind_all('<MouseWheel>', self.on_wheel)
        self.root.bind_class('TCombobox', '<MouseWheel>', self.on_wheel)
        self.build_play()
        # The settings pages go into `content`; the status, Stop and Play along their bottom.
        self.content = tk.Frame(self.stage, bg=SURFACE)
        self.dock = tk.Frame(self.stage, bg=BG)
        self.status = tk.Label(self.dock, text='', bg=BG, fg=MUTED, font=('Segoe UI', 10), anchor='w',
                               justify='left')
        self.status.pack(side='left', padx=(self.px(56), 0))
        self.play_button = self.pill(self.dock, _('Play'), self.play, kind='primary', background=BG,
                                     padding=(30, 8), icon='\ue768' if self.font_icon else None)
        self.play_button.pack(side='right', padx=(self.px(10), self.px(56)))
        self.stop_button = self.pill(self.dock, _('Stop'), self.stop, background=BG, padding=(24, 8),
                                     icon='\ue71a' if self.font_icon else None)
        self.stop_button.configure(state='disabled')
        self.stop_button.pack(side='right')
        self.build_game()
        self.build_display()
        self.build_controls()
        self.build_advanced()
        self.build_log()
        self.stage.pack(fill='both', expand=True)
        self.refresh()

    def show(self, name):
        """'play' is the home screen; the others are settings pages under the same menu row."""
        for page in self.pages.values():
            page.pack_forget()
        self.current_page = name
        self.menu_index = next(i for i, (n, _t) in enumerate(HOME_MENU) if n == name)
        self.home_row = 1
        if name == 'play':
            self.refresh()
            key = HOME_TILES[self.tile_index]
            if self.music and not self.process and key != 'settings':
                self.music.hover(key)
        else:
            self.pages[name].pack(fill='both', expand=True)
            self.quiet_media()
            self.draw_stage()

    def quiet_media(self):
        """No music or cover video, and the header video paused, away from the Play page or while
        a game runs."""
        if self.music:
            self.music.stop()
        if self.hero_video:
            self.hero_video.pause(True)
        self.stop_tile_clip()

    def stop_tile_clip(self):
        if self.tile_clip:
            self.tile_clip.stop()
        self.tile_clip = self.tile_clip_key = self.tile_photo = None

    def build_play(self):
        """The home screen: one canvas over the whole window (the settings view is hidden while it
        shows), console style: a menu row at the top, the tile row (the three games, Settings),
        the focused game's name and Start, and the window options along the bottom."""
        tk = self.tk
        import tkinter.font as tkfont
        self.font_home_title = (self.ps_medium, 26)
        self.measure_play = tkfont.Font(root=self.root, font=self.font_play).measure
        self.stage = tk.Canvas(self.root, bg=BG, highlightthickness=0, bd=0)
        self.stage.bind('<Configure>', lambda _e: self.schedule_stage())
        self.stage.bind('<Motion>', lambda e: self.home_hover(self.hit_at(e.x, e.y)))
        self.stage.bind('<Leave>', lambda _e: self.home_hover(None))
        self.stage.bind('<Button-1>', lambda e: self.home_click(self.hit_at(e.x, e.y)))
        self.stage.bind('<Double-Button-1>', lambda e: self.home_click(self.hit_at(e.x, e.y), True))
        for sequence, action in (('<Left>', (0, -1)), ('<Right>', (0, 1)), ('<Up>', (-1, 0)),
                                 ('<Down>', (1, 0)), ('<Return>', 'enter'), ('<KP_Enter>', 'enter'),
                                 ('<Escape>', 'back')):
            self.root.bind(sequence, lambda _e, a=action: self.home_key(a))
        # Fullscreen, Add to Steam and Desktop shortcut along the bottom.
        self.home_options = tk.Frame(self.stage, bg=BG)
        self.toggle(self.home_options, self.var('fullscreen'), BG).pack(side='left')
        fullscreen = tk.Label(self.home_options, text=_('Fullscreen'), bg=BG, fg=TEXT, font=self.font_ui,
                              cursor='hand2')
        fullscreen.pack(side='left', padx=(self.px(10), 0))
        fullscreen.bind('<Button-1>', lambda _e: self.var('fullscreen').set(not self.var('fullscreen').get()))
        self.pill(self.home_options, _('Add to Steam'), self.add_to_steam, background=BG, padding=(14, 6)).pack(
            side='left', padx=(self.px(24), 0))
        self.pill(self.home_options, _('Desktop shortcut'), self.shortcut, background=BG, padding=(14, 6)).pack(
            side='left', padx=(self.px(8), 0))
        self.home_hits, self.home_hot, self.home_images = [], None, {}
        self.ready_text, self.clock_item, self.stage_job = '', None, None
        start = self.var('start').get()
        self.home_row, self.menu_index = 1, 0
        self.tile_index = HOME_TILES.index(start) if start in HOME_TILES else 0
        self.pictures, self.pictures_size, self.fade_job, self.fade_start = {}, None, None, 0.0
        self.back_photo = self.back_shown = self.back_target = self.back_from = None
        self.var('start').trace_add('write', lambda *_a: self.refresh())
        self.root.after(1000, self.tick_clock)

    def schedule_stage(self):
        """Redraw once the window stops changing size (the background is made per size)."""
        if self.stage_job:
            self.root.after_cancel(self.stage_job)
        self.stage_job = self.root.after(60, self.draw_stage)

    def home_key(self, action):
        """Arrow keys move the focus (Up: the menu row, Down: the tiles), Enter opens or starts
        what has it. On a settings page they move through its list (Up past its first row: the
        menu row); Esc closes a pop-up list or goes back home."""
        if getattr(self, 'capturing', None):
            return None
        if self.current_page != 'play':
            lst = self.lists.get(self.current_page)
            typing = self.root.focus_get() is not None and \
                self.root.focus_get().winfo_class() in ('Entry', 'TEntry', 'Text')
            if action == 'back':
                if not (lst and lst.back()):
                    self.show('play')
                return 'break'
            if typing or (lst and lst.popup):
                return lst.key(action) if lst and not typing else None
            if self.home_row == 0:  # the menu row: Down goes back into the page
                if action == 'enter':
                    self.show(HOME_MENU[self.menu_index][0])
                elif action[0] > 0:
                    self.home_row = 1
                    self.draw_stage()
                elif action[1]:
                    self.menu_index = min(len(HOME_MENU) - 1, max(0, self.menu_index + action[1]))
                    self.draw_stage()
                return 'break'
            result = lst.key(action) if lst else None
            if result == 'top' or (lst is None and action == (-1, 0)):  # Up past the first row
                self.home_row = 0
                self.draw_stage()
                return 'break'
            return result
        if action == 'enter':
            self.home_activate()
        elif action != 'back':
            self.home_move(*action)
        return 'break'

    def home_move(self, rows, columns):
        if rows:
            self.home_row = 0 if rows < 0 else 1
        elif self.home_row == 0:
            self.menu_index = min(len(HOME_MENU) - 1, max(0, self.menu_index + columns))
        else:
            self.focus_tile(min(len(HOME_TILES) - 1, max(0, self.tile_index + columns)))
        self.draw_stage()

    def focus_tile(self, index):
        """A game tile with the focus is the game Start runs (and its music plays)."""
        self.tile_index = index
        key = HOME_TILES[index]
        if key != 'settings' and self.var('start').get() != key:
            self.var('start').set(key)
        if self.music:
            self.music.hover(None if self.process or key == 'settings' else key)

    def home_activate(self):
        if self.home_row == 0:
            self.show(HOME_MENU[self.menu_index][0])
        elif HOME_TILES[self.tile_index] == 'settings':
            self.show('display')
        elif not self.process:
            self.play()

    def hit_at(self, x, y):
        return next((hit for (x0, y0, x1, y1), hit in reversed(self.home_hits)
                     if x0 <= x < x1 and y0 <= y < y1), None)

    def home_hover(self, hit):
        if hit == self.home_hot:
            return
        self.home_hot = hit
        self.stage.configure(cursor='hand2' if hit else '')
        self.draw_stage()

    def home_click(self, hit, double=False):
        if not hit:
            return
        kind = hit[0]
        if kind == 'menu':
            self.home_row, self.menu_index = 0, hit[1]
            self.show(HOME_MENU[hit[1]][0])
        elif kind == 'tile':
            self.home_row = 1
            self.focus_tile(hit[1])
            if HOME_TILES[hit[1]] == 'settings':
                self.show('display')
            elif double and not self.process:
                self.play()
            else:
                self.draw_stage()
        elif kind == 'start':
            self.play()
        elif kind == 'stop':
            self.stop()

    def home_image(self, key, make):
        """A cached PhotoImage of a Pillow picture, made ahead by prepare_pictures when it could
        be; None without Pillow."""
        if key not in self.home_images:
            try:
                from PIL import ImageTk
                picture = self.pictures.get(key)
                self.home_images[key] = ImageTk.PhotoImage(picture if picture is not None else make())
            except Exception:
                self.home_images[key] = None
        return self.home_images[key]

    def tile_sizes(self, w, h):
        """The focused tile's side and the others' for a window size."""
        big = max(self.px(150), min(self.px(250), int(min(w * 0.18, h * 0.27))))
        return big, int(big * 0.64)

    def draw_stage(self):
        """The window, drawn again after each change (pictures are cached): the background and the
        menu row on every page; then the home screen's tiles, or the settings page."""
        self.stage_job = None
        c = self.stage
        w, h = c.winfo_width(), c.winfo_height()
        if self.current_page is None or w < 200 or h < 200:
            return
        try:
            import uncharted_art as art
        except ImportError:
            art = None
        px = self.px
        home = self.current_page == 'play'
        c.delete('all')
        self.home_hits = []
        start = self.var('start').get()
        # The background: the selected game's picture (darker behind a settings page), or the art
        # folder's background video on the home screen.
        own = art is not None and art.has_background(start)
        video = None if own else self.sync_hero_video(w, h, start, hidden=not home)
        if (w, h) != self.pictures_size:
            self.pictures_size = (w, h)
            self.prepare_pictures(w, h)
        back = self.background(start, w, h, dim=not home)
        if back is not None:
            self.show_background(back)
            c.create_image(0, 0, image=self.back_photo, anchor='nw')
        if video and home:
            c.create_image(w // 2, h // 2, image=self.video_photo)  # see-through until a frame
        margin = px(56)

        # The menu row: round icons, the focused or pointed one lit with its name under it, the
        # page on screen marked in gold.
        top = px(46)
        for i, (name, label) in enumerate(HOME_MENU):
            x = margin + px(20) + i * px(56)
            focused = self.home_row == 0 and self.menu_index == i
            hot = self.home_hot == ('menu', i)
            here = name == self.current_page
            if (focused or hot) and art:
                alpha = 64 if focused else 34
                dot = self.home_image(('dot', alpha), lambda a=alpha: art.glow_dot(px(44), a))
                if dot:
                    c.create_image(x, top, image=dot)
            colour = GOLD if here else TEXT if focused or hot else '#cfc9bd'
            if self.font_icon:
                c.create_text(x, top, text=NAV_ICONS[name], font=(self.font_icon[0], 15), fill=colour)
            else:
                c.create_text(x, top, text=_(label)[:1], font=self.font_ui_md, fill=colour)
            if here:
                c.create_rectangle(x - px(9), top + px(19), x + px(9), top + px(19) + max(2, px(2)),
                                   fill=GOLD, outline='')
            if focused or hot:
                c.create_text(x, top + px(36), text=_(label), font=self.font_ui_sm, fill=TEXT)
            self.home_hits.append(((x - px(22), top - px(22), x + px(22), top + px(22)), ('menu', i)))
        self.clock_item = c.create_text(w - margin, top, text=self.clock_text(), anchor='e',
                                        font=(self.ps_family, 12), fill='#e4ded2')
        band = px(58)
        if self.update_box:
            c.create_window(w - margin, top + px(34), window=self.update_box, anchor='ne')

        if not home:
            # The page as a panel under the menu row; the status, Stop and Play along the bottom.
            panel_x, panel_y = margin - px(20), px(92)
            panel_w = min(w - 2 * panel_x, px(1120))
            c.create_window(panel_x, panel_y, window=self.content, anchor='nw', width=panel_w,
                            height=h - band - panel_y)
            c.create_window(0, h - band, window=self.dock, anchor='nw', width=w, height=band)
            return

        # The tile row: the focused tile larger, a gold ring while the row has the focus.
        big, small = self.tile_sizes(w, h)
        tile_top, x = px(122), margin
        for i, key in enumerate(HOME_TILES):
            side = big if i == self.tile_index else small
            ring = i == self.tile_index and self.home_row == 1
            hot = self.home_hot == ('tile', i)
            image = None
            if art and key == 'settings':
                image = self.home_image(('blank', side, ring, hot),
                                        lambda s=side, r=ring, o=hot: art.home_blank_tile(s, focused=r, hover=o))
            elif art:
                dim = not self.available.get(key, True)
                image = self.home_image(('tile', key, side, ring, hot, dim),
                                        lambda k=key, s=side, r=ring, o=hot, d=dim:
                                        art.home_tile(k, s, focused=r, hover=o, dimmed=d))
            if image:
                pad = (image.width() - side) // 2
                c.create_image(x - pad, tile_top - pad, image=image, anchor='nw')
            else:
                c.create_rectangle(x, tile_top, x + side, tile_top + side, fill=CARD,
                                   outline=GOLD if ring else LINE)
            if key == 'settings':
                if self.font_icon:
                    c.create_text(x + side / 2, tile_top + side * 0.44, text='\ue713',
                                  font=(self.font_icon[0], max(12, side // 6)), fill=TEXT)
                c.create_text(x + side / 2, tile_top + side * 0.78, text=_('Settings'),
                              font=self.font_ui_sm if side < big else self.font_ui_md, fill=TEXT)
            elif i == self.tile_index and not self.process:
                self.draw_tile_clip(key, x, tile_top, side, side)
            self.home_hits.append(((x, tile_top, x + side, tile_top + side), ('tile', i)))
            x += side + px(16)

        # The focused tile's block: accent rule, series line, name, Start, the folder status.
        if HOME_TILES[self.tile_index] == 'settings':
            accent, eyebrow, title = GOLD, _('Launcher'), _('Settings')
            meta = _('Display and graphics, controls, the game folder and more. Esc comes back here.')
        else:
            focus = HOME_TILES[self.tile_index]
            title, eyebrow = next((t, sub) for k, t, sub, _e in GAMES if k == focus)
            accent, title, eyebrow, meta = GAME_ACCENTS.get(focus, GOLD), _(title), _(eyebrow), self.ready_text
        y = tile_top + big + px(38)
        c.create_rectangle(margin, y, margin + px(28), y + max(2, px(2)), fill=accent, outline='')
        self.shadow_text(margin, y + px(14), eyebrow.upper(), 'nw', accent, self.font_eyebrow)
        self.shadow_text(margin, y + px(32), title, 'nw', '#f6f1e6', self.font_home_title)
        button_y = y + px(88)
        if HOME_TILES[self.tile_index] == 'settings':
            self.home_pill(margin, button_y, _('Open settings'), ('tile', self.tile_index), True)
        elif self.process:
            self.home_pill(margin, button_y, _('Close game'), ('stop',))
            meta = _('The game is running in its own window.')
        else:
            self.home_pill(margin, button_y, _('Start'), ('start',), True)
        self.shadow_text(margin, button_y + px(58), meta, 'nw', '#b3ab9d', self.font_ui_sm,
                         width=max(px(300), int(w * 0.55)))

        # The collection's heading and the port's credit, bottom right.
        if self.brand_image:
            c.create_image(w - margin, h - band - px(46), image=self.brand_image, anchor='se')
        self.shadow_text(w - margin, h - band - px(22), _('Port by {}  ·  version {}').format(AUTHOR, VERSION),
                         'se', '#8e8a82', self.font_ui_sm)

        # Along the bottom: the window options and the keys.
        c.create_rectangle(0, h - band, w, h, fill=BG, outline='')
        c.create_line(0, h - band, w, h - band, fill='#15161a')
        c.create_window(margin, h - band // 2, window=self.home_options, anchor='w')
        c.create_text(w - margin, h - band // 2, anchor='e', fill=MUTED, font=self.font_ui_sm,
                      text=_('← →  Choose        ↑ ↓  Menu        Enter  Start'))

    def home_pill(self, x, y, text, hit, primary=False):
        """A canvas button (smooth corners); returns its right edge."""
        px = self.px
        width, height = max(px(140), self.measure_play(text) + px(60)), px(44)
        hot = self.home_hot == hit
        if primary:
            fill, fg = rgb(GOLD_HI if hot else GOLD) + (255,), '#1a1408'
        else:
            fill, fg = (255, 255, 255, 52 if hot else 30), TEXT
        try:
            import uncharted_art as art
            image = self.home_image(('pill', width, height, fill),
                                    lambda: art.pill_image(width, height, fill,
                                                           None if primary else (255, 255, 255, 70)))
        except ImportError:
            image = None
        if image:
            self.stage.create_image(x, y, image=image, anchor='nw')
        else:
            self.stage.create_rectangle(x, y, x + width, y + height, fill=GOLD if primary else CARD, outline='')
        self.stage.create_text(x + width / 2, y + height / 2, text=text, font=self.font_play, fill=fg)
        self.home_hits.append(((x, y, x + width, y + height), hit))
        return x + width

    def clock_text(self):
        name = str(self.var('player_name').get()).strip() or _('Player')
        return f"{name}        {time.strftime('%H:%M')}"

    def tick_clock(self):
        if self.clock_item and self.current_page == 'play':
            self.stage.itemconfigure(self.clock_item, text=self.clock_text())
        self.root.after(15000, self.tick_clock)

    def shadow_text(self, x, y, text, anchor, fill, font, width=0):
        """Text that stays readable over a bright background: a soft dark copy one pixel below."""
        offset = max(1, self.px(1))
        self.stage.create_text(x + offset, y + offset, text=text, anchor=anchor, fill='#000000', font=font,
                               width=width)
        self.stage.create_text(x, y, text=text, anchor=anchor, fill=fill, font=font, width=width)

    def draw_tile_clip(self, key, x, y, w, h):
        """The hovered game's video (art folder: u1_video.mp4 …) over its cover picture, like the
        PS3 menu's: in its own wide shape across the middle, the dimmed cover above and below,
        within the cover's rounded corners. The cover shows until the first frame arrives (the
        picture starts see-through)."""
        try:
            from PIL import ImageTk
            import uncharted_media
        except ImportError:
            return
        clip = self.tile_clip
        if not clip or self.tile_clip_key != key:
            path = uncharted_media.clip_for(key)
            if not path:
                return
            self.stop_tile_clip()
            self.tile_clip_key = key
            self.tile_clip = clip = uncharted_media.VideoClip(path, (w, h), self.round_clip,
                                                              CONFIG_DIR / 'media-cache', cover=False,
                                                              fill=self.dimmed_cover(key))
            self.root.after(15, lambda: self.pump_tile_clip(clip))
        if clip.failed:
            return
        clip.resize((w, h))
        if not self.tile_photo or (self.tile_photo.width(), self.tile_photo.height()) != (w, h):
            self.tile_photo = ImageTk.PhotoImage('RGBA', (w, h))
        self.stage.create_image(x, y, image=self.tile_photo, anchor='nw')

    def pump_tile_clip(self, clip):
        """Show the newest frame of the cover's video; runs while that video is the current one."""
        if clip is not self.tile_clip:
            return
        frame = clip.latest()
        photo = self.tile_photo
        if frame is not None and photo and frame.size == (photo.width(), photo.height()):
            photo.paste(frame)
        self.root.after(15, lambda: self.pump_tile_clip(clip))

    @staticmethod
    def dimmed_cover(key):
        """What shows above and below the wide video in a cover (helper thread): the cover
        itself, darkened so the video stands out."""
        cache = {}

        def fill(size):
            if cache.get('size') != size:
                from PIL import Image
                import uncharted_art
                cover = uncharted_art.card_scene(key, *size)
                cache.update(size=size, image=Image.blend(cover, Image.new('RGB', size, (6, 5, 4)), 0.6))
            return cache['image']
        return fill

    def round_clip(self, image):
        """Tile video frames (helper thread): the tile's rounded corners."""
        from uncharted_art import rounded_mask
        if self.clip_masks.get('size') != image.size:
            self.clip_masks = {'size': image.size, 'mask': rounded_mask(image.size, max(4, image.size[0] // 12))}
        image = image.convert('RGBA')
        image.putalpha(self.clip_masks['mask'])
        return image

    def background(self, key, width, height, dim=False):
        """A game's background picture for the window (uncharted_art.home_backdrop; `dim`: darker,
        behind a settings page), or None without Pillow. Usually made ahead (prepare_pictures)."""
        wanted = ('back', key, width, height, dim)
        if wanted not in self.pictures:
            try:
                import uncharted_art
                self.pictures[wanted] = uncharted_art.home_backdrop(key, width, height, rgb(BG),
                                                                    0.72 if dim else 0)
            except Exception:
                self.pictures[wanted] = None
        return self.pictures[wanted]

    def prepare_pictures(self, width, height):
        """For a new window size: every game's backgrounds and tiles, made on a helper thread so
        moving between games never waits for them (making one takes 0.1 to 0.3 s)."""
        self.pictures, self.home_images = {}, {}
        pictures, (big, small) = self.pictures, self.tile_sizes(width, height)
        games = [self.var('start').get()] + [k for k in HOME_TILES[:3] if k != self.var('start').get()]
        dimmed = {key: not self.available.get(key, True) for key in games}

        def make(wanted, build):
            if wanted not in pictures:
                pictures[wanted] = build()

        def work():
            try:
                import uncharted_art as art
                for dim in (False, True):
                    for key in games:
                        make(('back', key, width, height, dim),
                             lambda: art.home_backdrop(key, width, height, rgb(BG), 0.72 if dim else 0))
                for key in games:
                    for side, ring in ((big, True), (big, False), (small, False)):
                        make(('tile', key, side, ring, False, dimmed[key]),
                             lambda: art.home_tile(key, side, focused=ring, dimmed=dimmed[key]))
                for side, ring in ((big, True), (big, False), (small, False)):
                    make(('blank', side, ring, False), lambda: art.home_blank_tile(side, focused=ring))
            except Exception:
                pass  # what is missing is made when it is first shown
        threading.Thread(target=work, daemon=True).start()

    def show_background(self, picture):
        """Puts a background picture (the window's size) on screen, cross-fading from the one shown
        over a quarter of a second; a newer one during a fade carries on from the frame shown."""
        from PIL import ImageTk
        photo = self.back_photo
        if photo is None or (photo.width(), photo.height()) != picture.size:
            self.back_photo = ImageTk.PhotoImage(picture)
            self.back_shown = self.back_target = picture
            return
        if picture is self.back_target:
            return
        self.back_from, self.back_target, self.fade_start = self.back_shown, picture, time.perf_counter()
        if not self.fade_job:
            self.fade_job = self.root.after(1, self.fade_step)

    def fade_step(self):
        from PIL import Image
        t = min(1.0, (time.perf_counter() - self.fade_start) / 0.25)
        t = t * t * (3 - 2 * t)
        frame = self.back_target if t >= 1 else Image.blend(self.back_from, self.back_target, t)
        self.back_photo.paste(frame)
        self.back_shown = frame
        self.fade_job = self.root.after(15, self.fade_step) if t < 1 else None

    def sync_hero_video(self, w, h, start, hidden=False):
        """The selected game's video (else the background video) of the art folder, playing in the
        header; paused while `hidden` (a game background covers the header). Returns the player,
        or None when there is no video or PyAV is missing."""
        try:
            import uncharted_media
            path = uncharted_media.header_video()
        except Exception:
            uncharted_media, path = None, None
        video = self.hero_video
        if video and (video.path != path or video.failed):
            video.stop()
            self.hero_video = video = None
        if path and not video:
            self.hero_video = video = uncharted_media.VideoClip(path, (w, h), self.shade_video,
                                                                  CONFIG_DIR / 'media-cache')
            self.root.after(15, lambda: self.pump_video(video))
        if not video:
            return None
        video.resize((w, h))
        video.pause(hidden or self.current_page not in (None, 'play') or bool(self.process))
        if not self.video_photo or (self.video_photo.width(), self.video_photo.height()) != (w, h):
            from PIL import ImageTk
            self.video_photo = ImageTk.PhotoImage('RGB', (w, h))
        return video

    def pump_video(self, video):
        """Show the newest decoded frame; runs while this player is the header's."""
        if video is not self.hero_video:
            return
        frame = video.latest()
        photo = self.video_photo
        if frame is not None and photo and frame.size == (photo.width(), photo.height()):
            photo.paste(frame)
        self.root.after(15, lambda: self.pump_video(video))

    def shade_video(self, image):
        """Video frames (helper thread): a little darker, faded into the page along the bottom so
        the names stay readable, like the still banner."""
        from PIL import Image
        masks = self.video_masks
        if masks.get('size') != image.size:
            page = tuple(int(BG[i:i + 2], 16) for i in (1, 3, 5))
            fade = Image.linear_gradient('L').resize(image.size)
            masks.update(size=image.size, page=Image.new('RGB', image.size, page),
                         dark=Image.new('RGB', image.size, (6, 5, 4)),
                         fade=fade.point(lambda v: 0 if v < 110 else min(255, (v - 110) * 255 // 140)))
        image = Image.blend(image, masks['dark'], 0.22)
        return Image.composite(masks['page'], image, masks['fade'])

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

    def scaled_art(self, path, width, height):
        """The art cut to the banner, darkened (the parchment is light) and faded into the page."""
        try:
            from PIL import Image, ImageTk
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
                    _('Where saves and the shader cache are kept. Empty: the user folder next to the launcher.'))
        self.action(f, _('Open the game folder'), lambda: self.open_path(self.var('game_dir').get(), 'game_dir'))
        self.action(f, _('Open the saves folder'), lambda: self.open_path(self.var('user_dir').get(), 'user_dir'))
        self.section(f, _('Separate game folders'))
        self.note(f, _('Only when the games are not in one collection folder (each dumped on its own). A game with '
                       'a folder here is taken from it; the others from the collection folder above.'))
        for key, _title, subtitle, _exe in GAMES:
            self.folder(f, f'game_dir_{key}', _(subtitle.split(' · ')[0]), _('The folder of this game'),
                        _('The folder of this game. Empty: the collection folder.'), on_change=self.refresh)
        self.section(f, _('Console'))
        self.choice(f, 'language', LANGUAGES, _('Language'),
                    _('The language the PS4 would be set to; the games use it for text and speech.'))
        name = self.ttk.Entry(f, textvariable=self.var('player_name'), width=30)
        self.row(f, _('Player name'), name, _('Shown where the game uses the PSN name.'))

    def build_display(self):
        f = self.page('display', _('Display and graphics'), _('How the game window is shown, and the picture.'))
        self.check(f, 'fullscreen', _('Fullscreen'))
        self.choice(f, 'present_mode', PRESENT_MODES, _('Presentation'),
                    _('Mailbox shows each frame as soon as it is ready without tearing; FIFO waits for the display.'))
        refresh = display_refresh_rate()
        hint = _('Gameplay keeps its normal speed at every setting; the cutscene videos always play at '
                 '60 FPS. Above your display\'s refresh rate use Mailbox or Immediate.')
        if refresh:
            hint += ' ' + _('Your display runs at {} Hz.').format(refresh)
        self.choice(f, 'frame_rate', FRAME_RATES, _('Frame rate'), hint)
        self.choice(f, 'fps_limit', FPS_LIMITS, _('FPS limit'),
                    _('Caps the frame rate below it, for an even, steady rate when your PC can\'t keep up '
                      '(for example 30 on a 60 Hz display).'))
        self.check(f, 'show_fps', _('Show the FPS counter'))
        self.section(f, _('Resolution'))
        self.choice(f, 'window_size', WINDOW_SIZES, _('Window size'),
                    _('The games render at 1920 × 1080. A larger window, or fullscreen on a 1440p or 4K '
                      'screen, gets the image upscaled to fit.'))
        self.choice(f, 'scaler', SCALERS, _('Upscaling'),
                    _('FSR 1 keeps edges sharp when the image is enlarged; bilinear is softer.'))
        self.choice(f, 'sharpening', SHARPENING, _('Sharpening'),
                    _('AMD RCAS after FSR 1 (only while the image is enlarged).'))
        self.section(f, _('Anti-aliasing'))
        modes = ANTI_ALIASING + ([FSR411] if fsr411_assets() else [])
        self.choice(f, 'anti_aliasing', modes, _('Anti-aliasing'),
                    _('Temporal anti-aliasing at the games\' 1920 × 1080, with their own motion vectors: '
                      'smoother edges and less shimmering than the games\' own. DLSS needs a GeForce RTX; '
                      'FSR 4 runs best on a Radeon RX 7000/9000 or a GeForce RTX 40/50.'))
        self.choice(f, 'aa_sharpness', AA_SHARPNESS, _('Anti-aliasing sharpness'),
                    _('Sharpening after the temporal anti-aliasing.'))
        self.section(f, _('Frame generation'))
        hint = _('Frames made between two of the game\'s, for smoother motion: 60 FPS shows as 120. '
                 'Use Mailbox or Immediate presentation and a display of at least that refresh rate. '
                 'Adds a little input latency; menus and cutscene videos are not generated. Use it '
                 'with the 60 or 120 FPS frame rate: above about 165 FPS it pauses.')
        if refresh:
            hint += ' ' + _('Your display runs at {} Hz.').format(refresh)
        self.choice(f, 'frame_generation', FRAME_GENERATION, _('Frame generation'), hint)
        self.choice(f, 'frame_gen_multiplier', FRAME_GEN_MULTIPLIERS, _('Multiplier'),
                    _('FSR 3 makes one frame (2×). DLSS makes up to three on a GeForce RTX 50 (4×), '
                      'one on an RTX 40.'))

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
                 Slider(self.tk, f, self.var('mouse_sensitivity'), MOUSE_SENS_MIN, MOUSE_SENS_MAX, 0.05,
                        COLORS, self.px, self.font_ui_md, width=300, log=True, entry=True),
                 _('How far the camera turns for a mouse movement; 1.00 is the default. Drag the slider or '
                   'type a number (0.1 to 10) for a faster turn.'))
        self.check(f, 'mouse_invert_y', _('Invert the mouse vertically'))

        self.section(f, _('Keyboard and mouse buttons'))
        self.note(f, _('Click a box, then press a key or a mouse button (the wheel counts too). '
                       '✕ removes a binding. Each action can have two.'))
        rows = [(a, t) for a, t, _k, _p in BUTTON_ACTIONS] + [(a, t) for a, t, _k in STICK_ACTIONS]
        for action, title in rows:
            holder = self.tk.Frame(f, bg=CARD)
            for slot in (0, 1):
                button = self.tk.Button(holder, width=16, relief='flat', bg=CARD_HI, fg=TEXT,
                                        activebackground='#2a2c31', activeforeground=TEXT,
                                        font=self.font_ui, cursor='hand2', bd=0, padx=8, pady=4,
                                        command=lambda a=action, i=slot: self.capture_key(a, i))
                button.pack(side='left', padx=(self.px(10) if slot else 0, 0))
                clear = self.tk.Label(holder, text='\ue711' if self.font_icon else '✕', bg=CARD, fg=MUTED,
                                      font=self.font_icon_sm or self.font_ui, cursor='hand2', padx=8)
                clear.pack(side='left', padx=(2, 0))
                clear.bind('<Button-1>', lambda _e, a=action, i=slot: self.set_key(a, i, ''))
                clear.bind('<Enter>', lambda _e, w=clear: w.configure(fg=TEXT))
                clear.bind('<Leave>', lambda _e, w=clear: w.configure(fg=MUTED))
                self.key_buttons[(action, slot)] = button
            self.row(f, _(title), holder)
        self.show_keys()
        self.action(f, _('Default keys'), self.reset_keys, _('Puts every key and mouse button back as it was.'))

        self.section(f, _('Controller'))
        self.note(f, _('Which controller button gives each PS4 button (Xbox / PlayStation names).'))
        self.pad_vars = {}
        values = [v for v, _t in PAD_BUTTONS]
        for action, title, _k, _p in BUTTON_ACTIONS:
            value = (self.app.get('pad_bindings') or {}).get(action, DEFAULT_PAD[action])
            var = self.tk.StringVar(value=value if value in values else 'none')
            self.pad_vars[action] = var
            self.choice(f, None, PAD_BUTTONS, _(title), variable=var)
        self.check(f, 'pad_invert_y', _('Invert the right stick vertically'))
        self.action(f, _('Default controller buttons'), self.reset_pad,
                    _('Puts every controller button back as it was.'))

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
        name = TK_KEYS.get(sym)
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

    def build_advanced(self):
        f = self.page('advanced', _('Advanced'), _('Maintenance and options for testing.'))
        self.check(f, 'close_on_play', _('Close the launcher when the game starts'))
        extra = self.ttk.Entry(f, textvariable=self.var('extra_env'), width=60)
        self.row(f, _('Environment'), extra, _('Extra variables for the game, NAME=value separated by spaces.'))
        self.section(f, _('Shader cache'))
        self.action(f, _('Clear shader cache'), self.clear_cache,
                    _('Compiled shaders are kept so the game does not stutter again. Clear the cache only '
                      'when the picture is broken after an update.'))
        self.section(f, _('Updates'))
        self.note(f, _('This is version {} of the port by {}.').format(VERSION, AUTHOR))
        self.check(f, 'check_updates', _('Look for a new version when the launcher starts'))
        self.action(f, _('Check for updates'),
                    lambda: threading.Thread(target=self.check_update, args=(True,), daemon=True).start())
        self.action(f, _('Releases page'), lambda: webbrowser.open(RELEASES_PAGE),
                    _('Opens the port\'s releases on GitHub in your browser.'))

    def build_log(self):
        tk = self.tk
        page = tk.Frame(self.content, bg=SURFACE)
        self.pages['log'] = page
        shell = tk.Frame(page, bg=SURFACE)
        shell.pack(fill='both', expand=True, pady=(self.px(4), self.px(14)))
        head = tk.Frame(shell, bg=SURFACE)
        head.pack(fill='x', padx=self.px(20), pady=(self.px(18), self.px(14)))
        tk.Label(head, text=_('Log'), bg=SURFACE, fg=TEXT, font=self.font_title).pack(side='left')
        self.pill(head, _('Copy'), self.copy_log, background=SURFACE, padding=(16, 6)).pack(side='right')
        body = tk.Frame(shell, bg=SURFACE)
        body.pack(fill='both', expand=True, padx=self.px(20), pady=(0, self.px(8)))
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
        if not info:
            ready = _('Choose the game folder (the one with eboot.bin) on the Game page.')
        else:
            missing = [title for key, title, _s, _e in GAMES if not self.available[key]]
            ready = f"{info['title']} · {info['id']} · {_('version')} {info['version']}"
            if missing:
                ready += '\n' + _('Not in this folder: ') + ', '.join(missing)
        self.ready_text = ready
        if not self.process:
            title = next(t for k, t, _s, _e in GAMES if k == start)
            self.status.configure(text=_('Ready: {}').format(title) if info else _('No game folder yet.'),
                                  fg=MUTED)
        self.draw_stage()

    # ---- actions -----------------------------------------------------------------------------
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
            self.process = subprocess.Popen(run_command(), cwd=PORT_DIR, env=game_environment(self.app),
                                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        except OSError as error:
            self.append(_('Could not start: {}').format(error) + '\n')
            self.process = None
            return
        self.job = GameJob(self.process)
        self.quiet_media()
        self.entered, self.last_problem, self.stopping = False, '', False
        threading.Thread(target=self.read_output, args=(self.process,), daemon=True).start()
        self.play_button.configure(state='disabled')
        self.stop_button.configure(state='normal')
        self.status.configure(text=_('Preparing the game; it opens in its own window…'), fg=GOLD)
        self.draw_stage()
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
                    # It failed to start or crashed (not Stop, not closing the window): say why.
                    if not self.stopping and (code != 0 or not self.entered):
                        reason = self.last_problem or _('see the Log page')
                        self.status.configure(text=_('The game stopped: {}').format(reason[:110]),
                                              fg='#e07a4f')
                        self.show('log')
                else:
                    stripped = item.strip()
                    if stripped.startswith(('STOP:', 'ERROR', 'GPU crash: the', 'Fault:')) or \
                            ('failed' in stripped and not stripped.startswith('GPU [')) or \
                            stripped.startswith(('No eboot.bin', 'No big', 'Set BB_GAME_DIR', 'bb-probe.exe not found')):
                        if not self.last_problem or stripped.startswith(('STOP:', 'GPU crash')):
                            self.last_problem = stripped
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
        if self.job:
            self.stopping = True
            self.job.terminate()

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
        box = tk.Frame(self.stage, bg=CARD, highlightthickness=1, highlightbackground=GOLD_DIM)
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
        self.update_box = box  # shown under the clock (draw_stage)
        self.draw_stage()
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
        shutil.rmtree(Path(self.app['user_dir'] or DATA_DIR / 'user') / 'cache', ignore_errors=True)
        self.messagebox.showinfo(NAME, _('Shader cache cleared. The next start stutters for a while as it is rebuilt.'))

    def shortcut(self):
        """Uncharted.lnk on the desktop."""
        if FROZEN:
            target, arguments, icon = sys.executable, '', f'{sys.executable},0'
        else:
            pythonw = Path(sys.executable).with_name('pythonw.exe')
            target = str(pythonw if pythonw.exists() else sys.executable)
            arguments, icon = Path(__file__).resolve(), PORT_DIR / 'launcher' / 'uncharted.ico'
        script = ('$s=(New-Object -ComObject WScript.Shell).CreateShortcut([Environment]::GetFolderPath("Desktop")'
                  '+"\\Uncharted.lnk");'
                  f'$s.TargetPath="{target}";$s.WorkingDirectory="{PORT_DIR}";$s.IconLocation="{icon}";'
                  + (f"$s.Arguments='\"{arguments}\"';" if arguments else '') + '$s.Save()')
        result = subprocess.run(['powershell', '-NoProfile', '-Command', script], capture_output=True,
                                creationflags=NO_WINDOW)
        if result.returncode == 0:
            self.messagebox.showinfo(NAME, _('Shortcut created on the desktop.'))
        else:
            self.messagebox.showerror(NAME, result.stderr.decode(errors='replace')[:400])

    def add_to_steam(self):
        """The collection in the Steam library (a non-Steam game that starts Uncharted.exe --play)."""
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
                    uncharted_art.steam_artwork(game_folder(self.app, 'u1') or game_folder(self.app, 'u2'), config / 'grid',
                                                bbport_steam.shortcut_id(f'"{exe}"', title))
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
        if self.music:
            self.music.stop()
        if self.hero_video:
            self.hero_video.stop()
        self.stop_tile_clip()
        self.root.destroy()


class GameJob:
    """A Windows job holding run.py and everything it starts (the game, the next game of the
    collection, an in-game restart), so Stop ends all of them."""

    def __init__(self, process):
        kernel32 = ctypes.windll.kernel32
        kernel32.CreateJobObjectW.restype = ctypes.c_void_p
        self.handle = kernel32.CreateJobObjectW(None, None)
        if self.handle:
            kernel32.AssignProcessToJobObject(ctypes.c_void_p(self.handle), ctypes.c_void_p(int(process._handle)))

    def terminate(self):
        if self.handle:
            ctypes.windll.kernel32.TerminateJobObject(ctypes.c_void_p(self.handle), 1)

    def close(self):
        if self.handle:
            ctypes.windll.kernel32.CloseHandle(ctypes.c_void_p(self.handle))
            self.handle = None


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
        process = subprocess.Popen(run_command(), cwd=PORT_DIR, env=game_environment(settings),
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


def main():
    args = sys.argv[1:]
    if args and args[0] in ('--run', '--script'):
        sys.exit(run_role(args))
    if args[:1] == ['--install-update'] and len(args) == 3:
        sys.exit(install_update(args[1], args[2]))
    if FROZEN and UPDATE_DIR not in PORT_DIR.parents:
        shutil.rmtree(UPDATE_DIR, ignore_errors=True)  # what a finished update left behind
    settings = {**APP_DEFAULTS, **load_json(CONFIG_FILE, {})}
    if '--play' in args and game_available(settings, settings['start']):
        sys.exit(play_without_window(settings))
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (AttributeError, OSError):
        pass
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    root = tk.Tk()
    Launcher(root, tk, ttk, filedialog, messagebox)
    root.mainloop()


if __name__ == '__main__':
    main()
