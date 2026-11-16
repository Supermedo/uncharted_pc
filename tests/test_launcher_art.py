# SPDX-License-Identifier: GPL-2.0-or-later
"""Uncharted launcher: each game's Steam library pictures (needs Pillow, as the launcher does)."""
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT

sys.path.insert(0, str(ROOT / 'launcher'))
try:
    from PIL import Image
    import uncharted_art as art
except ImportError:  # Pillow is in the packaging environment, not every Python
    art = None
import bbport_steam  # noqa: E402
import uncharted_launcher as launcher  # noqa: E402

KINDS = ('cover', 'header', 'hero', 'logo', 'icon')


@unittest.skipUnless(art, 'needs Pillow')
class SteamGameArt(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.root = Path(self.folder.name)
        self.own = self.root / 'art'
        self.own.mkdir()
        self.directories = art.art_directories
        covers = [d for d in self.directories() if d.name == 'covers']
        art.art_directories = lambda: [self.own] + covers  # the player's art folder, then the port's covers

    def tearDown(self):
        art.art_directories = self.directories
        self.folder.cleanup()

    def test_five_pictures_per_game_at_steam_sizes(self):
        covers = set()
        for appid, key in ((1, 'u1'), (2, 'u2'), (3, 'u3')):
            written = art.steam_game_art(self.root / 'grid', appid, key)
            self.assertEqual(sorted(written), sorted(KINDS))
            self.assertEqual(written['cover'].name, f'{appid}p.png')
            self.assertEqual(written['header'].name, f'{appid}.png')
            self.assertEqual(written['hero'].name, f'{appid}_hero.png')
            self.assertEqual(written['logo'].name, f'{appid}_logo.png')
            for kind, path in written.items():
                with Image.open(path) as image:
                    self.assertEqual(image.size, art.STEAM_KINDS[kind][1], (key, kind))
                    if kind == 'logo':  # the title on transparency
                        alpha = image.getchannel('A').getextrema()
                        self.assertEqual(alpha[0], 0)
                        self.assertGreater(alpha[1], 200)
            covers.add(written['cover'].read_bytes())
        self.assertEqual(len(covers), 3)  # a picture of its own for each game

    def test_own_box_art_is_used_whole(self):
        Image.new('RGB', (860, 1000), (200, 20, 20)).save(self.own / 'u2_cover.jpg')  # box art: not 2:3
        Image.new('RGBA', (100, 100), (0, 0, 255, 255)).save(self.own / 'u2_icon.png')
        written = art.steam_game_art(self.root / 'grid', 7, 'u2')
        with Image.open(written['cover']) as cover:
            self.assertEqual(cover.size, (600, 900))
            r, g, b = cover.convert('RGB').getpixel((300, 450))
            self.assertGreater(r, 180)
            self.assertLess(g, 60)
            top = cover.convert('RGB').getpixel((300, 10))  # the blurred, darkened band above the box
            self.assertLess(top[0], r)
        with Image.open(written['icon']) as icon:
            self.assertEqual(icon.size, (256, 256))
            self.assertEqual(icon.convert('RGB').getpixel((128, 128)), (0, 0, 255))
        other = art.steam_game_art(self.root / 'grid', 8, 'u1')  # only u2 has own art
        with Image.open(other['cover']) as cover:
            self.assertNotEqual(cover.convert('RGB').getpixel((300, 450)), (200, 20, 20))

    def test_add_steam_games_gives_each_entry_its_icon(self):
        config = self.root / 'config'
        launcher.add_steam_games(config, now=1)
        root = bbport_steam.parse((config / 'shortcuts.vdf').read_bytes())[0]
        entries = [{k: v for _t, k, v in value} for _kind, _key, value in root[0][2]]
        self.assertEqual(len(entries), 3)
        for entry in entries:
            appid = bbport_steam.shortcut_id(entry['Exe'], entry['AppName'])
            self.assertEqual(entry['icon'], str(config / 'grid' / f'{appid}_icon.png'))
            for name in (f'{appid}p.png', f'{appid}.png', f'{appid}_hero.png', f'{appid}_logo.png'):
                self.assertTrue((config / 'grid' / name).is_file(), name)


if __name__ == '__main__':
    unittest.main()
