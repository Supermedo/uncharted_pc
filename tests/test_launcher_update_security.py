"""Launcher update integrity and extraction tests."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import stat
import tempfile
import unittest
from unittest import mock
import zipfile

from paths import ROOT


SPEC = importlib.util.spec_from_file_location(
    'uncharted_launcher', ROOT / 'launcher' / 'uncharted_launcher.py')
LAUNCHER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LAUNCHER)


class Response(io.BytesIO):
    def __init__(self, data, content_length=None):
        super().__init__(data)
        self.headers = {'Content-Length': str(content_length if content_length is not None else len(data))}


class ReleaseTests(unittest.TestCase):
    def release(self, digest):
        return {
            'tag_name': 'v0.4.0',
            'html_url': 'https://example.test/release',
            'assets': [{'name': 'uncharted-windows.zip',
                        'browser_download_url': 'https://example.test/update.zip',
                        'digest': digest}],
        }

    def test_latest_release_returns_verified_asset(self):
        digest = 'sha256:' + 'a' * 64
        response = Response(json.dumps(self.release(digest)).encode())
        with mock.patch.object(LAUNCHER.urllib.request, 'urlopen', return_value=response):
            self.assertEqual(LAUNCHER.latest_release(),
                             ('0.4.0', 'https://example.test/update.zip', digest,
                              'https://example.test/release'))

    def test_unverifiable_release_opens_page_instead_of_downloading(self):
        response = Response(json.dumps(self.release(None)).encode())
        with mock.patch.object(LAUNCHER.urllib.request, 'urlopen', return_value=response):
            version, url, digest, page = LAUNCHER.latest_release()
        self.assertEqual((version, url, digest), ('0.4.0', None, None))
        self.assertEqual(page, 'https://example.test/release')


class DownloadTests(unittest.TestCase):
    def test_matching_digest_publishes_download(self):
        data = b'complete release archive'
        digest = 'sha256:' + hashlib.sha256(data).hexdigest()
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / 'update.zip'
            progress = []
            with mock.patch.object(LAUNCHER.urllib.request, 'urlopen',
                                   return_value=Response(data)):
                LAUNCHER.download_update('https://example.test/update.zip', digest,
                                         destination, progress.append)
            self.assertEqual(destination.read_bytes(), data)
            self.assertEqual(progress, [100])

    def test_digest_mismatch_rejects_download(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / 'update.zip'
            digest = 'sha256:' + '0' * 64
            with mock.patch.object(LAUNCHER.urllib.request, 'urlopen',
                                   return_value=Response(b'altered')):
                with self.assertRaisesRegex(OSError, 'does not match'):
                    LAUNCHER.download_update('https://example.test/update.zip', digest, destination)
            self.assertFalse(destination.exists())
            self.assertFalse(destination.with_suffix('.zip.part').exists())


class ExtractionTests(unittest.TestCase):
    def archive(self, entries):
        data = io.BytesIO()
        with zipfile.ZipFile(data, 'w') as package:
            for name, contents in entries:
                package.writestr(name, contents)
        data.seek(0)
        return zipfile.ZipFile(data)

    def test_regular_files_are_extracted(self):
        with tempfile.TemporaryDirectory() as directory, \
                self.archive([('Uncharted/Uncharted.exe', b'exe'),
                              ('Uncharted/bin/runtime.dll', b'dll')]) as package:
            destination = Path(directory) / 'new'
            LAUNCHER.extract_update(package, destination)
            self.assertEqual((destination / 'Uncharted/Uncharted.exe').read_bytes(), b'exe')
            self.assertEqual((destination / 'Uncharted/bin/runtime.dll').read_bytes(), b'dll')

    def test_parent_absolute_drive_and_backslash_paths_are_rejected(self):
        for name in ('../escape', '/absolute', 'C:/drive', '..\\backslash'):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory, \
                    self.archive([(name, b'bad')]) as package:
                with self.assertRaisesRegex(ValueError, 'unsafe path'):
                    LAUNCHER.extract_update(package, Path(directory) / 'new')

    def test_symbolic_links_are_rejected(self):
        data = io.BytesIO()
        with zipfile.ZipFile(data, 'w') as package:
            info = zipfile.ZipInfo('Uncharted/link')
            info.create_system = 3
            info.external_attr = (stat.S_IFLNK | 0o777) << 16
            package.writestr(info, '../outside')
        data.seek(0)
        with tempfile.TemporaryDirectory() as directory, zipfile.ZipFile(data) as package:
            with self.assertRaisesRegex(ValueError, 'unsafe path'):
                LAUNCHER.extract_update(package, Path(directory) / 'new')


if __name__ == '__main__':
    unittest.main()
