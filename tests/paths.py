"""Repository paths for the Python tests (run: python3 -m unittest discover -s tests).

Also holds the two platform probes the suite needs. Some tests exercise POSIX
behaviour (bash scripts, symlinks) that is not available on every Windows
machine: `bash` there may be the WSL launcher stub, which fails when the
Virtual Machine Platform is disabled, and creating a symlink needs Developer Mode
or Administrator. Those tests skip rather than fail, because CI runs the Windows
job on a runner where neither is set up.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT / 'scripts') not in sys.path:
    sys.path.insert(0, str(ROOT / 'scripts'))


def bash_executable():
    """Path to a bash that can run this repository's scripts, or None.

    On Windows, `shutil.which('bash')` finds the WSL launcher, which prints an
    error and exits non-zero when the Virtual Machine Platform is off. Test it
    before handing it to a subprocess.
    """
    found = shutil.which('bash')
    if not found:
        return None
    try:
        result = subprocess.run([found, '-c', 'echo bbport-bash-ok'],
                                capture_output=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None
    return found if b'bbport-bash-ok' in result.stdout else None


def symlinks_available():
    """Whether creating a symlink works in the temp directory."""
    probe = Path(tempfile.gettempdir()) / f'bbport-symlink-probe-{os.getpid()}'
    try:
        probe.mkdir(parents=True, exist_ok=True)
        (probe / 'link').symlink_to(probe, target_is_directory=True)
        return True
    except (OSError, NotImplementedError, AttributeError):
        return False
    finally:
        shutil.rmtree(probe, ignore_errors=True)


requires_bash = unittest.skipUnless(bash_executable(),
                                    'needs a working bash for the POSIX scripts')
requires_symlinks = unittest.skipUnless(symlinks_available(),
                                        'needs symlink support (Windows: Developer Mode)')