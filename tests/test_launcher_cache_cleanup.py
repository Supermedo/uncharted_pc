from paths import ROOT
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock


spec = importlib.util.spec_from_file_location(
    'uncharted_launcher', ROOT / 'launcher/uncharted_launcher.py')
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)


def window(user_dir, process=None):
    instance = launcher.Launcher.__new__(launcher.Launcher)
    instance.app = {'user_dir': str(user_dir)}
    instance.process = process
    instance.messagebox = mock.Mock()
    return instance


class ShaderCacheCleanupTests(unittest.TestCase):
    def test_existing_cache_is_removed_and_success_is_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / 'cache'
            nested = cache / 'pipelines/game'
            nested.mkdir(parents=True)
            (nested / 'pipeline.bin').write_bytes(b'cache')
            launcher_window = window(directory)
            launcher_window.clear_cache()
            self.assertFalse(cache.exists())
            launcher_window.messagebox.showinfo.assert_called_once()
            launcher_window.messagebox.showerror.assert_not_called()

    def test_missing_cache_is_already_a_success(self):
        with tempfile.TemporaryDirectory() as directory:
            launcher_window = window(directory)
            launcher_window.clear_cache()
            launcher_window.messagebox.showinfo.assert_called_once()
            launcher_window.messagebox.showerror.assert_not_called()

    def test_default_user_folder_is_used(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / 'user/cache'
            cache.mkdir(parents=True)
            launcher_window = window('')
            with mock.patch.object(launcher, 'DATA_DIR', Path(directory)):
                launcher_window.clear_cache()
            self.assertFalse(cache.exists())
            launcher_window.messagebox.showinfo.assert_called_once()

    def test_file_at_cache_path_is_reported_without_deleting_it(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / 'cache'
            cache.write_bytes(b'not a directory')
            launcher_window = window(directory)
            launcher_window.clear_cache()
            self.assertTrue(cache.is_file())
            launcher_window.messagebox.showerror.assert_called_once()
            launcher_window.messagebox.showinfo.assert_not_called()

    def test_cleanup_failure_is_reported_without_false_success(self):
        launcher_window = window('user')
        failure = PermissionError('cache is in use')
        with mock.patch.object(launcher.shutil, 'rmtree', side_effect=failure):
            launcher_window.clear_cache()
        launcher_window.messagebox.showerror.assert_called_once_with(
            launcher.NAME, 'Could not clear the shader cache: cache is in use')
        launcher_window.messagebox.showinfo.assert_not_called()

    @unittest.skipUnless(os.name == 'nt', 'Windows locks open files')
    def test_locked_windows_cache_reports_real_filesystem_error(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / 'cache'
            cache.mkdir()
            pipeline = cache / 'pipeline.bin'
            pipeline.write_bytes(b'cache')
            launcher_window = window(directory)
            with pipeline.open('rb'):
                launcher_window.clear_cache()
                self.assertTrue(cache.exists())
            message = launcher_window.messagebox.showerror.call_args.args[1]
            self.assertIn('Could not clear the shader cache:', message)
            self.assertIn('WinError 32', message)
            launcher_window.messagebox.showinfo.assert_not_called()
            launcher_window.clear_cache()
            self.assertFalse(cache.exists())
            launcher_window.messagebox.showinfo.assert_called_once()

    def test_running_game_keeps_cache_untouched(self):
        launcher_window = window('user', process=object())
        with mock.patch.object(launcher.shutil, 'rmtree') as rmtree:
            launcher_window.clear_cache()
        rmtree.assert_not_called()
        launcher_window.messagebox.showinfo.assert_not_called()
        launcher_window.messagebox.showerror.assert_not_called()


if __name__ == '__main__':
    unittest.main()
