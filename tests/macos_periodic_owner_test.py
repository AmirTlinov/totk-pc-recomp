"""Run addressed-continuation regressions without game data or Metal."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PeriodicOwnerTest(unittest.TestCase):
    def test_addressed_continuations(self):
        with tempfile.TemporaryDirectory(prefix='totk-owner-') as directory:
            exe = Path(directory) / 'owner-test'
            subprocess.run(['clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'runtime/macos'),
                            str(ROOT / 'runtime/macos/periodic_owner.c'),
                            str(ROOT / 'tests/macos_periodic_owner_test.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
