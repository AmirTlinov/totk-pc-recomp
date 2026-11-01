"""Exercise the production snapshot writer at its former torn-publication window."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PeriodicCameraTests(unittest.TestCase):
    def test_concurrent_publication(self):
        with tempfile.TemporaryDirectory(prefix='totk-camera-') as directory:
            binary = Path(directory) / 'camera-test'
            subprocess.run(['xcrun', 'clang', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-DPERIODIC_CAMERA_TEST_HOOK=1',
                            '-I', ROOT / 'runtime/macos', ROOT / 'runtime/macos/periodic_camera.c',
                            ROOT / 'tests/macos_periodic_camera_test.c', '-o', binary], check=True)
            subprocess.run([binary], check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()
