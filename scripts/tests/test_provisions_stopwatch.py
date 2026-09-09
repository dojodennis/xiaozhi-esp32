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
        self.assertEqual(config["target"], "esp32s3")
        builds = {item["name"]: item for item in config["builds"]}
        demo_name = f"{PROFILE}-schedule-demo"
        bench_name = f"{PROFILE}-schedule-bench"
        expected_symbols = {
            "m5stack-stopwatch": "CONFIG_BOARD_TYPE_M5STACK_STOPWATCH",
            PROFILE: PROFILE_SYMBOL,
            demo_name: PROFILE_SYMBOL,
            bench_name: PROFILE_SYMBOL,
        }
        self.assertEqual(set(builds), set(expected_symbols))
        self.assertEqual(len(config["builds"]), len(builds), "duplicate variant name")

        for name, expected_symbol in expected_symbols.items():
            with self.subTest(variant=name):
                options = builds[name]["sdkconfig_append"]
                self.assertEqual(
                    len(options), len(build._sdkconfig_assignments(options)),
                    "a variant must not redefine the same config key",
                )
                self.assertEqual(
                    build._resolve_board_config(
                        "m5stack/stopwatch",
                        config["target"],
                        builds[name]["sdkconfig_append"],
                        variant_name=name,
                    ),
                    expected_symbol,
                )

        generic = set(builds["m5stack-stopwatch"]["sdkconfig_append"])
        provisions = set(builds[PROFILE]["sdkconfig_append"])
        demo = set(builds[demo_name]["sdkconfig_append"])
        self.assertIn("CONFIG_USE_AFE_WAKE_WORD=y", generic)
        self.assertNotIn(f"{PROFILE_SYMBOL}=y", generic)
        self.assertNotIn("CONFIG_PROVISIONS_GATEWAY_REQUIRED=y", generic)
        self.assertNotIn("CONFIG_WAKE_WORD_DISABLED=y", generic)
        self.assertIn(f"{PROFILE_SYMBOL}=y", provisions)
        self.assertIn("CONFIG_WAKE_WORD_DISABLED=y", provisions)
        self.assertIn("CONFIG_PROVISIONS_GATEWAY_REQUIRED=y", provisions)
        self.assertIn("CONFIG_SPIRAM_MODE_OCT=y", provisions)
        self.assertNotIn("CONFIG_USE_AFE_WAKE_WORD=y", provisions)

        # The isolated fixture shares the Provisions hardware configuration,
        # but only its distinct variant opts out of capture and into the demo.
        demo_only = {
            "CONFIG_PROVISIONS_LOCAL_CAPTURE=n",
            "CONFIG_PROVISIONS_SCHEDULE_BENCH_DEMO=y",
        }
        self.assertEqual(demo - provisions, demo_only)
        self.assertEqual(provisions - demo, set())
        accepted = json.loads((BOARD_DIR / "bench_profile.json").read_text())["builds"][0]
        hardware_bench = json.loads((BOARD_DIR / "hardware_bench_profile.json").read_text())["builds"][0]
        self.assertEqual(hardware_bench, builds[bench_name])
        self.assertEqual(
            set(hardware_bench["sdkconfig_append"]) - set(accepted["sdkconfig_append"]),
            {"CONFIG_PROVISIONS_LOCAL_CAPTURE=y", "CONFIG_PROVISIONS_SCHEDULE_BENCH_DEMO=n",
             "CONFIG_PROVISIONS_SCHEDULE_HARDWARE_BENCH=y"},
        )
        self.assertEqual(set(accepted["sdkconfig_append"]) - set(hardware_bench["sdkconfig_append"]), set())
        for name in ("m5stack-stopwatch", PROFILE):
            with self.subTest(non_demo_variant=name):
                options = build._sdkconfig_assignments(
                    builds[name]["sdkconfig_append"]
                )
                self.assertNotEqual(
                    options.get("CONFIG_PROVISIONS_SCHEDULE_BENCH_DEMO"), "y"
                )
                self.assertNotEqual(options.get("CONFIG_PROVISIONS_LOCAL_CAPTURE"), "n")

    def test_provisions_behavior_is_thin_hold_to_talk(self):
        source = (BOARD_DIR / "m5stack_stopwatch.cc").read_text(encoding="utf-8")
        config = (BOARD_DIR / "config.h").read_text(encoding="utf-8")
        self.assertIn('"PROVISIONS_SIGNED_HARDWARE_IDENTITY=" BOARD_NAME', source)
        self.assertIn("button1_.OnPressDown", source)
        self.assertIn("StartListening", source)
        self.assertIn("button1_.OnPressUp", source)
        self.assertIn("StopListening", source)
        self.assertIn("hide_subtitle_ = true", source)
        self.assertIn("lv_label_set_text(reply_label_, content)", source)
        self.assertIn("No transcript text is retained", source)
        self.assertNotIn("last_reply_text_", source)
        reply_setter = source.split("void SetChatMessage", 1)[1].split(
            "void SetPowerSaveMode", 1
        )[0]
        self.assertNotIn("ProvisionsStopWatch::EllipsizeUtf8", reply_setter)
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

    def test_provisions_screen_is_branded_and_reply_capable(self):
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
        self.assertIn("lv_obj_set_size(brand_rule_, 68, 3)", source)
        self.assertIn('lv_label_set_text(brand_mark_label_, "P")', source)
        self.assertIn("lv_obj_set_parent(emoji_box_, hero_halo_)", source)
        self.assertIn("lv_obj_set_style_bg_opa(hero_halo_, LV_OPA_10", source)
        self.assertIn("lv_obj_set_style_border_opa(hero_halo_, LV_OPA_20", source)
        self.assertIn("lv_obj_set_style_bg_opa(status_bar_, LV_OPA_10", source)
        self.assertIn("lv_obj_set_style_bg_opa(hint_panel_, LV_OPA_10", source)
        self.assertIn("lv_obj_set_style_bg_color(talk_button_dot_", source)
        self.assertIn("kColorTalkButton = 0xF2C84B", source)
        self.assertIn('return {"READY", "HOLD TO TALK"', source)
        self.assertIn('return {"LISTENING", "RELEASE TO SEND"', source)
        self.assertIn('return {"CHECKING", "ONE MOMENT"', source)
        self.assertIn('return {"ADDED", "DRAFT - NOT SENT"', source)
        self.assertIn('return {"OFFLINE", "TRY AGAIN"', source)
        self.assertIn("MATERIAL_SYMBOLS_CHECK_CIRCLE", source)
        self.assertIn("MATERIAL_SYMBOLS_CLOUD_OFF", source)
        self.assertIn("kRoundTopBarWidth = 260", source)
        self.assertIn("kRoundTopBarOffset = 46", source)
        self.assertIn("kRoundContentWidth = 330", source)
        self.assertIn("kRoundBrandTopOffset = 78", source)
        self.assertIn("kRoundTitleTopOffset = 103", source)
        self.assertIn("kRoundRuleTopOffset = 146", source)
        self.assertIn("kRoundHeroSize = 124", source)
        self.assertIn("kRoundHeroOffset = -10", source)
        self.assertIn("kRoundStatusWidth = 320", source)
        self.assertIn("kRoundStatusOffset = 82", source)
        self.assertIn("kRoundHintOffset = 132", source)
        self.assertIn("lv_color_hex(0x000000)", provisions_ui)
        self.assertIn('std::strcmp(notification, "Added")', source)
        self.assertIn('std::strcmp(notification, "Undone")', source)
        recorded_mapping = source.split(
            'if (std::strcmp(notification, "Recorded") == 0)', 1
        )[1].split("}", 1)[0]
        self.assertIn("VisualState::kRecorded", recorded_mapping)
        self.assertIn(
            'return {"RECORDED", "FROM RECORDS", MATERIAL_SYMBOLS_INFO, kColorAmber}',
            source,
        )
        self.assertIn(
            'return {"RECORDED", "ON ORBIT", MATERIAL_SYMBOLS_CHECK_CIRCLE,',
            source,
        )
        local_receipt = source.split(
            "void ShowLocalCaptureReceipt", 1
        )[1].split("private:", 1)[0]
        self.assertIn('ShowReceipt("RECORDED", VisualState::kLocalRecorded', local_receipt)
        self.assertIn(
            'return {"DRAFT", "NOT SENT", MATERIAL_SYMBOLS_INFO, kColorAmber}',
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
        self.assertIn('lv_label_set_text(reply_header_label_, "REPLY")', source)
        self.assertIn("lv_label_set_long_mode(reply_label_, LV_LABEL_LONG_WRAP)", source)
        self.assertIn('.name = "stopwatch_reply_scroll"', source)
        self.assertIn("lv_obj_get_scroll_bottom", source)
        self.assertIn("lv_obj_scroll_to_y", source)
        self.assertIn("void RestartReplyFromTop()", source)
        self.assertIn("lv_obj_get_scroll_y(self->reply_panel_) > 0", source)
        self.assertIn("self->CancelReplyScroll();", source)
        self.assertIn("reply_generation_.load() != generation", source)
        self.assertIn("kReplyPlaybackMaximumMs = 35 * 1000", source)
        self.assertIn("kReplyHoldAfterSpeechMs = 12 * 1000", source)
        self.assertIn('lv_label_set_text(reply_label_, "")', source)
        self.assertIn("resting_state_.store(VisualState::kUnavailable)", source)
        self.assertIn("if (!ScheduleVisualReset(kReplyPlaybackMaximumMs))", source)
        self.assertIn("if (!ScheduleVisualReset(kReplyHoldAfterSpeechMs))", source)

    def test_provisions_stopwatch_clears_amoled_to_black_without_changing_defaults(self):
        source = (BOARD_DIR / "m5stack_stopwatch.cc").read_text(encoding="utf-8")
        display_header = (ROOT / "main/display/lcd_display.h").read_text(encoding="utf-8")
        display_source = (ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")

        self.assertIn("kStopwatchInitialClearColor = 0x0000", source)
        self.assertIn("kStopwatchInitialClearColor = 0xFFFF", source)
        self.assertIn("swap_xy, kStopwatchInitialClearColor", source)
        self.assertIn("uint16_t initial_clear_color = 0xFFFF", display_header)
        self.assertIn(
            "std::vector<uint16_t> buffer(width_, initial_clear_color)",
            display_source,
        )

    def test_provisions_amoled_idles_to_black_and_restores_all_chrome(self):
        source = (BOARD_DIR / "m5stack_stopwatch.cc").read_text(encoding="utf-8")
        power_save = source.split(
            "void SetPowerSaveMode(bool on) override", 1
        )[1].split("void SetStatus", 1)[0]

        for object_name in (
            "top_bar_",
            "brand_label_",
            "title_label_",
            "brand_rule_",
            "hero_halo_",
            "status_bar_",
            "hint_panel_",
            "reply_header_label_",
            "reply_panel_",
            "orbit_layer_",
            "alarm_layer_",
        ):
            self.assertIn(object_name, power_save)
        self.assertIn("lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN)", power_save)
        self.assertIn("power_save_active_.store(on)", power_save)
        self.assertIn("SetReplyLayoutLocked(reply_visible_.load())", power_save)
        reply_layout = source.split("void SetReplyLayoutLocked", 1)[1].split(
            "void CancelReplyScroll", 1
        )[0]
        self.assertIn("show_alarm = display_awake && timer_alarm_active_", reply_layout)
        self.assertIn("show_reply = display_awake && !show_alarm && visible", reply_layout)
        self.assertIn("ShouldShowOrbitLocked()", reply_layout)
        self.assertIn("SetVisible(alarm_layer_, show_alarm)", reply_layout)
        self.assertNotIn("LV_ANIM_REPEAT_INFINITE", source)

    def test_provisions_blue_button_silences_alarm_or_toggles_high_and_max_volume(self):
        source = (BOARD_DIR / "m5stack_stopwatch.cc").read_text(encoding="utf-8")
        method = re.search(
            r"^    void InitializeButtons\(\) \{.*?^    \}", source, re.MULTILINE | re.DOTALL
        )
        self.assertIsNotNone(method, "missing board button initializer")
        compiler = shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler, "host C++ preprocessor is required")

        # Select the actual normal branch with the compiler. Splitting at the
        # first #else accidentally reads the nested synthetic demo instead.
        def preprocess(demo):
            result = subprocess.run(
                [compiler, "-E", "-P", "-x", "c++",
                 "-DCONFIG_PROVISIONS_GATEWAY_REQUIRED=1",
                 f"-DCONFIG_PROVISIONS_SCHEDULE_BENCH_DEMO={int(demo)}", "-"],
                input=method.group(0), capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            return result.stdout

        provisions_buttons = preprocess(False)

        self.assertIn("button2_.OnClick", provisions_buttons)
        self.assertIn("Application::GetInstance().Schedule", provisions_buttons)
        self.assertRegex(
            provisions_buttons,
            r"if\s*\(display_->SilenceTimerAlarm\(\)\)\s*\{\s*return;\s*\}",
        )
        self.assertIn("kDefaultOutputVolume", provisions_buttons)
        self.assertIn("kMaximumOutputVolume", provisions_buttons)
        self.assertIn(
            "codec->SetOutputVolume(maximum ? kDefaultOutputVolume : kMaximumOutputVolume)",
            provisions_buttons,
        )
        self.assertLess(
            provisions_buttons.index("display_->SilenceTimerAlarm()"),
            provisions_buttons.index("codec->SetOutputVolume("),
        )
        self.assertIn("StartListening", provisions_buttons)
        self.assertNotIn("AdvanceScheduleDemo", provisions_buttons)
        self.assertNotIn("volume = 0", provisions_buttons)
        self.assertNotIn("SetOutputVolume(0)", provisions_buttons)
        self.assertNotIn("ShowNotification", provisions_buttons)

        demo_buttons = preprocess(True)
        self.assertIn("AdvanceScheduleDemo", demo_buttons)
        self.assertIn("display_->SilenceTimerAlarm()", demo_buttons)
        self.assertNotIn("StartListening", demo_buttons)
        self.assertNotIn("SetOutputVolume", demo_buttons)

    def test_local_capture_haptic_is_bounded_and_yields_to_timer_alarm(self):
        source = (BOARD_DIR / "m5stack_stopwatch.cc").read_text(encoding="utf-8")
        board = source.split("class M5StackStopwatchBoard", 1)[1]
        pulse = board.split("void PulseLocalCaptureHaptic", 1)[1].split("#endif", 1)[0]

        self.assertIn("duration_ms == 0", pulse)
        self.assertIn("display_->HasTimerAlarm()", pulse)
        self.assertIn("ioe_.digitalWriteWithRes(IOE_PIN_MOTOR, HIGH", pulse)
        self.assertIn("static_cast<int64_t>(duration_ms) * 1000", pulse)
        self.assertIn('name = "stopwatch_capture_haptic"', board)
        self.assertIn("ioe_.digitalWriteWithRes(IOE_PIN_MOTOR, LOW", board)
        self.assertIn("Application::GetInstance().Schedule", board)
        self.assertIn("capture_haptic_pulse_.IsExpired", board)

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
            "hero": (
                constant("kRoundHeroSize"),
                constant("kRoundHeroSize"),
                center,
                center + constant("kRoundHeroOffset"),
            ),
            "status": (
                constant("kRoundStatusWidth"),
                constant("kRoundStatusHeight"),
                center,
                center + constant("kRoundStatusOffset"),
            ),
            "hint": (
                constant("kRoundHintWidth"),
                constant("kRoundHintHeight"),
                center,
                center + constant("kRoundHintOffset"),
            ),
            "reply header": (
                constant("kReplyHeaderWidth"),
                22,
                center,
                constant("kReplyHeaderTopOffset") + 11,
            ),
        }

        for name, (width, height, x, y) in rectangles.items():
            with self.subTest(name=name):
                farthest_corner = math.hypot(
                    abs(x - center) + width / 2,
                    abs(y - center) + height / 2,
                )
                self.assertLessEqual(farthest_corner, safe_radius)

        # The reply surface is an opaque-black scroll viewport clipped by the
        # round panel, not visible rectangular chrome. Keep it within the
        # physical canvas while the text viewport intentionally uses the width.
        self.assertLessEqual(constant("kReplyPanelWidth"), 466)
        self.assertLessEqual(constant("kReplyPanelHeight"), 466)
        self.assertIn("lv_obj_set_style_border_width(reply_panel_, 0", source)

        hero_bottom = (
            center
            + constant("kRoundHeroOffset")
            + constant("kRoundHeroSize") / 2
        )
        status_top = (
            center
            + constant("kRoundStatusOffset")
            - constant("kRoundStatusHeight") / 2
        )
        status_bottom = (
            center
            + constant("kRoundStatusOffset")
            + constant("kRoundStatusHeight") / 2
        )
        hint_top = (
            center
            + constant("kRoundHintOffset")
            - constant("kRoundHintHeight") / 2
        )
        self.assertLess(hero_bottom, status_top)
        self.assertLess(status_bottom, hint_top)

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

    @unittest.skipUnless(shutil.which("c++"), "host C++ compiler is unavailable")
    def test_tts_text_policy_rejects_unsafe_unicode_and_malformed_utf8(self):
        test_source = textwrap.dedent(
            r"""
            #include "provisions_tts_text.h"
            #include <cassert>
            #include <string>

            int main() {
                using ProvisionsTtsText::IsValid;
                assert(IsValid("Soy sauce is in your draft."));
                assert(IsValid(u8"Crème fraîche, 醤油, and ✅"));
                assert(!IsValid(""));
                assert(IsValid(std::string(500, 'a')));
                assert(!IsValid(std::string(501, 'a')));

                std::string maximum_bytes;
                for (int i = 0; i < 500; ++i) maximum_bytes += u8"😀";
                assert(maximum_bytes.size() == 2000);
                assert(IsValid(maximum_bytes));
                assert(!IsValid(maximum_bytes + "a"));

                assert(!IsValid(std::string("safe\0hidden", 11)));
                assert(!IsValid(std::string("\x80", 1)));
                assert(!IsValid(std::string("\xc0\xaf", 2)));
                assert(!IsValid(std::string("\xe2\x82", 2)));
                assert(!IsValid(std::string("\xe2\x28\xa1", 3)));
                assert(!IsValid(std::string("\xed\xa0\x80", 3)));
                assert(!IsValid(std::string("\xf0\x80\x80\x80", 4)));
                assert(!IsValid(std::string("\xf4\x90\x80\x80", 4)));
                assert(!IsValid(std::string("\xf5\x80\x80\x80", 4)));

                assert(!IsValid("line\nbreak"));
                assert(!IsValid(std::string("\xc2\x80", 2)));
                assert(!IsValid(std::string("\xc2\xad", 2)));
                assert(!IsValid(std::string("\xe2\x80\xae", 3)));
                assert(!IsValid(std::string("\xee\x80\x80", 3)));
                assert(!IsValid(std::string("\xef\xb7\x90", 3)));
                assert(!IsValid(std::string("\xf3\xbf\xbf\xbe", 4)));
                return 0;
            }
            """
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            source = temporary / "tts_text_policy_test.cc"
            executable = temporary / "tts_text_policy_test"
            source.write_text(test_source, encoding="utf-8")
            subprocess.run(
                [
                    shutil.which("c++"),
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "main"),
                    str(source),
                    "-o",
                    str(executable),
                ],
                check=True,
                cwd=ROOT,
            )
            subprocess.run([str(executable)], check=True, cwd=ROOT)

    @unittest.skipUnless(shutil.which("c++"), "host C++ compiler is unavailable")
    def test_tts_turn_deduplicates_orders_and_serializes_invalidation(self):
        test_source = textwrap.dedent(
            r"""
            #include "provisions_tts_turn.h"
            #include <atomic>
            #include <cassert>
            #include <thread>

            int main() {
                using Outcome = ProvisionsTtsTurn::Outcome;
                using Phase = ProvisionsTtsTurn::Phase;

                ProvisionsTtsTurn turn;
                assert(turn.Sentence().outcome == Outcome::kInvalidOrder);
                assert(turn.Stop().outcome == Outcome::kDuplicate);

                const auto first = turn.Start();
                assert(first.outcome == Outcome::kAccepted);
                assert(first.token != ProvisionsTtsTurn::kInvalidToken);
                assert(turn.Start().outcome == Outcome::kDuplicate);
                assert(turn.Sentence().outcome == Outcome::kAccepted);
                assert(turn.Sentence().outcome == Outcome::kDuplicate);
                assert(turn.Stop().outcome == Outcome::kAccepted);
                assert(turn.Stop().outcome == Outcome::kDuplicate);
                assert(turn.phase() == Phase::kIdle);

                const auto second = turn.Start();
                assert(second.outcome == Outcome::kAccepted);
                assert(second.token != first.token);
                bool stale_ran = false;
                assert(!turn.WithCurrent(first.token, [&]() { stale_ran = true; }));
                assert(!stale_ran);
                bool identical_reply_ran = false;
                assert(turn.WithCurrent(second.token, [&]() { identical_reply_ran = true; }));
                assert(identical_reply_ran);

                ProvisionsTtsTurn concurrent;
                const auto active = concurrent.Start();
                std::atomic<bool> action_started{false};
                std::atomic<bool> allow_action_finish{false};
                std::atomic<bool> invalidation_started{false};
                std::atomic<bool> invalidation_returned{false};
                std::thread action([&]() {
                    assert(concurrent.WithCurrent(active.token, [&]() {
                        action_started.store(true, std::memory_order_release);
                        while (!allow_action_finish.load(std::memory_order_acquire)) {
                            std::this_thread::yield();
                        }
                    }));
                });
                while (!action_started.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                std::thread invalidator([&]() {
                    invalidation_started.store(true, std::memory_order_release);
                    concurrent.Invalidate();
                    invalidation_returned.store(true, std::memory_order_release);
                });
                while (!invalidation_started.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (int i = 0; i < 1000; ++i) {
                    assert(!invalidation_returned.load(std::memory_order_acquire));
                    std::this_thread::yield();
                }
                allow_action_finish.store(true, std::memory_order_release);
                action.join();
                invalidator.join();
                assert(invalidation_returned.load(std::memory_order_acquire));
                bool post_invalidation_ran = false;
                assert(!concurrent.WithCurrent(active.token, [&]() {
                    post_invalidation_ran = true;
                }));
                assert(!post_invalidation_ran);
                return 0;
            }
            """
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            source = temporary / "tts_turn_test.cc"
            executable = temporary / "tts_turn_test"
            source.write_text(test_source, encoding="utf-8")
            subprocess.run(
                [
                    shutil.which("c++"),
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pthread",
                    "-I",
                    str(ROOT / "main"),
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
