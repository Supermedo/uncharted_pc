# SPDX-License-Identifier: GPL-2.0-or-later
"""Uncharted launcher: --play <game> and the three Steam shortcuts (pure Python, any host)."""
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT

sys.path.insert(0, str(ROOT / 'launcher'))
import bbport_steam  # noqa: E402
import uncharted_launcher as launcher  # noqa: E402
from bbport_steam import END, INT32, MAP, STRING  # noqa: E402


class PlayArguments(unittest.TestCase):
    def test_no_play(self):
        self.assertIsNone(launcher.play_request([]))
        self.assertIsNone(launcher.play_request(['--run']))

    def test_plain_play_is_the_selected_game(self):
        self.assertEqual(launcher.play_request(['--play']), '')
        self.assertEqual(launcher.play_request(['--play', '--other']), '')

    def test_play_game(self):
        self.assertEqual(launcher.play_request(['--play', 'u2']), 'u2')
        self.assertEqual(launcher.play_request(['--play', 'U3']), 'u3')
        self.assertEqual(launcher.play_request(['script.py', '--play', 'u1']), 'u1')

    def test_unknown_or_unavailable_game_is_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            settings = {**launcher.APP_DEFAULTS, 'game_dir': folder}
            self.assertIn('Unknown game "u4"', launcher.play_problem(settings, 'u4'))
            self.assertIn('Uncharted 2: Among Thieves is not available', launcher.play_problem(settings, 'u2'))
            (Path(folder) / 'big2-ps4_Shipping.elf').write_bytes(b'')
            self.assertIsNone(launcher.play_problem(settings, 'u2'))
            self.assertIsNotNone(launcher.play_problem(settings, 'u3'))

    def test_play_uses_that_game(self):
        settings = {**launcher.APP_DEFAULTS, 'game_dir': 'x', 'start': 'u1'}
        self.assertEqual(launcher.game_environment(settings, 'u3')['BB_EXE'], 'u3')
        self.assertEqual(launcher.game_environment(settings)['BB_EXE'], 'u1')

    def test_play_game_goes_past_the_collection_menu(self):
        settings = {**launcher.APP_DEFAULTS, 'game_dir': 'x', 'start': 'u3'}
        env = launcher.play_environment(settings, 'u1')
        self.assertEqual((env['BB_EXE'], env['BB_START_GAME']), ('u1', 'u1'))
        # plain --play (the selected game) shows the menu as before, whatever the outer environment says
        launcher.os.environ['BB_START_GAME'] = 'u1'
        try:
            self.assertNotIn('BB_START_GAME', launcher.play_environment(settings))
        finally:
            del launcher.os.environ['BB_START_GAME']


def shortcut(name, exe, options, last_play=0):
    entry = bbport_steam.entry(name, exe, str(Path(exe).parent), exe, options)
    return [(t, k, last_play if k == 'LastPlayTime' else v) for t, k, v in entry]


def fields(entry):
    return {k: v for _t, k, v in entry}


