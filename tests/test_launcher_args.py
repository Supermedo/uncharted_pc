"""The launcher's command line: --u1/--u2/--u3 or --game <key> name the game to start."""
from paths import ROOT
import importlib.util
import unittest


def load_launcher():
    spec = importlib.util.spec_from_file_location('uncharted_launcher_module',
                                                  ROOT / 'launcher/uncharted_launcher.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class RequestedGameTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.launcher = load_launcher()

    def test_forms(self):
        game = self.launcher.requested_game
        self.assertEqual(game(['--u2']), 'u2')
        self.assertEqual(game(['--game', 'u3']), 'u3')
        self.assertEqual(game(['--game=U1']), 'u1')
        self.assertEqual(game(['-g', 'u2', '--play']), 'u2')

    def test_nothing_requested(self):
        game = self.launcher.requested_game
        self.assertIsNone(game([]))
        self.assertIsNone(game(['--play']))
        self.assertIsNone(game(['--game', 'u4']))
        self.assertIsNone(game(['--game']))


if __name__ == '__main__':
    unittest.main()
