"""Check publication identity consistency during concurrent ring reuse."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PeriodicStateTest(unittest.TestCase):
    def test_concurrent_publication(self):
        with tempfile.TemporaryDirectory(prefix='totk-state-') as directory:
            exe = Path(directory) / 'state-test'
            subprocess.run(['clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'runtime/macos'),
                            str(ROOT / 'runtime/macos/periodic_state.c'),
                            str(ROOT / 'tests/macos_periodic_state_test.c'), '-pthread', '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
