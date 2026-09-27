import pathlib
import shutil
import subprocess
import tempfile
import unittest

class OrbitWifiSessionTests(unittest.TestCase):
    def test_expiry_cancel_commit_and_credentials(self):
        root = pathlib.Path(__file__).resolve().parents[2]
        compiler = shutil.which('c++')
        if not compiler:
            self.skipTest('host C++ compiler unavailable')
        with tempfile.TemporaryDirectory() as folder:
            binary = pathlib.Path(folder) / 'orbit-wifi-session'
            subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                            '-I', str(root / 'components/esp-wifi-connect/include'),
                            str(root / 'scripts/tests/orbit_wifi_session_test.cc'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
