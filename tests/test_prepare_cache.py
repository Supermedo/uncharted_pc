"""Prepared executable cache tests."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from paths import ROOT
from link_modules import atomic_output


SPEC = importlib.util.spec_from_file_location('bb_run', ROOT / 'run.py')
RUN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUN)


class PrepareCacheTests(unittest.TestCase):
    def fixture(self, directory):
        root = Path(directory)
        port = root / 'port'
        game = root / 'game'
        out = root / 'out'
        (port / 'scripts').mkdir(parents=True)
        (game / 'sce_module').mkdir(parents=True)
        (port / 'scripts/prepare.py').write_text('script', encoding='utf-8')
        (game / 'eboot.bin').write_bytes(b'game')
        (game / 'sce_module/libc.prx').write_bytes(b'module')
        return port, game, out

    def write_manifest(self, game, out, size, **changes):
        metadata = {
            'version': RUN.PREPARE_CACHE_VERSION,
            'source': str(game / 'eboot.bin'),
            'linked_size': size,
        }
        metadata.update(changes)
        (out / 'prepared.json').write_text(json.dumps(metadata), encoding='utf-8')

    def test_complete_matching_cache_is_reused(self):
        with tempfile.TemporaryDirectory() as directory:
            port, game, out = self.fixture(directory)
            out.mkdir()
            linked = out / 'boot-linked.bin'
            linked.write_bytes(b'complete')
            self.write_manifest(game, out, linked.stat().st_size)

            with mock.patch.object(RUN, 'PORT', port), \
                    mock.patch.object(RUN, 'run_script') as run_script:
                RUN.prepare(game, 'eboot.bin', out)

            run_script.assert_not_called()

    def test_missing_or_mismatched_manifest_rebuilds_cache(self):
        cases = (None, {'linked_size': 1}, {'version': 0}, {'source': 'another game'})
        for changes in cases:
            with self.subTest(changes=changes), tempfile.TemporaryDirectory() as directory:
                port, game, out = self.fixture(directory)
                out.mkdir()
                linked = out / 'boot-linked.bin'
                linked.write_bytes(b'stale')
                if changes is not None:
                    self.write_manifest(game, out, linked.stat().st_size, **changes)

                def run_script(name, *_args):
                    if name == 'link_modules.py':
                        linked.write_bytes(b'complete')

                with mock.patch.object(RUN, 'PORT', port), \
                        mock.patch.object(RUN, 'run_script', side_effect=run_script) as called:
                    RUN.prepare(game, 'eboot.bin', out)

                self.assertEqual([call.args[0] for call in called.call_args_list],
                                 ['prepare.py', 'link_modules.py'])
                metadata = json.loads((out / 'prepared.json').read_text(encoding='utf-8'))
                self.assertEqual(metadata['linked_size'], len(b'complete'))

    def test_failed_link_does_not_publish_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            port, game, out = self.fixture(directory)
            with mock.patch.object(RUN, 'PORT', port), \
                    mock.patch.object(RUN, 'run_script', side_effect=[None, OSError('failed')]):
                with self.assertRaises(OSError):
                    RUN.prepare(game, 'eboot.bin', out)
            self.assertFalse((out / 'prepared.json').exists())


class AtomicLinkedImageTests(unittest.TestCase):
    def test_success_replaces_previous_image(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'boot-linked.bin'
            path.write_bytes(b'old')
            with atomic_output(path) as stream:
                stream.write(b'new')
            self.assertEqual(path.read_bytes(), b'new')

    def test_failed_write_preserves_previous_image(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'boot-linked.bin'
            path.write_bytes(b'old')
            with self.assertRaises(OSError):
                with atomic_output(path) as stream:
                    stream.write(b'partial')
                    raise OSError('failed')
            self.assertEqual(path.read_bytes(), b'old')
            self.assertEqual(list(path.parent.glob('.boot-linked.bin.*.tmp')), [])


if __name__ == '__main__':
    unittest.main()
