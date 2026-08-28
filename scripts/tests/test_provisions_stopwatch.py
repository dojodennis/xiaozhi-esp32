import importlib.util
import json
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD_DIR = ROOT / "main/boards/m5stack/stopwatch"
PROFILE = "provisions-kitchen-helper-stopwatch"
PROFILE_SYMBOL = "CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_STOPWATCH"
SPEC = importlib.util.spec_from_file_location("provisions_build", ROOT / "scripts/build.py")
build = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(build)


class ProvisionsStopWatchProfileTests(unittest.TestCase):
    def test_generic_and_provisions_builds_are_distinct(self):
        config = json.loads((BOARD_DIR / "config.json").read_text(encoding="utf-8"))
        self.assertEqual(config["manufacturer"], "m5stack")
        self.assertEqual(config["type"], "m5stack-stopwatch")
        builds = {item["name"]: item for item in config["builds"]}
        self.assertEqual(set(builds), {"m5stack-stopwatch", PROFILE})

        generic = set(builds["m5stack-stopwatch"]["sdkconfig_append"])
        provisions = set(builds[PROFILE]["sdkconfig_append"])
        self.assertIn("CONFIG_USE_AFE_WAKE_WORD=y", generic)
        self.assertNotIn(f"{PROFILE_SYMBOL}=y", generic)
        self.assertIn(f"{PROFILE_SYMBOL}=y", provisions)
        self.assertIn("CONFIG_WAKE_WORD_DISABLED=y", provisions)
        self.assertIn("CONFIG_PROVISIONS_GATEWAY_REQUIRED=y", provisions)
        self.assertIn("CONFIG_SPIRAM_MODE_OCT=y", provisions)
        self.assertNotIn("CONFIG_USE_AFE_WAKE_WORD=y", provisions)
        self.assertEqual(
            build._resolve_board_config(
                "m5stack/stopwatch",
                "esp32s3",
                builds[PROFILE]["sdkconfig_append"],
                variant_name=PROFILE,
            ),
            PROFILE_SYMBOL,
        )

    def test_provisions_behavior_is_thin_hold_to_talk(self):
        source = (BOARD_DIR / "m5stack_stopwatch.cc").read_text(encoding="utf-8")
        config = (BOARD_DIR / "config.h").read_text(encoding="utf-8")
        self.assertIn('"PROVISIONS_SIGNED_HARDWARE_IDENTITY=" BOARD_NAME', source)
        self.assertIn("button1_.OnPressDown", source)
        self.assertIn("StartListening", source)
        self.assertIn("button1_.OnPressUp", source)
        self.assertIn("StopListening", source)
        self.assertIn("SetHideSubtitle(true)", source)
        self.assertIn("kMaximumResultBytes = 48", source)
        self.assertIn("ProvisionsStopWatch::EllipsizeUtf8", source)
        self.assertIn('std::strcmp(role, "assistant")', source)
        self.assertIn("kDisplayIdleTimeoutUs = 45LL * 1000 * 1000", source)
        self.assertIn('.name = "stopwatch_display_idle"', source)
        self.assertIn("esp_timer_start_once", source)
        self.assertIn("display_idle_deadline_us_.store(deadline)", source)
        self.assertIn("stop_result != ESP_ERR_INVALID_STATE", source)
        self.assertNotIn("esp_timer_is_active", source)
        self.assertIn("Application::GetInstance().Schedule", source)
        self.assertIn("ResetDisplayIdleTimer()", source)
        self.assertIn("GetBacklight()->SetBrightness(5)", source)
        power_level = source.split(
            "void SetPowerSaveLevel(PowerSaveLevel level) override", 1
        )[1].split("#endif", 1)[0]
        self.assertIn("ResetDisplayIdleTimer();", power_level)
        self.assertNotIn("level != PowerSaveLevel::LOW_POWER", power_level)
        self.assertNotIn("PowerSaveTimer", source)
        self.assertNotIn("CanEnterSleepMode", source)
        self.assertIn("pmic_.getPowerSource", source)
        self.assertIn("M5PM1_PWR_SRC_BAT", source)
        self.assertIn('#define PROVISIONS_HARDWARE_PROFILE "stopwatch-client"', config)
        self.assertIn("#define PROVISIONS_NOMINAL_BATTERY_MAH 450", config)

    @unittest.skipUnless(shutil.which("c++"), "host C++ compiler is unavailable")
    def test_display_ellipsis_keeps_utf8_code_points_intact(self):
        test_source = textwrap.dedent(
            r"""
            #include "utf8_ellipsis.h"
            #include <cassert>
            #include <string>

            int main() {
                using ProvisionsStopWatch::EllipsizeUtf8;
                assert(EllipsizeUtf8("short", 48) == "short");
                assert(EllipsizeUtf8("long", 3) == "...");
                assert(EllipsizeUtf8("long", 2) == "..");

                const std::string accented = std::string(44, 'a') + "é-supplier";
                assert(EllipsizeUtf8(accented, 48) == std::string(44, 'a') + "...");

                const std::string emoji = std::string(44, 'b') + "✅-added";
                assert(EllipsizeUtf8(emoji, 48) == std::string(44, 'b') + "...");
            }
            """
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            source = temporary / "stopwatch_utf8_test.cc"
            executable = temporary / "stopwatch_utf8_test"
            source.write_text(test_source, encoding="utf-8")
            subprocess.run(
                [
                    shutil.which("c++"),
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(BOARD_DIR),
                    str(source),
                    "-o",
                    str(executable),
                ],
                check=True,
                cwd=ROOT,
            )
            subprocess.run([str(executable)], check=True, cwd=ROOT)

    def test_secure_profile_keeps_octal_psram_and_shared_pilot_safety(self):
        config = json.loads(
            (BOARD_DIR / "pilot_profile.json").read_text(encoding="utf-8")
        )
        self.assertEqual(config["type"], "m5stack-stopwatch")
        self.assertEqual(len(config["builds"]), 1)
        profile = config["builds"][0]
        self.assertEqual(profile["name"], PROFILE)
        options = set(profile["sdkconfig_append"])
        required = {
            f"{PROFILE_SYMBOL}=y",
            "CONFIG_SPIRAM_MODE_OCT=y",
            "CONFIG_PROVISIONS_GATEWAY_REQUIRED=y",
            "CONFIG_WAKE_WORD_DISABLED=y",
            'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/provisions/16m.csv"',
            "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y",
            "CONFIG_SECURE_BOOT=y",
            "CONFIG_SECURE_BOOT_V2_ENABLED=y",
            "CONFIG_SECURE_FLASH_ENC_ENABLED=y",
            "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y",
            "CONFIG_NVS_ENCRYPTION=y",
        }
        self.assertTrue(required.issubset(options))
        self.assertNotIn("CONFIG_SPIRAM_MODE_QUAD=y", options)

    def test_cmake_and_kconfig_bind_exact_identity(self):
        cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text(encoding="utf-8")
        policy = (ROOT / "main/provisions_endpoint_policy.cc").read_text(
            encoding="utf-8"
        )
        self.assertIn("CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_STOPWATCH", cmake)
        self.assertIn('set(BOARD_DIR "m5stack/stopwatch")', cmake)
        self.assertIn(f'set(PROVISIONS_STOPWATCH_BOARD_NAME "{PROFILE}")', cmake)
        self.assertIn("NOT BOARD_NAME STREQUAL PROVISIONS_STOPWATCH_BOARD_NAME", cmake)
        self.assertIn("config BOARD_TYPE_M5STACK_PROVISIONS_STOPWATCH", kconfig)
        self.assertIn(f'kSelectedHardwareIdentity = "{PROFILE}"', policy)


class ProvisionsStopWatchEndpointPolicyTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "host C++ compiler is unavailable")
    def test_stopwatch_accepts_only_its_own_firmware_path(self):
        test_source = textwrap.dedent(
            r"""
            #include "provisions_endpoint_policy.h"
            #include <cassert>
            #include <string>

            int main() {
                using namespace ProvisionsEndpointPolicy;
                const std::string hash(64, 'a');
                const std::string prefix =
                    "https://app.provisions-app.com/kitchen-helper/preview/v1/firmware/";
                assert(IsAllowedFirmwareUrl(
                    prefix + "provisions-kitchen-helper-stopwatch/1.0.0/" +
                    hash + ".bin"));
                assert(!IsAllowedFirmwareUrl(
                    prefix + "provisions-kitchen-helper-core-s3-lite/1.0.0/" +
                    hash + ".bin"));
                assert(!IsAllowedFirmwareUrl(
                    prefix + "provisions-kitchen-helper-core-s3/1.0.0/" +
                    hash + ".bin"));
            }
            """
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            source = temporary / "stopwatch_policy_test.cc"
            executable = temporary / "stopwatch_policy_test"
            source.write_text(test_source, encoding="utf-8")
            command = [
                shutil.which("c++"),
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                f'-DBOARD_NAME="{PROFILE}"',
                f"-D{PROFILE_SYMBOL}=1",
                "-DCONFIG_SPIRAM_MODE_OCT=1",
                '-DCONFIG_PROVISIONS_PREVIEW_HOST="app.provisions-app.com"',
                '-DCONFIG_PROVISIONS_PREVIEW_PATH_PREFIX="/kitchen-helper/preview/v1/"',
                '-DCONFIG_PROVISIONS_PREVIEW_WEBSOCKET_URL="wss://app.provisions-app.com/kitchen-helper/preview/v1/device"',
                '-DCONFIG_OTA_URL="https://app.provisions-app.com/kitchen-helper/preview/v1/bootstrap"',
                "-I",
                str(ROOT / "main"),
                str(ROOT / "main/provisions_endpoint_policy.cc"),
                str(source),
                "-o",
                str(executable),
            ]
            subprocess.run(command, check=True, cwd=ROOT)
            subprocess.run([str(executable)], check=True, cwd=ROOT)


if __name__ == "__main__":
    unittest.main()
