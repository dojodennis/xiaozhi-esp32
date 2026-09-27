"""Execute the actual portal handlers with fragmented HTTP input and mocked radio/NVS."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

class OrbitWifiPortalTests(unittest.TestCase):
    def test_actual_handlers(self):
        root = pathlib.Path(__file__).resolve().parents[2]
        cjson = root / 'managed_components/espressif__cjson/cJSON'
        if not (cjson / 'cJSON.c').exists():
            self.skipTest('Prepare canonical firmware dependencies first')
        compiler = shutil.which('c++')
        c_compiler = shutil.which('cc')
        with tempfile.TemporaryDirectory() as folder:
            folder = pathlib.Path(folder)
            obj = folder / 'cjson.o'
            subprocess.run([c_compiler, '-c', str(cjson / 'cJSON.c'), '-o', str(obj)], check=True)
            binary = folder / 'portal-test'
            subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                            '-I', str(root / 'scripts/tests/orbit_wifi_stubs'),
                            '-I', str(root / 'components/esp-wifi-connect/include'), '-I', str(cjson),
                            str(root / 'components/esp-wifi-connect/orbit_wifi_portal.cc'),
                            str(root / 'scripts/tests/orbit_wifi_portal_test.cc'), str(obj),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