class SteamShortcuts(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.vdf = Path(self.folder.name) / 'shortcuts.vdf'
        self.exe, self.play = launcher.steam_launch()
        exe = str(self.exe)
        self.others = {
            '0': shortcut('Other game', r'C:\Games\other.exe', ''),
            # the collection, but another copy of the launcher: not ours to replace
            '2': shortcut(launcher.COLLECTION_TITLE, r'C:\Elsewhere\Uncharted.exe', '--play'),
            # this launcher with other options: left alone
            '3': shortcut('Uncharted (custom)', exe, self.play + ' u2 --custom'),
        }
        self.old = shortcut(launcher.COLLECTION_TITLE, exe, self.play, last_play=1234)
        items = [(MAP, '0', self.others['0']), (MAP, '1', self.old), (MAP, '2', self.others['2']),
                 (MAP, '3', self.others['3'])]
        self.original = bbport_steam.dump([(MAP, 'shortcuts', items)]) + bytes([END])
        self.vdf.write_bytes(self.original)

    def tearDown(self):
        self.folder.cleanup()

    def entries(self):
        root = bbport_steam.parse(self.vdf.read_bytes())[0]
        self.assertEqual(bbport_steam.dump(root) + bytes([END]), self.vdf.read_bytes())
        return {key: value for _kind, key, value in root[0][2]}

    def test_three_entries_replace_the_old_collection_entry(self):
        exe, (results, saved) = launcher.add_steam_shortcuts(self.vdf, now=1700000000)
        self.assertEqual(exe, self.exe)
        self.assertEqual(results, ['replaced', 'added', 'added'])
        entries = self.entries()
        self.assertEqual(sorted(entries), ['0', '1', '2', '3', '4', '5'])
        for key, value in self.others.items():  # untouched, same slot
            self.assertEqual(entries[key], value)
        wanted = {'1': ("Uncharted: Drake's Fortune", 'u1'), '4': ('Uncharted 2: Among Thieves', 'u2'),
                  '5': ("Uncharted 3: Drake's Deception", 'u3')}
        appids = set()
        for key, (name, game) in wanted.items():
            f = fields(entries[key])
            self.assertEqual(f['AppName'], name)
            self.assertEqual(f['Exe'], f'"{self.exe}"')
            self.assertEqual(f['LaunchOptions'], f'{self.play} {game}')
            self.assertEqual(launcher.play_request(f['LaunchOptions'].split()[-2:]), game)
            appids.add(f['appid'])
        self.assertEqual(len(appids), 3)
        self.assertFalse(any(fields(v)['AppName'] == launcher.COLLECTION_TITLE and fields(v)['Exe'] == f'"{self.exe}"'
                             for v in entries.values()))
        self.assertEqual(saved.name, 'shortcuts.vdf.%s.bak' % bbport_steam.time.strftime(
            '%Y%m%d-%H%M%S', bbport_steam.time.localtime(1700000000)))
        self.assertEqual(saved.read_bytes(), self.original)

    def test_again_updates_and_keeps_a_backup_per_write(self):
        _exe, (_results, first) = launcher.add_steam_shortcuts(self.vdf, now=1700000000)
        after_first = self.vdf.read_bytes()
        _exe, (results, second) = launcher.add_steam_shortcuts(self.vdf, now=1700000000)
        self.assertEqual(results, ['updated', 'updated', 'updated'])
        self.assertEqual(len(self.entries()), 6)
        self.assertNotEqual(first, second)
        self.assertEqual(first.read_bytes(), self.original)
        self.assertEqual(second.read_bytes(), after_first)
        self.assertEqual(self.vdf.read_bytes(), after_first)

    def test_kept_fields_survive_an_update(self):
        launcher.add_steam_shortcuts(self.vdf, now=1)
        root = bbport_steam.parse(self.vdf.read_bytes())[0]
        u2 = next(v for _k, key, v in root[0][2] if key == '4')
        u2[:] = [(t, k, 99 if k == 'LastPlayTime' else v) for t, k, v in u2]
        self.vdf.write_bytes(bbport_steam.dump(root) + bytes([END]))
        launcher.add_steam_shortcuts(self.vdf, now=2)
        self.assertEqual(fields(self.entries()['4'])['LastPlayTime'], 99)

    def test_new_file(self):
        self.vdf.unlink()
        _exe, (results, saved) = launcher.add_steam_shortcuts(self.vdf)
        self.assertEqual(results, ['added'] * 3)
        self.assertIsNone(saved)
        self.assertEqual(sorted(self.entries()), ['0', '1', '2'])

    def test_collection_entry_replaces_the_three_games(self):
        launcher.add_steam_shortcuts(self.vdf, now=1)
        exe, (results, saved) = launcher.add_collection_shortcut(self.vdf, now=2)
        self.assertEqual(exe, self.exe)
        self.assertEqual(results, ['replaced'])
        entries = self.entries()
        self.assertEqual(sorted(entries), ['0', '1', '2', '3'])  # the two extra game entries gone
        for key, value in self.others.items():  # untouched, same slot (u2 --custom too)
            self.assertEqual(entries[key], value)
        f = fields(entries['1'])
        self.assertEqual((f['AppName'], f['Exe'], f['LaunchOptions']),
                         (launcher.COLLECTION_TITLE, f'"{self.exe}"', self.play))
        self.assertEqual(launcher.play_request(f['LaunchOptions'].split()[-1:]), '')
        self.assertIsNotNone(saved)

    def test_collection_again_updates(self):
        launcher.add_collection_shortcut(self.vdf, now=1)
        _exe, (results, _saved) = launcher.add_collection_shortcut(self.vdf, now=2)
        self.assertEqual(results, ['updated'])
        self.assertEqual(len(self.entries()), 4)

    def test_kinds(self):
        f = {k: t for t, k, _v in bbport_steam.entry('n', 'e', 's', 'i', 'o')}
        self.assertEqual((f['appid'], f['LaunchOptions']), (INT32, STRING))


if __name__ == '__main__':
    unittest.main()
