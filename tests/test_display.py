"""BB_GAME_RES: the guest patch (tests/test_display.c), run.py's memory budget and the launcher's size choice."""
from paths import ROOT
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class GamePatchTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc') or shutil.which('clang') or shutil.which('gcc'), 'needs a C compiler')
    def test_display_patch_on_a_synthetic_image(self):
        cc = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'display-test'
            build = subprocess.run([cc, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'src'),
                                    str(ROOT / 'tests/test_display.c'), str(ROOT / 'src/runtime_display.c'), '-o', str(exe)],
                                   capture_output=True, text=True)
            if build.returncode:
                self.skipTest(build.stderr[-300:])   # e.g. runtime.h needs the Windows compat layer in this toolchain
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class RunBudgetTests(unittest.TestCase):
    def budget(self, resolution, **extra):
        run = load('run_module', ROOT / 'run.py')
        env = {'BB_GAME_RES': resolution, **extra}
        run.game_res(env)
        return env

    def test_default_size_changes_nothing(self):
        self.assertNotIn('BB_GAME_RES', self.budget('1920x1080'))
        self.assertNotIn('BB_GAME_RES', self.budget('garbage'))

    def test_1440p_grows_the_heaps_and_the_direct_memory(self):
        env = self.budget('2560x1440')
        self.assertGreater(int(env['BB_GAME_RT_MB']), 0)
        self.assertEqual(int(env['BB_GAME_RT_MB']) % 64, 0)
        self.assertEqual(int(env['BB_DMEM_MB']), 5056 + int(env['BB_GAME_RT_MB']) + int(env['BB_GAME_RT2_MB']) -
                         int(env['BB_GAME_FREE_MB']))

    def test_explicit_amounts_win(self):
        env = self.budget('3840x2160', BB_GAME_RT_MB='100', BB_GAME_RT2_MB='0', BB_GAME_FREE_MB='0')
        self.assertEqual((env['BB_GAME_RT_MB'], env['BB_GAME_RT2_MB'], env['BB_GAME_FREE_MB']), ('100', '0', '0'))
        self.assertEqual(env['BB_DMEM_MB'], '5156')

    def test_540p_sizes_pass_through_with_no_extra_memory(self):
        # 960x540 (16:9), 1280x540 (21:9), 1920x540 (32:9): inside run.py's 640x360 minimum, no heap growth.
        for resolution in ('960x540', '1280x540', '1920x540'):
            with self.subTest(resolution=resolution):
                env = self.budget(resolution)
                self.assertEqual(env['BB_GAME_RES'], resolution)
                self.assertEqual((env['BB_GAME_RT_MB'], env['BB_GAME_RT2_MB'], env['BB_GAME_FREE_MB']), ('0', '0', '0'))
                self.assertEqual(env['BB_GAME_MOVIE_MB'], '4')
                self.assertNotIn('BB_DMEM_MB', env)


