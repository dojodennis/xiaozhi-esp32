import importlib.util
import json
import math
import re
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
        self.assertIn("hide_subtitle_ = true", source)
        self.assertIn("Spoken detail stays in audio", source)
        self.assertNotIn("ProvisionsStopWatch::EllipsizeUtf8", source)
        self.assertIn("kDisplayIdleTimeoutUs = 45LL * 1000 * 1000", source)
        self.assertIn('.name = "stopwatch_display_idle"', source)
        self.assertIn("esp_timer_start_once", source)
        self.assertIn("display_idle_deadline_us_.store(deadline)", source)
        self.assertIn("stop_result != ESP_ERR_INVALID_STATE", source)
        self.assertNotIn("esp_timer_is_active", source)
        self.assertIn("Application::GetInstance().Schedule", source)
        self.assertIn("ResetDisplayIdleTimer()", source)
        self.assertIn("GetBacklight()->SetBrightness(5)", source)
        self.assertIn("kDefaultOutputVolume = 90", source)
        self.assertIn("class ProvisionsStopwatchAudioCodec", source)
        self.assertIn("Es8311AudioCodec::Start();", source)
        self.assertIn(
            "output_volume() < kDefaultOutputVolume || output_volume() > 100",
            source,
        )
        self.assertIn(
            "SetOutputVolumeForSession(kDefaultOutputVolume)", source
        )
        self.assertIn("it never writes NVS", source)
        self.assertNotIn("Settings settings", source)
        codec_source = (
            ROOT / "main/audio/codecs/es8311_audio_codec.cc"
        ).read_text()
        transient_setter = codec_source.split(
            "void Es8311AudioCodec::SetOutputVolumeForSession", 1
        )[1].split("void Es8311AudioCodec::EnableInput", 1)[0]
        self.assertIn("output_volume_ = volume", transient_setter)
        self.assertIn("esp_codec_dev_set_out_vol", transient_setter)
        self.assertNotIn("Settings", transient_setter)
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

    def test_provisions_screen_is_branded_and_state_only(self):
        source = (BOARD_DIR / "m5stack_stopwatch.cc").read_text(encoding="utf-8")
        provisions_ui = source.split("#if CONFIG_PROVISIONS_GATEWAY_REQUIRED", 2)[2]

        self.assertIn('lv_label_set_text(brand_label_, "PROVISIONS")', source)
        self.assertIn('lv_label_set_text(title_label_, "KITCHEN HELPER")', source)
        self.assertIn(
            "lv_obj_set_style_text_font(brand_label_, &font_noto_sans_basic_16_4",
            source,
        )
        self.assertIn(
            "lv_obj_set_style_text_font(title_label_, &font_noto_sans_basic_30_4",
            source,
        )
        self.assertIn("lv_obj_set_style_text_letter_space(brand_label_, 4", source)
        self.assertIn("lv_obj_set_size(brand_rule_, 52, 2)", source)
        self.assertIn('return {"Ready", "Hold yellow button to talk"', source)
        self.assertIn('return {"Listening", "Release when finished"', source)
        self.assertIn('return {"Working", "Checking Provisions"', source)
        self.assertIn('return {"Added to draft", "Draft only - not sent"', source)
        self.assertIn('return {"Unavailable", "Please try again"', source)
        self.assertIn("MATERIAL_SYMBOLS_CHECK_CIRCLE", source)
        self.assertIn("MATERIAL_SYMBOLS_CLOUD_OFF", source)
        self.assertIn("kRoundTopBarWidth = 260", source)
        self.assertIn("kRoundTopBarOffset = 46", source)
        self.assertIn("kRoundContentWidth = 330", source)
        self.assertIn("kRoundBrandTopOffset = 84", source)
        self.assertIn("kRoundTitleTopOffset = 108", source)
        self.assertIn("kRoundRuleTopOffset = 151", source)
        self.assertIn("kRoundIconOffset = -2", source)
        self.assertIn("kRoundStatusOffset = 76", source)
        self.assertIn("kRoundHintOffset = 123", source)
        self.assertIn("lv_color_hex(0x000000)", provisions_ui)
        self.assertIn('std::strcmp(notification, "Added")', source)
        self.assertIn('std::strcmp(notification, "Undone")', source)
        recorded_mapping = source.split(
            'if (std::strcmp(notification, "Recorded") == 0)', 1
        )[1].split("}", 1)[0]
        self.assertIn("VisualState::kRecorded", recorded_mapping)
        self.assertIn(
            'return {"Recorded", "Not physically verified", MATERIAL_SYMBOLS_INFO, kColorAmber}',
            source,
        )
        self.assertIn(
            'return {"Draft only", "Unsent - not submitted", MATERIAL_SYMBOLS_INFO, kColorAmber}',
            source,
        )
        draft_mapping = source.split(
            'if (std::strcmp(notification, "Draft only") == 0)', 1
        )[1].split("}", 1)[0]
        self.assertIn("VisualState::kDraft", draft_mapping)
        self.assertIn('std::strcmp(status, Lang::Strings::LISTENING)', source)
        self.assertIn("IsClockStatus(status)", source)
        self.assertIn('.name = "stopwatch_visual_reset"', source)
        self.assertIn("receipt_visible_.store(true)", source)
        self.assertIn("receipt_visible_.load() && state != VisualState::kListening", source)
        self.assertIn("ApplyVisualStateLocked(resting_state_.load())", source)
        self.assertNotIn("ApplyVisualState(self->resting_state_.load())", source)
        self.assertIn("Lang::Strings::CHECKING_NEW_VERSION", source)
        self.assertIn("Lang::Strings::LOADING_PROTOCOL", source)
        self.assertIn("lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN)", provisions_ui)

    def test_provisions_blue_button_toggles_only_high_and_max_volume(self):
        source = (BOARD_DIR / "m5stack_stopwatch.cc").read_text(encoding="utf-8")
        provisions_buttons = source.split("void InitializeButtons()", 1)[1].split("#else", 1)[0]

        self.assertIn("button2_.OnClick", provisions_buttons)
        self.assertIn("Application::GetInstance().Schedule", provisions_buttons)
        self.assertIn("kDefaultOutputVolume", provisions_buttons)
        self.assertIn("kMaximumOutputVolume", provisions_buttons)
        self.assertNotIn("volume = 0", provisions_buttons)
        self.assertNotIn("SetOutputVolume(0)", provisions_buttons)
        self.assertNotIn("ShowNotification", provisions_buttons)

    def test_receipt_reset_rejects_stale_timer_callbacks(self):
        source = (BOARD_DIR / "m5stack_stopwatch.cc").read_text(encoding="utf-8")

        self.assertIn("visual_reset_deadline_us_", source)
        self.assertIn("visual_reset_deadline_us_.load() != deadline", source)
        self.assertIn("visual_reset_deadline_us_.store(0)", source)
        self.assertNotIn("LvglDisplay::ShowNotification(title, duration_ms)", source)

    def test_provisions_screen_geometry_stays_inside_round_safe_area(self):
        source = (BOARD_DIR / "m5stack_stopwatch.cc").read_text(encoding="utf-8")

        def constant(name):
            match = re.search(rf"constexpr int {name} = (-?\d+);", source)
            self.assertIsNotNone(match, f"missing {name}")
            return int(match.group(1))

        center = 233
        safe_radius = 229
        content_width = constant("kRoundContentWidth")
        rectangles = {
            "top bar": (
                constant("kRoundTopBarWidth"),
                30,
                center,
                constant("kRoundTopBarOffset") + 15,
            ),
            "brand": (
                constant("kRoundBrandWidth"),
                22,
                center,
                constant("kRoundBrandTopOffset") + 11,
            ),
            "title": (
                content_width,
                38,
                center,
                constant("kRoundTitleTopOffset") + 19,
            ),
            "icon": (
                92,
                92,
                center,
                center + constant("kRoundIconOffset"),
            ),
            "status": (
                content_width,
                48,
                center,
                center + constant("kRoundStatusOffset"),
            ),
            "hint": (
                content_width,
                22,
                center,
                center + constant("kRoundHintOffset"),
            ),
        }

        for name, (width, height, x, y) in rectangles.items():
            with self.subTest(name=name):
                farthest_corner = math.hypot(
                    abs(x - center) + width / 2,
                    abs(y - center) + height / 2,
                )
                self.assertLessEqual(farthest_corner, safe_radius)

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
