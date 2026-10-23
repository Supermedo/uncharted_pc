"""Wide-screen field of view and UI patches of Uncharted 1 1.02 (scripts/ui_aspect.py), on a synthetic image."""
import hashlib
import importlib.util
import struct
import sys
import unittest

from paths import ROOT


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


class UiAspectTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ui = load('ui_aspect_under_test', 'scripts/ui_aspect.py')

    def image(self):
        """address -> the bytes the game has there, for every site."""
        ui, memory = self.ui, {}
        memory[ui.FOV_NUMERATOR] = struct.pack('<f', 1.0)
        for address in ui.SCALES:
            memory[address] = bytes.fromhex(ui.F_2_1280)
        for address in ui.OFFSETS:
            memory[address] = bytes.fromhex(ui.F_M1)
        for address, expected, _at, _target in [ui.SCALAR_OFFSET]:
            memory[address] = bytes.fromhex(expected)
        for address, expected, _at in ui.LEFT + ui.RIGHT:
            memory[address] = bytes.fromhex(expected)
        return memory

    def plan(self, memory, width, height):
        return self.ui.plan(lambda a, n: memory.get(a) if len(memory.get(a, b'')) == n else None, width, height)

    def test_sixteen_by_nine_and_narrower_change_nothing(self):
        self.assertEqual(self.plan(self.image(), 1920, 1080), [])
        self.assertEqual(self.plan(self.image(), 2560, 1440), [])
        self.assertEqual(self.plan(self.image(), 1600, 1200), [])

    def test_ultrawide_scales_and_offsets_follow_the_aspect(self):
        writes = dict(self.plan(self.image(), 2560, 1080))
        k = (2560 / 1080) / (16 / 9)
        self.assertAlmostEqual(struct.unpack('<f', writes[self.ui.FOV_NUMERATOR])[0], 1 / k, places=6)
        for address in self.ui.SCALES:
            self.assertAlmostEqual(struct.unpack('<f', writes[address])[0], 2 / (1280 * k), places=9)
        for address in self.ui.OFFSETS:
            self.assertAlmostEqual(struct.unpack('<f', writes[address])[0], -1 / k, places=6)

    def test_safe_area_moves_out_by_half_the_extra_width(self):
        writes = dict(self.plan(self.image(), 2560, 1080))
        half = round(640 * ((2560 / 1080) / (16 / 9) - 1))
        left = self.ui.LEFT[0]
        self.assertEqual(struct.unpack('<i', writes[left[0] + left[2]])[0], 50 - half)
        right = self.ui.RIGHT[0]
        self.assertEqual(struct.unpack('<I', writes[right[0] + right[2]])[0], 1229 + half)

    def test_scalar_offset_operand_points_at_the_vector_lane(self):
        writes = dict(self.plan(self.image(), 2560, 1080))
        address, expected, at, _target = self.ui.SCALAR_OFFSET
        displacement = struct.unpack('<i', writes[address + at])[0]
        self.assertEqual(address + len(expected) // 2 + displacement, self.ui.OFFSETS[0])

    def test_unexpected_bytes_raise(self):
        memory = self.image()
        memory[self.ui.SCALES[0]] = b'\0\0\0\0'
        with self.assertRaises(self.ui.Mismatch):
            self.plan(memory, 2560, 1080)

    def test_parse_size(self):
        self.assertEqual(self.ui.parse_size('3440×1440'), (3440, 1440))
        self.assertIsNone(self.ui.parse_size('wide'))


if __name__ == '__main__':
    unittest.main()