class LauncherSizeTests(unittest.TestCase):
    def setUp(self):
        sys.path.insert(0, str(ROOT / 'launcher'))
        self.launcher = load('uncharted_launcher_module', ROOT / 'launcher/uncharted_launcher.py')

    def size(self, screen=(3440, 1440), **settings):
        return self.launcher.game_resolution({**self.launcher.APP_DEFAULTS, **settings}, screen)

    def test_own_size_is_empty(self):
        self.assertEqual(self.size(), '')

    def test_heights_and_shapes(self):
        self.assertEqual(self.size(render_height='1440'), '2560x1440')
        self.assertEqual(self.size(render_height='1440', aspect='21:9'), '3440x1440')
        self.assertEqual(self.size(render_height='1080', aspect='21:9'), '2560x1080')
        self.assertEqual(self.size(render_height='1440', aspect='window', fullscreen=True), '3440x1440')
        self.assertEqual(self.size(render_height='1080', aspect='window', window_size='2560x1080'), '2560x1080')

    def test_too_large_sizes_step_down_with_the_shape(self):
        width, height = (int(v) for v in self.size(render_height='2160', aspect='32:9').split('x'))
        self.assertLessEqual(width * height, self.launcher.MAX_RENDER_PIXELS)
        self.assertGreater(width / height, 3.0)

    def test_540_is_a_render_height_after_the_games_own_1080(self):
        heights = [value for value, _label in self.launcher.RENDER_HEIGHTS]
        self.assertIn('540', heights)
        self.assertEqual(heights[0], '1080')   # the default and the choice list's fallback stay first
        label = dict(self.launcher.RENDER_HEIGHTS)['540']
        self.assertEqual(label, '540p (handhelds, low-memory GPUs)')

    def test_540_widths_for_each_shape(self):
        # Widths from the shape's ratio at 540 with the even rounding: 540 x 16/9 = 960, 540 x 64/27 = 1280,
        # 540 x 32/9 = 1920, the window's 1920x1080 (16:9) = 960, fullscreen 3440x1440 (43:18) = 1290, 2560x1080 (64:27) = 1280.
        self.assertEqual(self.size(render_height='540'), '960x540')
        self.assertEqual(self.size(render_height='540', aspect='21:9'), '1280x540')
        self.assertEqual(self.size(render_height='540', aspect='32:9'), '1920x540')
        self.assertEqual(self.size(render_height='540', aspect='window', window_size='1920x1080'), '960x540')
        self.assertEqual(self.size(render_height='540', aspect='window', fullscreen=True), '1290x540')
        self.assertEqual(self.size((2560, 1080), render_height='540', aspect='window', fullscreen=True), '1280x540')

    def test_existing_heights_give_the_same_sizes(self):
        # Snapshot of game_resolution() before the 540 choice: the non-window shapes do not depend on the screen.
        non_window = {
            '720': ('1280x720', '1706x720', '2560x720'),
            '1080': ('', '2560x1080', '3840x1080'),
            '1440': ('2560x1440', '3440x1440', '4480x1260'),
            '1620': ('2880x1620', '3440x1440', '4480x1260'),
            '2160': ('3200x1800', '3440x1440', '4480x1260'),
        }
        for height, (w16, w21, w32) in non_window.items():
            for screen in ((3440, 1440), (1920, 1080), None):
                with self.subTest(height=height, screen=screen):
                    self.assertEqual(self.size(screen, render_height=height, aspect='16:9'), w16)
                    self.assertEqual(self.size(screen, render_height=height, aspect='21:9'), w21)
                    self.assertEqual(self.size(screen, render_height=height, aspect='32:9'), w32)
        window = [
            ('720', dict(fullscreen=True), (3440, 1440), '1720x720'),
            ('720', dict(fullscreen=True), (2560, 1080), '1706x720'),
            ('720', dict(window_size='1600x900'), (1920, 1080), '1280x720'),
            ('720', dict(window_size='2560x1080'), (1920, 1080), '1706x720'),
            ('1080', dict(fullscreen=True), (3440, 1440), '2580x1080'),
            ('1080', dict(fullscreen=True), (2560, 1080), '2560x1080'),
            ('1080', dict(window_size='1600x900'), (1920, 1080), ''),
            ('1080', dict(window_size='2560x1080'), (1920, 1080), '2560x1080'),
            ('1440', dict(fullscreen=True), (3440, 1440), '3440x1440'),
            ('1440', dict(fullscreen=True), (2560, 1080), '3414x1440'),
            ('1440', dict(window_size='1600x900'), (1920, 1080), '2560x1440'),
            ('1440', dict(window_size='2560x1080'), (1920, 1080), '3414x1440'),
            ('1620', dict(fullscreen=True), (3440, 1440), '3440x1440'),
            ('1620', dict(fullscreen=True), (2560, 1080), '3414x1440'),
            ('1620', dict(window_size='1600x900'), (1920, 1080), '2880x1620'),
            ('1620', dict(window_size='2560x1080'), (1920, 1080), '3414x1440'),
            ('2160', dict(fullscreen=True), (3440, 1440), '3440x1440'),
            ('2160', dict(fullscreen=True), (2560, 1080), '3414x1440'),
            ('2160', dict(window_size='1600x900'), (1920, 1080), '3200x1800'),
            ('2160', dict(window_size='2560x1080'), (1920, 1080), '3414x1440'),
        ]
        for height, settings, screen, expected in window:
            with self.subTest(height=height, settings=settings, screen=screen):
                self.assertEqual(self.size(screen, render_height=height, aspect='window', **settings), expected)


if __name__ == '__main__':
    unittest.main()
