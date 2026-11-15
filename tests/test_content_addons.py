"""Standalone AppContent tests; no GPU, game data or real DLC metadata needed."""
from paths import ROOT
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


class AddonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which(os.environ.get('CC', 'clang'))
        if not compiler:
            raise unittest.SkipTest('set CC to a C compiler supporting sysv_abi')
        cls.work = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.work.cleanup)
        cls.directory = Path(cls.work.name)
        cls.executables = {}
        for name in ('content', 'content_addons'):
            executable = cls.directory / (name + ('.exe' if os.name == 'nt' else ''))
            subprocess.run([compiler, '-std=c11', '-D_GNU_SOURCE', '-DNDEBUG', '-O2',
                            '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                            str(ROOT / 'tests' / ('test_' + name + '.c')),
                            str(ROOT / 'src/runtime_content.c'), '-o', str(executable)],
                           check=True, capture_output=True, text=True, timeout=60)
            cls.executables[name] = executable

    def test_existing_base_profile_behavior(self):
        for args in ([], ['--missing'], ['--unknown']):
            with self.subTest(args=args):
                result = subprocess.run([str(self.executables['content']), *args],
                                        capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 21 if args == ['--missing'] else 0,
                                 result.stdout + result.stderr)

    def test_metadata_and_api_fixtures(self):
        result = subprocess.run([sys.executable, str(ROOT / 'tests/content_addon_fixtures.py'),
                                 str(self.directory / 'fixtures'),
                                 str(self.executables['content_addons'])],
                                capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS 52 parser/API fixtures', result.stdout)
