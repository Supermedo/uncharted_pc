"""Render heap and fixed movie backing budgets in the Windows runner."""
from paths import ROOT
import runpy
import unittest


class ResolutionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = runpy.run_path(str(ROOT / 'run.py'))

    def test_4k_budgets_include_movie_backing(self):
        env = {'BB_GAME_RES': '3840x2160'}
        self.runner['game_res'](env, 54, arena64=True)
        self.assertEqual(env['BB_GAME_RT_MB'], '960')
        self.assertEqual(env['BB_GAME_RT2_MB'], '512')
        self.assertEqual(env['BB_DMEM_MB'], '6582')
        self.assertEqual(env['BB_GAME_MOVIE_MB'], '14')

    def test_explicit_budget_is_preserved(self):
        env = {'BB_GAME_RES': '3840x2160', 'BB_DMEM_MB': '8192'}
        self.runner['game_res'](env, 54, arena64=True)
        self.assertEqual(env['BB_DMEM_MB'], '8192')

    def test_unknown_build_cannot_exceed_original_pool(self):
        with self.assertRaises(SystemExit):
            self.runner['game_res']({'BB_GAME_RES': '3840x2160'})

    def test_pixel_budget(self):
        with self.assertRaises(SystemExit):
            self.runner['game_res']({'BB_GAME_RES': '5120x2160'}, 54, arena64=True)

    def test_original_movie_backing(self):
        env = {'BB_GAME_RES': '2560x1440'}
        self.runner['game_res'](env)
        self.assertEqual(int(env['BB_DMEM_MB']), 5056 + int(env['BB_GAME_RT_MB'])
                         + int(env['BB_GAME_RT2_MB']))


if __name__ == '__main__':
    unittest.main()
