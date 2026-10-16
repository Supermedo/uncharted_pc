import sys
import tempfile
import unittest
from pathlib import Path

from PIL import Image

from paths import ROOT

sys.path.insert(0, str(ROOT / 'launcher'))
import uncharted_art


class CustomArtworkTests(unittest.TestCase):
    def tearDown(self):
        uncharted_art.set_custom_art({})

    def test_custom_cover_and_banner_override_defaults(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cover = root / 'cover.png'
            banner = root / 'banner.png'
            Image.new('RGB', (200, 300), (220, 20, 30)).save(cover)
            Image.new('RGB', (600, 200), (20, 40, 220)).save(banner)

            uncharted_art.set_custom_art({'u1.jpg': cover, 'collection_banner.jpg': banner})

            self.assertEqual(uncharted_art.card_scene('u1', 200, 300).getpixel((100, 150)),
                             (220, 20, 30))
            self.assertEqual(uncharted_art.collection_banner(600, 200).getpixel((300, 20)),
                             (20, 40, 220))

    def test_one_custom_image_creates_all_steam_sizes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'collection.png'
            Image.new('RGB', (1200, 700), (30, 180, 90)).save(source)

            written = uncharted_art.steam_artwork('', root / 'grid', 1234, source)

            self.assertEqual({path.name for path in written},
                             {'1234p.png', '1234.png', '1234_hero.png'})
            expected = {'1234p.png': (600, 900), '1234.png': (920, 430),
                        '1234_hero.png': (1920, 620)}
            for path in written:
                with Image.open(path) as image:
                    self.assertEqual(image.size, expected[path.name])

    def test_crop_settings_keep_independent_focus(self):
        source = Image.new('RGB', (300, 100), (220, 20, 30))
        source.paste((20, 40, 220), (200, 0, 300, 100))

        left = uncharted_art.crop_image(source, (100, 100), {'x': 0, 'y': .5, 'zoom': 1})
        right = uncharted_art.crop_image(source, (100, 100), {'x': 1, 'y': .5, 'zoom': 1})

        self.assertEqual(left.getpixel((50, 50)), (220, 20, 30))
        self.assertEqual(right.getpixel((50, 50)), (20, 40, 220))

    def test_landscape_default_cover_fills_portrait_card(self):
        card = uncharted_art.card_scene('u3', 200, 300)

        self.assertEqual(card.size, (200, 300))
        self.assertNotEqual(card.getpixel((100, 0)), (9, 9, 9))


if __name__ == '__main__':
    unittest.main()
