"""The Windows package must ship every preparation script that run.py runs.

package.sh copies a hand-written list out of scripts/ while run.py calls the
scripts by name, so the two can drift apart. This is not only cosmetic: run.py
calls the patch compiler with tolerant=True, so a script missing from the
package lets the game start anyway, silently without its FPS and resolution
patches. patches.py was missed exactly this way.
"""
import re
import unittest
from pathlib import Path

from paths import ROOT


def scripts_run_by_run_py():
    """The <name>.py files run.py invokes with run_script()/run_script_capture()."""
    text = (ROOT / 'run.py').read_text(encoding='utf-8')
    return set(re.findall(r"run_script(?:_capture)?\(\s*'([^']+)'", text))


def scripts_copied_by_package_sh():
    """The scripts/*.py files package.sh copies into the packaged scripts/ folder."""
    text = (ROOT / 'packaging' / 'windows' / 'package.sh').read_text(encoding='utf-8')
    # The copy command is continued across lines with backslashes; join it first.
    joined = re.sub(r'\\\n\s*', ' ', text)
    match = re.search(r'\bcp\s+([^"\n]*?)\s+"\$dest/scripts/"', joined)
    if not match:
        return set()
    return {Path(name).name for name in match.group(1).split() if name.startswith('scripts/')}


class PackageScriptsTest(unittest.TestCase):
    def test_run_py_scripts_are_all_shipped(self):
        required = scripts_run_by_run_py()
        copied = scripts_copied_by_package_sh()
        # Guard against the regexes going stale: both sides must find something.
        self.assertTrue(required, 'run.py: no script invocations found')
        self.assertTrue(copied, 'package.sh: no scripts/ copy command found')
        self.assertEqual(required - copied, set(),
                         'run.py runs these but package.sh does not ship them: '
                         f'{sorted(required - copied)}')

    def test_shipped_scripts_exist(self):
        for name in scripts_copied_by_package_sh():
            self.assertTrue((ROOT / 'scripts' / name).is_file(),
                            f'package.sh ships scripts/{name}, which is not in the tree')


if __name__ == '__main__':
    unittest.main()
