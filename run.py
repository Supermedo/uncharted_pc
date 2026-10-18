#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepares an Uncharted executable of the Nathan Drake Collection and starts bb-probe.exe.

BB_GAME_DIR   the game folder (CUSA02343, CUSA02320, ...; the one with eboot.bin)
BB_GAME_DIR_U1 / _U2 / _U3   optional: the folder of a game that was dumped on its own; used for
              that game instead of BB_GAME_DIR (its executable may be that folder's eboot.bin)
BB_EXE        u1 / u2 / u3 or an executable name (default u1: eboot.bin, the collection menu)
BB_USER_DIR   saves and shader caches (default user/ beside this script)
Extra arguments go to bb-probe. When the game starts another executable of the collection
(sceSystemServiceLoadExec), that one is prepared and started in its place.
The in-game "Apply and restart" runs this script again through BB_RESTART_COMMAND;
`--after PID` waits for the previous game process to end first (its GPU device and memory).
"""
import ctypes
import os
from pathlib import Path
import subprocess
import sys

PORT = Path(__file__).resolve().parent
EXECUTABLES = {'u1': 'eboot.bin', 'u2': 'big2-ps4_Shipping.elf', 'u3': 'big3-ps4_Shipping.elf'}
LOADEXEC_EXIT = 101  # runtime_services.c: the game asked for another executable


def fail(message):
    print(message, file=sys.stderr)
    sys.exit(1)


def wait_for(pid):
    kernel32 = ctypes.windll.kernel32
    SYNCHRONIZE = 0x00100000
    handle = kernel32.OpenProcess(SYNCHRONIZE, False, pid)
    if handle:
        kernel32.WaitForSingleObject(handle, 30000)
        kernel32.CloseHandle(handle)


def no_console():
    """Started without a console (the launcher): console programs must not open their own."""
    return 0 if ctypes.windll.kernel32.GetConsoleWindow() else subprocess.CREATE_NO_WINDOW


def run_script(name, *args, tolerant=False):
    """Run one of the preparation scripts. `tolerant` reports instead of exiting.

    The preparation scripts must succeed; the patch compiler may legitimately
    decline (a game version the patches do not fit), so callers that can carry on
    without it pass tolerant=True.
    """
    result = subprocess.run(script_command(name, *args), stdin=subprocess.DEVNULL,
                            creationflags=no_console())
    if result.returncode and not tolerant:
        sys.exit(result.returncode)
    return result


def script_command(name, *args):
    # Inside the packaged executable (PyInstaller) the executable runs scripts itself.
    script = ['--script'] if getattr(sys, 'frozen', False) else []
    return [sys.executable, *script, str(PORT / 'scripts' / name), *map(str, args)]


def run_script_capture(name, *args):
    """Run a preparation script and return its stdout, '' when it fails."""
    try:
        result = subprocess.run(script_command(name, *args), stdin=subprocess.DEVNULL,
                                capture_output=True, text=True, encoding='utf-8',
                                errors='replace', creationflags=no_console())
    except OSError:
        return ''
    return result.stdout if result.returncode == 0 else ''


def find_executable(name):
    """bin/ (packaged), else out/ (built with build.sh)."""
    for candidate in (PORT / 'bin' / name, PORT / 'out' / name):
        if candidate.is_file():
            return candidate
    return None


def executable_name(value):
    return EXECUTABLES.get(value.lower(), value) if value else EXECUTABLES['u1']


def locate(env, name):
    """(folder, executable file) for one of the collection's executables. A game dumped on its own
    (BB_GAME_DIR_U1/U2/U3) comes from its folder, where the game may be that folder's eboot.bin."""
    key = next((k for k, v in EXECUTABLES.items() if v == name), '')
    own = env.get(f'BB_GAME_DIR_{key.upper()}', '') if key else ''
    folder = own or env.get('BB_GAME_DIR', '')
    if not folder:
        return None, name
    folder = Path(folder)
    if own and not (folder / name).is_file() and (folder / 'eboot.bin').is_file():
        name = 'eboot.bin'
    return folder, name


def patch_arguments(env, exe, game, out):
    """patches.py arguments for the game being run, or [] without BB_PATCHES_DIR. AppElf is the
    collection's name for the game (eboot.bin, big2-ps4_Shipping.elf, big3-ps4_Shipping.elf), also
    when a game dumped on its own runs from its folder's eboot.bin: a patch for Uncharted 1's
    eboot.bin must not reach Uncharted 2 or 3."""
    patches_dir = env.get('BB_PATCHES_DIR', '')
    if not patches_dir or not Path(patches_dir).is_dir():
        return []
    return ['--external-only', '--app-elf', exe, '--game-dir', str(game), '--out', str(out),
            '--patches-dir', patches_dir,
            '--patches-config', env.get('BB_PATCHES_CONFIG', str(Path(patches_dir) / 'patches.json'))]


def prepare(game, exe, out):
    """boot-linked.bin for one executable, kept until the executable or the scripts change."""
    out.mkdir(parents=True, exist_ok=True)
    linked = out / 'boot-linked.bin'
    # Which executable the files in out/ were made from: another folder (a per-game one) needs a new run.
    stamp, source = out / 'source.txt', str(game / exe)
    sources = [game / exe, *(PORT / 'scripts').glob('*.py'), *(game / 'sce_module').glob('*.prx')]
    if linked.is_file() and all(linked.stat().st_mtime >= s.stat().st_mtime for s in sources):
        if not stamp.is_file():
            stamp.write_text(source, encoding='utf-8')
        if stamp.read_text(encoding='utf-8', errors='replace') == source:
            return
    print(f'Preparing {exe} ...')
    run_script('prepare.py', game, '--out', out, '--exe', exe)
    run_script('link_modules.py', game, '--out', out, '--exe', exe)
    stamp.write_text(source, encoding='utf-8')


def read_setting(settings, key):
    """One key from bbport.ini, or '' when absent."""
    try:
        with open(settings, encoding='utf-8-sig', errors='replace') as stream:
            for line in stream:
                name, _, value = line.partition('=')
                if name.strip() == key:
                    return value.strip()
    except OSError:
        pass
    return ''


def render_sizes(config):
    """(scene, output) from bbport.ini, or (None, None) for plain 1080p."""
    result = run_script_capture('patches.py', '--print-scaled', '--settings', config)
    parts = result.split()
    if len(parts) == 2 and 'x' in parts[0] and 'x' in parts[1]:
        return parts[0], parts[1]
    return None, None


def apply_patches(game, out, env, caps):
    """Compile patches.bin for this game version and point the loader at it.

    The patches are byte writes at the addresses of one game version; on another
    version they would corrupt code, so patches.py writes an empty patch set and
    says so rather than failing (the game then runs at its own frame rate). That
    message is the user-visible explanation, so it is printed here, not swallowed.
    """
    fps = env.get('BB_FPS') or 'uncap'
    scaled_render, scaled_output = (None, None)
    if not env.get('BB_RENDER_RES'):
        scaled_render, scaled_output = render_sizes(env['BB_CONFIG'])
    # Live resolution changes keep the guest at 1920x1080 and scale host targets at run
    # time; otherwise the render size is a startup patch. Same rules as run.sh: the
    # setting wins, then bbport.ini, and anything but "1" means the startup patch.
    live = env.get('BB_LIVE_RES') or read_setting(env['BB_CONFIG'], 'live_resolution') or '0'
    if scaled_output and live == 'auto':
        live = '1' if gpu_supports_live(caps) else '0'
    if live != '1':
        live = '0'
    if scaled_output and live == '1':
        print(f'Output {scaled_output}: live resolution changes (no startup patch)')
    elif scaled_output:
        env['BB_RENDER_RES'], env['BB_OUTPUT_RES'] = scaled_render, scaled_output
        env['BB_AUTO_RENDER_RES'] = '1'
        env.setdefault('BB_DMEM_MB', '9152')
        print(f'Output {scaled_output}: scene {scaled_render} (startup patch)')

    run_script('patches.py', '--out', out, '--fps', fps,
               '--extra', env.get('BB_PATCHES', ''), '--settings', env['BB_CONFIG'],
               '--game-dir', game, '--render-res', env.get('BB_RENDER_RES', ''),
               '--output-res', env.get('BB_OUTPUT_RES', ''),
               '--patches-dir', env.get('BB_PATCHES_DIR', ''),
               '--patches-config', env.get('BB_PATCHES_CONFIG', ''),
               tolerant=True)
    # The launcher's frame rate wins; otherwise the present rate follows the FPS patch,
    # as in run.sh. setdefault, not assignment: an explicit BB_VBLANK_HZ is kept.
    env.setdefault('BB_VBLANK_HZ', {'uncap': '0', '90': '90'}.get(fps, '60'))
    # Only offer the loader a patch file that exists.
    if (out / 'patches.bin').is_file():
        env['BB_PATCHES_FILE'] = str(out / 'patches.bin')
    else:
        env.pop('BB_PATCHES_FILE', None)


def gpu_supports_live(caps):
    """Whether bb-gpu-capabilities says this GPU can do live resolution changes."""
    if not caps or not Path(caps).is_file():
        return False
    try:
        done = subprocess.run([str(caps), '--live-resolution'], capture_output=True,
                              text=True, timeout=30)
        return done.returncode == 0 and done.stdout.strip().endswith('1')
    except (OSError, subprocess.SubprocessError):
        return False


def main():
    args = sys.argv[1:]
    if len(args) >= 2 and args[0] == '--after':
        wait_for(int(args[1]))
        args = args[2:]
    os.chdir(PORT)
    # Paths and titles can hold characters the console code page (GBK, ...) lacks: never fail on a print.
    # Pipes (the launcher reads them as UTF-8) get UTF-8; a real console keeps its own encoding.
    for stream in (sys.stdout, sys.stderr):
        if stream:
            stream.reconfigure(errors='replace', **({} if stream.isatty() else {'encoding': 'utf-8'}))
    env = os.environ
    # Game titles carry "™": the scripts must not fall back to the Windows code page.
    env['PYTHONUTF8'] = '1'
    env['PYTHONIOENCODING'] = 'utf-8'
    data = Path(env.get('BB_DATA_DIR', PORT))
    out_root = data / 'out'
    env.setdefault('BB_CONFIG', str(data / 'bbport.ini'))
    # Keyboard, mouse and controller bindings (the launcher's Controls page).
    env.setdefault('BB_INPUT_CONFIG', str(data / 'input.ini'))
    # The collection's renderer (all three games): the camera velocity pass of the motion blur
    # (its constants give the motion vectors), the compute pass after the scene is lit, before
    # the post-processing, where DLSS/FSR/TAA and frame generation take it, and the pass that
    # writes the finished picture before the UI (docs/upscaler.md).
    env.setdefault('BB_VELOCITY_PS', 'f4b18264')
    env.setdefault('BB_UPSCALE_BEFORE_CS', 'aec539f0')
    env.setdefault('BB_HUDLESS_PS', '484cdbf5')
    if not env.get('BB_FSR411_DIR') and not (PORT / 'fsr4_411').is_dir() and (data / 'fsr4_411').is_dir():
        env['BB_FSR411_DIR'] = str(data / 'fsr4_411')

    probe = Path(env['BB_PROBE']) if env.get('BB_PROBE') else find_executable('bb-probe.exe')
    if not probe or not probe.is_file():
        fail('bb-probe.exe not found: build it with build.sh (MSYS2 CLANG64) or use a packaged build.')
    # Development builds run from the MSYS2 tree: its CLANG64 DLLs (SDL3, FFmpeg, ...).
    if not (probe.parent / 'SDL3.dll').is_file():
        clang64 = Path(env.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
        if clang64.is_dir():
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'

    exe = executable_name(env.get('BB_EXE', ''))
    user_dir = Path(env.get('BB_USER_DIR', data / 'user')).resolve()
    user_dir.mkdir(parents=True, exist_ok=True)
    # GPU caches (shaders, pipelines) beside the saves; read before main(), so set here.
    env.setdefault('BB_GPU_USER_DIR', str(user_dir))
    # BB_VBLANK_HZ is left to apply_patches(), which derives it from the FPS patch the
    # same way run.sh does (uncap -> uncapped, 90 -> 90, otherwise 60).
    loadexec = out_root / 'loadexec.txt'
    env['BB_LOADEXEC_FILE'] = str(loadexec)
    guest_argv = None
    while True:
        game, file = locate(env, exe)
        if not game:
            fail('Set BB_GAME_DIR to the game folder (the one with eboot.bin).')
        if not (game / file).is_file():
            fail(f'No {file} in {game}.')
        out = out_root / Path(exe).stem.split('-')[0]
        prepare(game, file, out)
        run_script('content_profile.py', game, '--out', out, '--sku', env.get('BB_CONTENT_SKU', 'full'))
        # After prepare(): patches.py reads out/eboot.elf, and the resolution presets
        # need BB_RENDER_RES settled before the loader starts.
        apply_patches(game, out, env, probe.parent / 'bb-gpu-capabilities.exe')
        this = ['--run'] if getattr(sys, 'frozen', False) else [str(Path(__file__).resolve())]
        env['BB_EXE'] = exe
        env['BB_RESTART_COMMAND'] = subprocess.list2cmdline([sys.executable, *this, '--after', str(os.getpid()), *args])
        command = [str(probe), str(out / 'boot-linked.bin'), '--content-profile', str(out / 'content.bin'),
                   '--app0', str(game), '--user', str(user_dir), '--timeout', env.get('BB_TIMEOUT', '0')]
        # Third-party patches (shadPS4/GoldHEN XML) for this executable, only when a folder is set:
        # BB_PATCHES_DIR, selection in BB_PATCHES_CONFIG (patches.json) or each patch's isEnabled.
        patches = patch_arguments(env, exe, game, out)
        if patches:
            run_script('patches.py', *patches)
            env['BB_PATCHES_FILE'] = str(out / 'patches.bin')
        # The code patches compiled for this game version (FPS, effects, startup
        # resolution), with the third-party ones after them. Only passed when patches.py
        # actually produced a file.
        if env.get('BB_PATCHES_FILE'):
            command += ['--patches', env['BB_PATCHES_FILE']]
        if guest_argv:
            command += ['--guest-argv', str(guest_argv)]
        loadexec.unlink(missing_ok=True)
        # No stdin: an inherited pipe (shells such as Git Bash) cost the game its console output.
        code = subprocess.run([*command, *args], stdin=subprocess.DEVNULL, creationflags=no_console()).returncode
        if code != LOADEXEC_EXIT or not loadexec.is_file():
            return code
        lines = loadexec.read_text(encoding='utf-8', errors='replace').splitlines()
        target = Path(lines[0].replace('\\', '/')).name if lines else ''
        if not target:
            fail('The game asked for another executable without a name.')
        print(f'Switching to {target}')
        exe = target
        guest_argv = out_root / 'guest-argv.txt'
        guest_argv.write_text('\n'.join(lines) + '\n', encoding='utf-8')


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
