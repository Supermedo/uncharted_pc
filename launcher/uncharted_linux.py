"""Linux counterparts for the shared Uncharted launcher."""

import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys


def config_dir():
    return Path(os.environ.get('XDG_CONFIG_HOME') or Path.home() / '.config') / 'uncharted-launcher'


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
    env = dict(os.environ)
    if getattr(sys, 'frozen', False):
        env.pop('LD_LIBRARY_PATH', None)
        original = env.pop('LD_LIBRARY_PATH_ORIG', None)
        if original:
            env['LD_LIBRARY_PATH'] = original
    subprocess.Popen(['xdg-open', str(target)], env=env, stdin=subprocess.DEVNULL,
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
