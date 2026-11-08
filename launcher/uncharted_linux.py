"""Linux counterparts for the shared Uncharted launcher."""

import ctypes
import ctypes.util
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys


def config_dir():
    return Path(os.environ.get('XDG_CONFIG_HOME') or Path.home() / '.config') / 'uncharted-launcher'


def host_environment():
    """Return an environment suitable for host utilities from a frozen AppImage."""
    env = dict(os.environ)
    if getattr(sys, 'frozen', False):
        env.pop('LD_LIBRARY_PATH', None)
        original = env.pop('LD_LIBRARY_PATH_ORIG', None)
        if original:
            env['LD_LIBRARY_PATH'] = original
    return env


def hyprland_environment():
    """Return the Hyprland environment even when the desktop did not export it."""
    env = host_environment()
    if env.get('HYPRLAND_INSTANCE_SIGNATURE'):
        return env
    runtime_dir = Path(env.get('XDG_RUNTIME_DIR') or f'/run/user/{os.getuid()}') / 'hypr'
    try:
        instances = [item for item in runtime_dir.iterdir() if (item / '.socket.sock').exists()]
        instance = max(instances, key=lambda item: item.stat().st_mtime)
    except (OSError, ValueError):
        return env
    env['HYPRLAND_INSTANCE_SIGNATURE'] = instance.name
    return env


def hyprland_scale():
    """Read the focused monitor scale from Hyprland, when it is available."""
    env = hyprland_environment()
    if not env.get('HYPRLAND_INSTANCE_SIGNATURE'):
        return None
    try:
        result = subprocess.run(['hyprctl', '-j', 'monitors'], env=env,
                                capture_output=True, text=True, check=True, timeout=1)
        monitor = next((item for item in json.loads(result.stdout) if item.get('focused')), None)
        scale = float(monitor['scale']) if monitor else 1.0
    except (OSError, ValueError, KeyError, StopIteration, json.JSONDecodeError, subprocess.SubprocessError):
        return None
    return scale if 1.1 <= scale <= 4.0 else None


def font_scale(root):
    """Return a compositor or Xft scale that XWayland does not report to Tk."""
    scale = hyprland_scale()
    if scale:
        return scale
    try:
        x11 = ctypes.CDLL(ctypes.util.find_library('X11') or 'libX11.so.6')
        x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x11.XResourceManagerString.restype = ctypes.c_char_p
        x11.XResourceManagerString.argtypes = [ctypes.c_void_p]
        x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        display = x11.XOpenDisplay(None)
        if not display:
            return 1.0
        try:
            resources = (x11.XResourceManagerString(display) or b'').decode(errors='replace')
        finally:
            x11.XCloseDisplay(display)
        dpi = next((float(line.split(':', 1)[1]) for line in resources.splitlines()
                    if ':' in line and line.split(':', 1)[0].strip() == 'Xft.dpi'), None)
        scale = dpi / root.winfo_fpixels('1i') if dpi else 1.0
    except (OSError, ValueError, AttributeError):
        return 1.0
    return scale if 1.1 <= scale <= 4.0 else 1.0


def set_icon(root, tk):
    for path in (Path(getattr(sys, '_MEIPASS', '')) / 'uncharted.png',
                 Path(__file__).resolve().parent / 'uncharted.png'):
        if path.is_file():
            try:
                root._uncharted_icon = tk.PhotoImage(file=str(path))
                root.iconphoto(True, root._uncharted_icon)
            except tk.TclError:
                pass
            return


def open_path(target):
    subprocess.Popen(['xdg-open', str(target)], env=host_environment(), stdin=subprocess.DEVNULL,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)


class GameJob:
    """Keep the game and its preparation processes in one stoppable process group."""

    def __init__(self, process):
        self.pgid = process.pid

    def terminate(self):
        if self.pgid is not None:
            os.killpg(self.pgid, signal.SIGTERM)
            return True
        return False

    def close(self):
        self.pgid = None


def steam_folder():
    home = Path.home()
    for candidate in (home / '.steam' / 'steam', home / '.local' / 'share' / 'Steam',
                      home / '.var' / 'app' / 'com.valvesoftware.Steam' / '.local' / 'share' / 'Steam'):
        if (candidate / 'userdata').is_dir():
            return candidate.resolve()
    return None


def steam_running():
    return subprocess.run(['pgrep', '-x', 'steam'], stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0


def steam_shutdown(steam):
    if 'com.valvesoftware.Steam' in str(steam):
        command = ['flatpak', 'run', 'com.valvesoftware.Steam', '-shutdown']
    else:
        command = [shutil.which('steam') or 'steam', '-shutdown']
    subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, start_new_session=True)


def launch_target(launcher_file):
    if os.environ.get('APPIMAGE'):
        return Path(os.environ['APPIMAGE']), '--play'
    if getattr(sys, 'frozen', False):
        return Path(sys.executable), '--play'
    return Path(sys.executable), f'"{Path(launcher_file).resolve()}" --play'


def desktop_entry(launcher_file, port_dir):
    desktop = Path.home() / 'Desktop'
    desktop.mkdir(parents=True, exist_ok=True)
    command = os.environ.get('APPIMAGE') or sys.executable
    args = '' if os.environ.get('APPIMAGE') or getattr(sys, 'frozen', False) else f' "{launcher_file}"'
    icon = Path(port_dir) / 'launcher' / 'uncharted.png'
    target = desktop / 'Uncharted.desktop'
    target.write_text('[Desktop Entry]\nType=Application\nName=Uncharted\n'
                      f'Exec="{command}"{args}\nIcon={icon}\nTerminal=false\nCategories=Game;\n',
                      encoding='utf-8')
    target.chmod(0o755)
