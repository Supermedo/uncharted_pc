"""Loaded-image patch guards; no copyrighted executable fixtures are required."""
from paths import ROOT
import json
import struct
import unittest

import arena64


class ArenaTests(unittest.TestCase):
    def test_supported_images(self):
        manifest = json.loads(arena64.MANIFEST.read_text(encoding='utf-8'))
        for digest, build in manifest['builds'].items():
            with self.subTest(executable=build['file']):
                size = max(p['context_address'] + len(bytes.fromhex(p['context']))
                           for p in build['patches'])
                image = bytearray(size)
                for patch in build['patches']:
                    start = patch['context_address']
                    context = bytes.fromhex(patch['context'])
                    image[start:start + len(context)] = context
                original = bytes(image)
                segments = [(0, size, 5)]
                report = arena64.apply_arena64(image, segments, digest)
                self.assertTrue(report['supported'])
                self.assertEqual(len(report['patches']), len(build['patches']))
                stub = int(report['stub_base'], 16)
                self.assertEqual(stub % arena64.PAGE, 0)
                self.assertEqual(segments[-1][0], stub)
                self.assertEqual(segments[-1][2], 5)
                for patch in build['patches']:
                    start = patch['address']
                    expected = bytes.fromhex(patch['expected'])
                    self.assertEqual(original[start:start + len(expected)], expected)
                    if 'replacement' in patch:
                        self.assertEqual(image[start:start + len(expected)],
                                         bytes.fromhex(patch['replacement']))
                    else:
                        self.assertEqual(image[start], 0xe9)
                        target = start + 5 + struct.unpack_from('<i', image, start + 1)[0]
                        self.assertGreaterEqual(target, stub)
                        self.assertLess(target, stub + report['stub_bytes'])
                # Even the last guard must fail before any earlier patch is written.
                damaged = bytearray(original)
                damaged[build['patches'][-1]['context_address']] ^= 1
                before = bytes(damaged)
                segments = [(0, size, 5)]
                with self.assertRaises(ValueError):
                    arena64.apply_arena64(damaged, segments, digest)
                self.assertEqual(damaged, before)
                self.assertEqual(segments, [(0, size, 5)])

    def test_unknown_build_retains_original_image(self):
        image, segments = bytearray(b'unknown build'), [(0, 13, 5)]
        before = bytes(image)
        report = arena64.apply_arena64(image, segments, '0' * 64)
        self.assertFalse(report['supported'])
        self.assertEqual(image, before)
        self.assertEqual(segments, [(0, 13, 5)])


if __name__ == '__main__':
    unittest.main()
