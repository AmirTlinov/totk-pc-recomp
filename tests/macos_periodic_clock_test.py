"""Check clock domains without starting the guest, staging sources or using Metal."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PeriodicClockTests(unittest.TestCase):
    def test_clock_domains(self):
        with tempfile.TemporaryDirectory(prefix='totk-periodic-clock-') as directory:
            binary = Path(directory) / 'clock-test'
            subprocess.run(['xcrun', 'clang', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', ROOT / 'runtime/macos',
                            ROOT / 'runtime/macos/periodic_clock.c',
                            ROOT / 'tests/macos_periodic_clock_test.c', '-o', binary], check=True)
            subprocess.run([binary], check=True)


if __name__ == '__main__':
    unittest.main()
