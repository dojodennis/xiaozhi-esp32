import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/m5stack/stopwatch"
spec = importlib.util.spec_from_file_location("capture", ROOT / "scripts/orbit_usb_capture.py")
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


class OrbitUsbSelfTestTests(unittest.TestCase):
    def test_profile_only_enables_explicit_bench_switch(self):
        def settings(name):
            values = json.loads((BOARD / name).read_text())["builds"][0]["sdkconfig_append"]
            return dict(line.split("=", 1) for line in values)
        normal = settings("bench_profile.json")
        diagnostic = settings("usb_selftest_profile.json")
        self.assertEqual(normal.pop("CONFIG_ORBIT_USB_SELFTEST"), "n")
        self.assertEqual(diagnostic.pop("CONFIG_ORBIT_USB_SELFTEST"), "y")
        self.assertEqual(normal, diagnostic)

    def test_entrypoint_is_offline_and_readonly(self):
        source = (BOARD / "usb_selftest_main.cc").read_text()
        for forbidden in ("app.Initialize(", "app.Run(", "StartNetwork(", "GetAudioCodec(",
                          "nvs_flash_erase(", "esp_ota_mark_app_valid", "SetString("):
            self.assertNotIn(forbidden, source)
        self.assertIn("return __real_nvs_open(name, NVS_READONLY, handle)", source)
        self.assertIn('command == "ORBIT_SELFTEST_V1 RUN"', source)
        self.assertIn("deadline", source)
        cmake = (ROOT / "main/CMakeLists.txt").read_text()
        self.assertIn('--wrap=nvs_open', cmake)
        self.assertIn('list(REMOVE_ITEM SOURCES "main.cc")', cmake)

    def test_theme_fixture_never_persists_preferences(self):
        source = (ROOT / "main/display/display.cc").read_text()
        theme = source.split("void Display::SetTheme(Theme* theme) {", 1)[1].split("\n}", 1)[0]
        self.assertIn('current_theme_ = theme;', theme)
        self.assertIn('#if !CONFIG_ORBIT_USB_SELFTEST\n    Settings settings("display", true);', theme)
        self.assertIn('settings.SetString("theme", theme->name());\n#endif', theme)

    def test_rle_cross_language_stride_and_long_run(self):
        program = r'''
#include "usb_selftest_packet.h"
#include <cstdio>
#include <vector>
int main() {
    std::vector<uint8_t> pixels(468 * 2 * 466, 0xAB);
    for (int y=0; y<466; ++y) for (int x=0; x<466; ++x) {
        auto offset=y*936+x*2; pixels[offset]=0; pixels[offset+1]=0;
    }
    pixels[0]=0; pixels[1]=0xF8;
    auto encoded=OrbitUsbSelfTest::Encode(pixels.data(),466,466,936);
    auto hash=OrbitUsbSelfTest::Checksum(encoded);
    fwrite(&hash,4,1,stdout); fwrite(encoded.data(),1,encoded.size(),stdout);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "test.cc"
            executable = Path(directory) / "test"
            source.write_text(program)
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(BOARD),
                            str(source), "-o", str(executable)], check=True)
            output = subprocess.check_output([str(executable)])
        self.assertEqual(capture.checksum(output[4:]), struct.unpack("<I", output[:4])[0])
        decoded = capture.decode_rle(output[4:], 466, 466)
        self.assertEqual(decoded, b"\0\xf8" + b"\0\0" * (466 * 466 - 1))
        from PIL import Image
        self.assertEqual(Image.frombytes("RGB", (466, 466), decoded, "raw", "BGR;16").getpixel((0, 0)),
                         (255, 0, 0))

    def test_invalid_packets_rejected(self):
        for payload in (b"", b"123", struct.pack("<HH", 0, 0), struct.pack("<HH", 1, 0)):
            with self.assertRaises(ValueError):
                capture.decode_rle(payload, 466, 466)
        with self.assertRaises(ValueError):
            capture.decode_rle(struct.pack("<HH", 4, 0), 2, 2)
        with self.assertRaises(ValueError):
            capture.validate_metadata({"caption": "arbitrary raw data"})

    def test_exact_fixture_manifest(self):
        source = (BOARD / "usb_selftest_main.cc").read_text()
        self.assertEqual(len(capture.CASES), 19)
        for name in capture.CASES:
            self.assertIn('Capture("' + name + '")', source)


if __name__ == "__main__":
    unittest.main()
