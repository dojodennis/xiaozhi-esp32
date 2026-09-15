import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD_DIR = ROOT / "main/boards/m5stack/stopwatch"
CJSON_DIR = ROOT / "managed_components/espressif__cjson/cJSON"
PM1_HEADER = ROOT / "managed_components/m5stack__m5pm1/src/M5PM1.h"

# Must stay a subset of the gateway's _HARDWARE_KEYS (protocol.py), which
# rejects the whole hello on an unknown key.
HELLO_HARDWARE_KEYS = {
    "touch", "mic_ready_ms", "pm1_wake",
    "reset", "crash_kind", "crash_bt", "crash_why", "crash_age",
}
TEXT_RE = re.compile(r"^[A-Za-z0-9_.-]{1,40}$")
INT_RANGES = {"mic_ready_ms": (0, 60_000), "pm1_wake": (0, 255), "crash_age": (0, 10_000)}
DIAGNOSTIC_OPTIONS = {
    "CONFIG_ESP_TASK_WDT_PANIC=y",
    "CONFIG_ESP_TASK_WDT_TIMEOUT_S=60",
    "CONFIG_HEAP_POISONING_LIGHT=y",
    "CONFIG_COMPILER_STACK_CHECK_MODE_NORM=y",
}


def _function_body(source, signature):
    start = source.index(signature)
    open_brace = source.index("{", start)
    depth = 0
    for index in range(open_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[open_brace:index + 1]
    raise AssertionError(f"unterminated body for {signature}")


class ProvisionsCrashRecordHelperTests(unittest.TestCase):
    @unittest.skipUnless(
        shutil.which("c++") and shutil.which("cc") and CJSON_DIR.is_dir(),
        "host C/C++ compilers or vendored cJSON are unavailable",
    )
    def test_helpers_crc_names_backtrace_why_age_and_hello_values(self):
        program = textwrap.dedent(
            r'''
            #include "provisions_crash_record.h"

            #include <cassert>
            #include <cstdio>
            #include <cstdlib>
            #include <cstring>
            #include <map>
            #include <string>

            using namespace provisions::crash;

            static bool AllowedText(const char* text) {
                const size_t length = std::strlen(text);
                if (length == 0 || length > kTextMax) return false;
                for (size_t i = 0; i < length; ++i) {
                    if (!WhyCharAllowed(text[i])) return false;
                }
                return true;
            }

            static Record Sample(const char* why) {
                Record record{};
                record.magic = kMagic;
                record.kind = kKindAbort;
                record.cause = 29;
                record.bt[0] = 0x4201a2b3u;
                record.bt[1] = 0x4200f00du;
                record.bt[2] = 0x42011234u;
                record.bt[3] = 0x4037abcdu;
                std::strncpy(record.why, why, kWhyBytes - 1);
                record.crc = RecordCrc(record);
                return record;
            }

            static void PrintFacts(const char* label, int reset, const Record* last, uint32_t age) {
                cJSON* hardware = cJSON_CreateObject();
                AppendFacts(hardware, reset, last, age);
                char* text = cJSON_PrintUnformatted(hardware);
                std::printf("FACTS %s %s\n", label, text);
                cJSON_free(text);
                cJSON_Delete(hardware);
            }

            // Mirrors CaptureAtBoot's NVS order with a fake store.
            struct FakeNvs {
                bool have_record = false;
                Record record{};
                bool have_age = false;
                uint32_t age = 0;
            };

            static void Boot(FakeNvs& nvs, Record& rtc) {
                const bool fresh = RecordValid(rtc);
                if (fresh) {
                    nvs.record = rtc;
                    nvs.have_record = true;
                }
                uint32_t next = 0;
                if (NextAge(fresh, nvs.have_age, nvs.age, &next)) {
                    nvs.age = next;
                    nvs.have_age = true;
                }
                if (fresh) rtc.magic = 0;
            }

            int main() {
                assert(sizeof(Record) == 72);
                assert(offsetof(Record, crc) == 68);

                // CRC-32 check value and the covered range.
                assert(Crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xCBF43926u);
                assert(Crc32(nullptr, 0) == 0);
                Record record = Sample("abort() was called at PC 0x4201a2b3 on core 0");
                assert(RecordValid(record));
                std::printf("CRC %08x ", RecordCrc(record));
                const auto* bytes = reinterpret_cast<const unsigned char*>(&record);
                for (size_t i = 0; i < offsetof(Record, crc); ++i) std::printf("%02x", bytes[i]);
                std::printf("\n");
                Record flipped = record;
                flipped.bt[3] ^= 1u;
                assert(!RecordValid(flipped));
                Record crc_only = record;
                crc_only.crc ^= 0x80000000u;
                assert(!RecordValid(crc_only));
                Record wrong_magic = record;
                wrong_magic.magic = kMagic + 1;
                wrong_magic.crc = RecordCrc(wrong_magic);
                assert(!RecordValid(wrong_magic));
                Record zero{};
                assert(!RecordValid(zero));

                // Reset names follow esp_reset_reason_t.
                const std::map<int, std::string> names = {
                    {0, "unknown"}, {1, "poweron"}, {2, "ext"}, {3, "sw"}, {4, "panic"},
                    {5, "int_wdt"}, {6, "task_wdt"}, {7, "wdt"}, {8, "deepsleep"},
                    {9, "brownout"}, {10, "sdio"}, {11, "usb"}, {12, "jtag"}, {13, "efuse"},
                    {14, "pwr_glitch"}, {15, "cpu_lockup"}, {16, "unknown"}, {-1, "unknown"},
                    {1000, "unknown"},
                };
                for (const auto& [reason, name] : names) {
                    assert(name == ResetName(reason));
                    assert(AllowedText(ResetName(reason)));
                }
                const char* kinds[] = {"debug", "iwdt", "twdt", "abort", "fault", "unknown"};
                for (uint32_t kind = 0; kind < 6; ++kind) {
                    assert(std::strcmp(KindName(kind), kinds[kind]) == 0);
                }
                assert(std::strcmp(KindName(0xFFFFFFFFu), "unknown") == 0);

                // Backtrace text: four PCs, 35 characters.
                char bt[kBacktraceTextBytes];
                assert(FormatBacktrace(record, bt, sizeof(bt)));
                assert(std::strcmp(bt, "4201a2b3.4200f00d.42011234.4037abcd") == 0);
                assert(std::strlen(bt) == 35 && AllowedText(bt));
                Record extremes{};
                extremes.bt[0] = 0xFFFFFFFFu;
                assert(FormatBacktrace(extremes, bt, sizeof(bt)));
                assert(std::strcmp(bt, "ffffffff.00000000.00000000.00000000") == 0);
                char small[kBacktraceTextBytes - 1] = {'x'};
                assert(!FormatBacktrace(record, small, sizeof(small)) && small[0] == '\0');

                // Why: sanitised, last 40 characters, bounded by the field.
                char why[kTextMax + 1];
                SanitizeTail("abort() was called at PC 0x4201a2b3 on core 0 (extra words)", 64,
                             why, sizeof(why));
                assert(std::strcmp(why, "at_PC_0x4201a2b3_on_core_0__extra_words_") == 0);
                assert(AllowedText(why));
                const std::string long_input = "assert failed: vTaskDelete tasks.c:1234 (xTask != NULL)";
                SanitizeTail(long_input.c_str(), long_input.size() + 1, why, sizeof(why));
                std::string expected = long_input.substr(long_input.size() - 40);
                for (auto& c : expected) {
                    if (!WhyCharAllowed(c)) c = '_';
                }
                assert(expected == why);
                SanitizeTail("", 1, why, sizeof(why));
                assert(why[0] == '\0');
                SanitizeTail(nullptr, 10, why, sizeof(why));
                assert(why[0] == '\0');
                char unterminated[kWhyBytes];
                std::memset(unterminated, 'A', sizeof(unterminated));
                unterminated[3] = ' ';
                SanitizeTail(unterminated, sizeof(unterminated), why, sizeof(why));
                assert(std::strlen(why) == 40 && why[3] == '_' && AllowedText(why));
                char tiny[4];
                SanitizeTail("LoadProhibited", 15, tiny, sizeof(tiny));
                assert(std::strcmp(tiny, "ted") == 0);
                SanitizeTail("\xff\x01/:", 5, why, sizeof(why));
                assert(std::strcmp(why, "____") == 0);

                // Age across fake boots: nothing before the first record, 0 on a
                // new record, +1 per boot, saturating at kMaxAge.
                FakeNvs nvs;
                Record rtc{};
                uint32_t next = 7;
                assert(!NextAge(false, false, 0, &next) && next == 7);
                assert(!NextAge(true, false, 0, nullptr));
                Boot(nvs, rtc);
                assert(!nvs.have_age && !nvs.have_record);
                rtc = Sample("StoreProhibited");
                Boot(nvs, rtc);
                assert(nvs.have_record && nvs.have_age && nvs.age == 0 && rtc.magic == 0);
                Boot(nvs, rtc);
                Boot(nvs, rtc);
                assert(nvs.age == 2);
                rtc = Sample("abort() was called at PC 0x42000000 on core 1");
                Boot(nvs, rtc);
                assert(nvs.age == 0 && std::strncmp(nvs.record.why, "abort()", 7) == 0);
                nvs.age = kMaxAge - 1;
                Boot(nvs, rtc);
                assert(nvs.age == kMaxAge);
                Boot(nvs, rtc);
                assert(nvs.age == kMaxAge);

                // Hello facts for Python to validate against the gateway rules.
                PrintFacts("none", 1, nullptr, 0);
                PrintFacts("abort", 4, &record, 0);
                Record fault = Sample("LoadProhibited");
                fault.kind = kKindFault;
                PrintFacts("fault", 11, &fault, 3);
                Record empty_why = Sample("");
                empty_why.kind = 99;
                PrintFacts("empty_why", 6, &empty_why, 20000);
                Record hostile = Sample("");
                std::memset(hostile.why, '"', sizeof(hostile.why));
                PrintFacts("hostile", 15, &hostile, kMaxAge);
                std::printf("crash record helpers passed\n");
                return 0;
            }
            '''
        )
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            source = temporary / "crash_record_test.cc"
            executable = temporary / "crash_record_test"
            source.write_text(program, encoding="utf-8")
            sanitize = ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                        "-fno-omit-frame-pointer"]
            cjson_object = temporary / "cJSON.o"
            built = subprocess.run(
                [shutil.which("cc"), "-std=c11", *sanitize, "-I", str(CJSON_DIR), "-c",
                 str(CJSON_DIR / "cJSON.c"), "-o", str(cjson_object)],
                capture_output=True, text=True, cwd=ROOT,
            )
            self.assertEqual(built.returncode, 0, built.stderr)
            built = subprocess.run(
                [shutil.which("c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror", *sanitize,
                 "-I", str(ROOT / "main"), "-I", str(CJSON_DIR), str(source), str(cjson_object),
                 "-o", str(executable)],
                capture_output=True, text=True, cwd=ROOT,
            )
            self.assertEqual(built.returncode, 0, built.stderr)
            run = subprocess.run(
                [str(executable)], capture_output=True, text=True, cwd=ROOT, timeout=30,
                env={**os.environ,
                     "ASAN_OPTIONS": "detect_leaks=0" if sys.platform == "darwin" else "detect_leaks=1"},
            )
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("crash record helpers passed", run.stdout)

        crc_line = next(line for line in run.stdout.splitlines() if line.startswith("CRC "))
        _, crc_hex, covered_hex = crc_line.split()
        self.assertEqual(int(crc_hex, 16), zlib.crc32(bytes.fromhex(covered_hex)))

        facts = {}
        for line in run.stdout.splitlines():
            if line.startswith("FACTS "):
                _, label, document = line.split(" ", 2)
                facts[label] = json.loads(document)
        self.assertEqual(facts["none"], {"reset": "poweron"})
        self.assertEqual(facts["abort"], {
            "reset": "panic", "crash_kind": "abort",
            "crash_bt": "4201a2b3.4200f00d.42011234.4037abcd",
            "crash_why": "abort___was_called_at_PC_0x4201a2b3_on_",
            "crash_age": 0,
        })
        self.assertEqual(facts["fault"]["reset"], "usb")
        self.assertEqual(facts["fault"]["crash_why"], "LoadProhibited")
        self.assertEqual(facts["fault"]["crash_age"], 3)
        self.assertNotIn("crash_why", facts["empty_why"])
        self.assertEqual(facts["empty_why"]["crash_kind"], "unknown")
        self.assertEqual(facts["empty_why"]["crash_age"], 10_000)
        self.assertEqual(facts["hostile"]["crash_why"], "_" * 40)
        for label, hardware in facts.items():
            with self.subTest(facts=label):
                self.assertTrue(set(hardware) <= HELLO_HARDWARE_KEYS, hardware)
                for key, value in hardware.items():
                    if key in INT_RANGES:
                        low, high = INT_RANGES[key]
                        self.assertIs(type(value), int, key)
                        self.assertTrue(low <= value <= high, (key, value))
                    else:
                        self.assertIsInstance(value, str, key)
                        self.assertRegex(value, TEXT_RE, key)


class ProvisionsCrashRecordWiringTests(unittest.TestCase):
    def test_hello_always_sends_hardware_with_only_allowlisted_keys(self):
        websocket = (ROOT / "main/protocols/websocket_protocol.cc").read_text(encoding="utf-8")
        header = (ROOT / "main/provisions_crash_record.h").read_text(encoding="utf-8")
        hello = _function_body(websocket, "std::string WebsocketProtocol::GetHelloMessage()")
        hardware = hello.split('cJSON_AddObjectToObject(root, "hardware")', 1)
        self.assertEqual(len(hardware), 2, "hello must create the hardware object")
        before = hardware[0].rsplit("#if CONFIG_PROVISIONS_GATEWAY_REQUIRED", 1)[1]
        self.assertNotIn("if (", before, "the hardware object must not be conditional")
        self.assertNotIn("touch.empty() || mic_ready", hello)
        self.assertIn("provisions::hardware::Pm1Wake()", hello)
        self.assertIn("pm1_wake >= 0 && pm1_wake <= 255", hello)
        self.assertIn("provisions::crash::AddHelloFacts(facts)", hello)
        self.assertIn('#include "provisions_crash_record.h"', websocket)

        emitted = set(re.findall(r'cJSON_Add\w+ToObject\(facts, "([a-z0-9_]+)"', hello))
        append = _function_body(header, "inline void AppendFacts(")
        emitted |= set(re.findall(r'cJSON_Add\w+ToObject\(hardware, "([a-z0-9_]+)"', append))
        self.assertEqual(emitted, HELLO_HARDWARE_KEYS)

    def test_panic_wrapper_is_linked_and_stays_off_heap_bus_flash_and_log(self):
        cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
        source = (ROOT / "main/provisions_crash_record.cc").read_text(encoding="utf-8")
        capture_block = cmake.split("if(CONFIG_PROVISIONS_LOCAL_CAPTURE)", 1)[1].split("endif()", 1)[0]
        self.assertIn('"provisions_hardware_facts.cc" "provisions_crash_record.cc"', capture_block)
        self.assertIn("set(PROVISIONS_CRASH_RECORD ON)", capture_block)
        link = cmake.split("if(PROVISIONS_CRASH_RECORD AND NOT CMAKE_BUILD_EARLY_EXPANSION)", 1)[1]
        link = link.split("\nendif()", 1)[0]
        self.assertIn('target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")', link)
        self.assertIn("CONFIG_BT_LE_CONTROLLER_LOG_WRAP_PANIC_HANDLER_ENABLE", link)
        self.assertLess(cmake.index("idf_component_register(SRCS"), cmake.index("--wrap=esp_panic_handler"))

        self.assertIn("RTC_NOINIT_ATTR Record s_rtc;", source)
        self.assertIn('extern "C" void __real_esp_panic_handler(panic_info_t* info);', source)
        wrapper = _function_body(source, "void __wrap_esp_panic_handler(panic_info_t* info)")
        signature = source[: source.index("void __wrap_esp_panic_handler(panic_info_t* info)")]
        self.assertIn("IRAM_ATTR", signature.rsplit("\n", 1)[1])
        self.assertTrue(wrapper.rstrip("}").rstrip().endswith("__real_esp_panic_handler(info);"))
        self.assertIn("g_panic_abort", wrapper)
        self.assertIn("esp_reset_reason_get_hint() == ESP_RST_TASK_WDT", wrapper)
        self.assertLess(wrapper.index("s_rtc = record;"), wrapper.index("esp_backtrace_get_next_frame"))
        for forbidden in ("ESP_LOG", "printf", "malloc", "new ", "std::", "nvs_", "i2c", "pmic_",
                          "esp_partition", "spi_flash", "heap_caps", "vTaskDelay"):
            self.assertNotIn(forbidden, wrapper, forbidden)

    def test_nvs_copy_runs_right_after_nvs_init(self):
        main = (ROOT / "main/main.cc").read_text(encoding="utf-8")
        source = (ROOT / "main/provisions_crash_record.cc").read_text(encoding="utf-8")
        body = _function_body(main, 'extern "C" void app_main(void)')
        check = body.index("ESP_ERROR_CHECK(ret);")
        capture = body.index("provisions::crash::CaptureAtBoot();")
        self.assertLess(body.index("nvs_flash_init()"), check)
        self.assertLess(check, capture)
        self.assertLess(capture, body.index("Application::GetInstance()"))
        between = body[check + len("ESP_ERROR_CHECK(ret);"):capture]
        self.assertEqual(between.strip().splitlines()[0], "#if CONFIG_PROVISIONS_LOCAL_CAPTURE")

        boot = _function_body(source, "void CaptureAtBoot()")
        self.assertIn("s_reset = esp_reset_reason();", boot.split("nvs_open", 1)[0])
        self.assertIn('kNamespace = "diag"', source)
        self.assertIn('kRecordKey = "crash"', source)
        self.assertIn('kAgeKey = "age"', source)
        self.assertIn("NextAge(fresh, age_present, stored_age, &next_age)", boot)
        self.assertLess(boot.index("nvs_commit(handle)"), boot.index("s_rtc.magic = 0;"))
        self.assertIn("static_assert(ESP_RST_UNKNOWN == 0", source)
        self.assertIn("ESP_RST_CPU_LOCKUP == 15", source)

    def test_pm1_reports_wake_source_and_requires_a_four_second_hold(self):
        board = (BOARD_DIR / "m5stack_stopwatch.cc").read_text(encoding="utf-8")
        init = _function_body(board, "void InitializeI2c()")
        begin = init.index("pmic_.begin(")
        wake = init.index("pmic_.getWakeSource(&pm1_wake, M5PM1_CLEAN_ALL) == M5PM1_OK")
        hold = init.index("pmic_.btnSetConfig(M5PM1_BTN_TYPE_LONG, M5PM1_BTN_LONG_PRESS_DELAY_4000MS)")
        self.assertLess(begin, wake)
        self.assertLess(begin, hold)
        self.assertLess(wake, init.index("provisions::hardware::SetPm1Wake(pm1_wake)"))
        self.assertLess(hold, init.index("pmic_.setChargeEnable(true)"))
        facts = (ROOT / "main/provisions_hardware_facts.cc").read_text(encoding="utf-8")
        self.assertIn("std::atomic<int> pm1_wake_{-1};", facts)
        if PM1_HEADER.is_file():
            pm1 = PM1_HEADER.read_text(encoding="utf-8", errors="replace")
            self.assertRegex(pm1, r"M5PM1_BTN_LONG_PRESS_DELAY_4000MS = 0x03")
            self.assertRegex(pm1, r"#define M5PM1_REG_BTN_CFG_1 0x49")
            self.assertIn("LONG_DLY - long-press delay: 00=1s, 01=2s, 10=3s, 11=4s", pm1)
            self.assertRegex(pm1, r"getWakeSource\(uint8_t\* src, m5pm1_clean_type_t cleanType")

    def test_diagnostic_orbit_profiles_pin_watchdog_heap_and_stack_checks(self):
        profiles = {
            "bench_profile.json": "provisions-kitchen-helper-stopwatch",
            "capture_profile.json": "provisions-kitchen-helper-stopwatch",
            "hardware_bench_profile.json": "provisions-kitchen-helper-stopwatch-schedule-bench",
            "config.json": "provisions-kitchen-helper-stopwatch-schedule-bench",
        }
        for filename, name in profiles.items():
            with self.subTest(profile=filename):
                config = json.loads((BOARD_DIR / filename).read_text(encoding="utf-8"))
                build = next(item for item in config["builds"] if item["name"] == name)
                options = build["sdkconfig_append"]
                self.assertTrue(DIAGNOSTIC_OPTIONS <= set(options), options)
                keys = [option.split("=", 1)[0] for option in options]
                self.assertEqual(len(keys), len(set(keys)), "a profile must not redefine a key")
                for competing in ("CONFIG_HEAP_POISONING_DISABLED", "CONFIG_HEAP_POISONING_COMPREHENSIVE",
                                  "CONFIG_COMPILER_STACK_CHECK_MODE_NONE",
                                  "CONFIG_COMPILER_STACK_CHECK_MODE_STRONG",
                                  "CONFIG_COMPILER_STACK_CHECK_MODE_ALL"):
                    self.assertNotIn(competing, keys)


if __name__ == "__main__":
    unittest.main()
