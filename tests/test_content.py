from paths import ROOT
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import content_profile

EXE=ROOT/'out/content-test'

class ProfileTests(unittest.TestCase):
    def test_sfo_parameters_and_explicit_trial_profile(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'sce_sys').mkdir();(p/'sce_sys/param.sfo').write_bytes(b'fixture')
            with patch.object(content_profile,'sfo',return_value={'USER_DEFINED_PARAM_1':13,'USER_DEFINED_PARAM_4':0xffffffff}):
                content_profile.prepare(p,p,'trial')
            self.assertEqual(struct.unpack('<8s5I',(p/'content.bin').read_bytes()),(b'BBCONT01',1,13,0,0,0xffffffff))

    def test_missing_param_1_is_the_title_number(self):
        for sfo_values, expected in (({'TITLE_ID': 'CUSA02320'}, 2320), ({'TITLE_ID': 'CUSA02343'}, 2343),
                                     ({'TITLE_ID': 'CUSA02320', 'USER_DEFINED_PARAM_1': 7}, 7),
                                     ({}, 0)):
            with tempfile.TemporaryDirectory() as tmp:
                p=Path(tmp);(p/'sce_sys').mkdir();(p/'sce_sys/param.sfo').write_bytes(b'fixture')
                with patch.object(content_profile,'sfo',return_value=sfo_values):
                    content_profile.prepare(p,p)
                self.assertEqual(struct.unpack('<8s5I',(p/'content.bin').read_bytes())[2],expected,sfo_values)

    def test_invalid_sfo_parameter_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'sce_sys').mkdir();(p/'sce_sys/param.sfo').write_bytes(b'fixture')
            with patch.object(content_profile,'sfo',return_value={'USER_DEFINED_PARAM_1':'13'}):
                with self.assertRaisesRegex(ValueError,'invalid user-defined'):
                    content_profile.prepare(p,p)

@unittest.skipUnless(EXE.exists(),'run bash build.sh --test first')
class ContentTests(unittest.TestCase):
    def run_case(self,*args):
        return subprocess.run([str(EXE.resolve()),*args],capture_output=True,text=True,timeout=5)

    def test_lifecycle_and_output_boundaries(self):
        r=self.run_case();self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_missing_profile_does_not_report_loaded(self):
        r=self.run_case('--missing');self.assertEqual(r.returncode,21)
        self.assertIn('needs --content-profile',r.stderr)

    def test_other_modules_are_counted_independently(self):
        r=self.run_case('--unknown');self.assertEqual(r.returncode,0,r.stdout+r.stderr)
