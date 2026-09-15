import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD_DIR = ROOT / "main/boards/m5stack/stopwatch"
CJSON_DIR = ROOT / "managed_components/espressif__cjson/cJSON"


class ProvisionsTimerSnapshotTests(unittest.TestCase):
    def test_stopwatch_advertises_and_consumes_read_only_orbit_contract(self):
        application = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        board = (BOARD_DIR / "m5stack_stopwatch.cc").read_text(encoding="utf-8")
        websocket = (ROOT / "main/protocols/websocket_protocol.cc").read_text(
            encoding="utf-8"
        )

        self.assertIn('"galley_timer_snapshot_v1", true', websocket)
        self.assertIn("HasProvisionsTimerSnapshotConsumer()", websocket)
        self.assertIn("kMaximumProvisionsTextFrameBytes = 32 * 1024", websocket)
        self.assertIn('strcmp(state->valuestring, "timer_snapshot")', application)
        self.assertIn(
            'strcmp(state->valuestring, "timer_snapshot_reset")', application
        )
        self.assertIn("HasProvisionsTimerSnapshotConsumer() &&", application)
        self.assertIn("RegisterProvisionsTimerSnapshotCallback", board)
        self.assertIn("ApplyTimerSnapshot(update.snapshot)", board)
        self.assertIn("ResetTimerSnapshot()", board)
        self.assertIn('lv_label_set_text(objects.label, "+")', board)
        self.assertIn("ProvisionsStopwatchOrbit::PreserveAttention", board)
        self.assertIn("ProvisionsStopwatchOrbit::LatchDueTimers", board)
        self.assertIn("display_->SilenceTimerAlarm()", board)
        self.assertNotIn("timer.acknowledge", board)

        apply_snapshot = board.split("void ApplyTimerSnapshot", 1)[1].split(
            "void ResetTimerSnapshot", 1
        )[0]
        new_session = apply_snapshot.split("if (!same_galley_session)", 1)[1].split(
            "orbit_snapshot_received_", 1
        )[0]
        self.assertIn("orbit_slot_board_ = ProvisionsStopwatchOrbit::SlotBoard{}", new_session)
        self.assertIn("timer_alarm_state_.BeginNewSession()", new_session)

        reset = board.split("void ResetTimerSnapshot()", 1)[1].split(
            "bool SilenceTimerAlarm()", 1
        )[0]
        self.assertIn("timer_alarm_active_.store(false)", reset)
        self.assertIn("timer_snapshot_ = ProvisionsTimerSnapshot::Snapshot{}", reset)
        self.assertIn("orbit_slot_board_ = ProvisionsStopwatchOrbit::SlotBoard{}", reset)
        self.assertIn("timer_alarm_state_.Reset()", reset)
        self.assertIn("ApplyAlarmOutputChange(output_change)", reset)
        silence = board.split("bool SilenceTimerAlarm()", 1)[1].split(
            "bool HasTimerAlarm()", 1
        )[0]
        self.assertIn("timer_alarm_state_.Silence()", silence)
        self.assertIn("ApplyAlarmOutputChange(output_change)", silence)
        self.assertNotIn("timer_snapshot_ =", silence)
        alarm_output = board.split("SetTimerAlarmOutputCallback", 1)[1].split(
            "RegisterProvisionsTimerSnapshotCallback", 1
        )[0]
        self.assertIn("active ? HIGH : LOW", alarm_output)
        self.assertIn("PlayLocalFeedback", alarm_output)
        self.assertIn("OGG_EXCLAMATION", alarm_output)
        self.assertIn("CancelLocalFeedback", alarm_output)

        alarm_listening = application.split("void Application::ServiceAlarmListening", 1)[
            1
        ].split("void Application::NoteTalkPressDown", 1)[0]
        self.assertIn("kAnnouncementGraceUs = 4LL * 1000 * 1000", alarm_listening)
        self.assertIn(
            "alarm_listen_next_us_ = now_us + kAnnouncementGraceUs", alarm_listening
        )

    @unittest.skipUnless(
        shutil.which("c++") and shutil.which("cc"),
        "host C and C++ compilers are unavailable",
    )
    def test_snapshot_parser_is_exact_bounded_and_atomic(self):
        test_source = textwrap.dedent(
            r'''
            #include "provisions_timer_snapshot.h"
            #include <cJSON.h>

            #include <cassert>
            #include <cstdio>
            #include <string>

            using ProvisionsTimerSnapshot::ApplyResult;
            using ProvisionsTimerSnapshot::Gate;
            using ProvisionsTimerSnapshot::Update;

            constexpr const char* kVoice =
                "11111111-1111-4111-8111-111111111111";
            constexpr const char* kOtherVoice =
                "22222222-2222-4222-8222-222222222222";
            constexpr const char* kGalleyA =
                "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA";
            constexpr const char* kGalleyB =
                "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
            constexpr const char* kTimer =
                "00000000-0000-0000-0000-000000000001";

            std::string TimerJson(const std::string& id,
                                  const std::string& label,
                                  const std::string& status = "active",
                                  const std::string& deadline = "2000000000000") {
                return "{\"id\":\"" + id + "\",\"label\":\"" + label +
                       "\",\"deadline_ms\":" + deadline + ",\"status\":\"" +
                       status + "\"}";
            }

            std::string SnapshotFrame(const std::string& voice,
                                      const std::string& galley,
                                      const std::string& revision,
                                      const std::string& timers,
                                      const std::string& payload_suffix = "",
                                      const std::string& server_now =
                                          "1900000000000") {
                return "{\"type\":\"provisions\",\"session_id\":\"" + voice +
                       "\",\"state\":\"timer_snapshot\",\"timer_snapshot\":"
                       "{\"version\":1,\"session_id\":\"" + galley +
                       "\",\"revision\":" + revision +
                       ",\"server_now_ms\":" + server_now + ",\"timers\":[" + timers +
                       "]" + payload_suffix + "}}";
            }

            std::string ResetFrame(const std::string& voice,
                                   const std::string& payload = "{\"version\":1}") {
                return "{\"type\":\"provisions\",\"session_id\":\"" + voice +
                       "\",\"state\":\"timer_snapshot_reset\","
                       "\"timer_snapshot_reset\":" + payload + "}";
            }

            ApplyResult ApplyJson(Gate& gate, const std::string& json,
                                  const std::string& voice, Update& update) {
                cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
                assert(root != nullptr);
                const auto result = gate.Apply(root, voice, update);
                cJSON_Delete(root);
                return result;
            }

            std::string TimerList(int count, const std::string& label) {
                std::string timers;
                for (int index = 0; index < count; ++index) {
                    char id[37];
                    std::snprintf(id, sizeof(id),
                                  "00000000-0000-0000-0000-%012d", index + 1);
                    if (!timers.empty()) timers.push_back(',');
                    timers += TimerJson(id, label);
                }
                return timers;
            }

            int main() {
                Gate gate;
                Update update;
                const auto first = SnapshotFrame(
                    kVoice, kGalleyA, "7", TimerJson(kTimer, "Sauce", "attention"));
                assert(ApplyJson(gate, first, kVoice, update) == ApplyResult::kAccepted);
                assert(update.kind == Update::Kind::kSnapshot);
                assert(update.snapshot.session_id ==
                       "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa");
                assert(update.snapshot.timers[0].id == kTimer);
                assert(ApplyJson(gate, first, kVoice, update) ==
                       ApplyResult::kStaleRevision);

                update.snapshot.session_id = "sentinel";
                const auto malformed = SnapshotFrame(
                    kVoice, kGalleyA, "8", TimerJson(kTimer, "Sauce"),
                    ",\"extra\":true");
                assert(ApplyJson(gate, malformed, kVoice, update) ==
                       ApplyResult::kMalformed);
                assert(update.snapshot.session_id == "sentinel");
                const auto revision_eight = SnapshotFrame(
                    kVoice, kGalleyA, "8", TimerJson(kTimer, "Sauce"));
                assert(ApplyJson(gate, revision_eight, kVoice, update) ==
                       ApplyResult::kAccepted);

                const auto mismatch = SnapshotFrame(
                    kOtherVoice, kGalleyA, "9", TimerJson(kTimer, "Sauce"));
                assert(ApplyJson(gate, mismatch, kVoice, update) ==
                       ApplyResult::kWebsocketSessionMismatch);
                const auto revision_nine = SnapshotFrame(
                    kVoice, kGalleyA, "9", TimerJson(kTimer, "Sauce"));
                assert(ApplyJson(gate, revision_nine, kVoice, update) ==
                       ApplyResult::kAccepted);

                const auto new_session = SnapshotFrame(
                    kVoice, kGalleyB, "0", TimerJson(kTimer, "Sauce"));
                assert(ApplyJson(gate, new_session, kVoice, update) ==
                       ApplyResult::kAccepted);

                const auto bad_reset = ResetFrame(
                    kVoice, "{\"version\":1,\"extra\":true}");
                assert(ApplyJson(gate, bad_reset, kVoice, update) ==
                       ApplyResult::kMalformed);
                assert(ApplyJson(gate, ResetFrame(kOtherVoice), kVoice, update) ==
                       ApplyResult::kWebsocketSessionMismatch);
                assert(ApplyJson(gate, ResetFrame(kVoice), kVoice, update) ==
                       ApplyResult::kAccepted);
                assert(update.kind == Update::Kind::kReset);
                assert(ApplyJson(gate, new_session, kVoice, update) ==
                       ApplyResult::kAccepted);

                Gate unicode_gate;
                std::string eighty_emoji;
                for (int index = 0; index < 80; ++index) {
                    eighty_emoji += "\xF0\x9F\x98\x80";
                }
                assert(eighty_emoji.size() == 320);
                const auto maximum = SnapshotFrame(
                    kVoice, kGalleyA, "0", TimerList(64, eighty_emoji));
                assert(maximum.size() <= 32 * 1024);
                assert(ApplyJson(unicode_gate, maximum, kVoice, update) ==
                       ApplyResult::kAccepted);

                Gate too_many_gate;
                assert(ApplyJson(too_many_gate,
                                 SnapshotFrame(kVoice, kGalleyA, "0",
                                               TimerList(65, "x")),
                                 kVoice, update) == ApplyResult::kMalformed);
                Gate too_long_gate;
                assert(ApplyJson(too_long_gate,
                                 SnapshotFrame(kVoice, kGalleyA, "0",
                                               TimerJson(kTimer,
                                                         eighty_emoji + "x")),
                                 kVoice, update) == ApplyResult::kMalformed);

                const char* forbidden[] = {
                    "\\n", "\xE2\x80\x8B", "\xE2\x80\xAE",
                    "\xEE\x80\x80", "\xEF\xB7\x90",
                };
                for (const char* label : forbidden) {
                    Gate forbidden_gate;
                    assert(ApplyJson(forbidden_gate,
                                     SnapshotFrame(kVoice, kGalleyA, "0",
                                                   TimerJson(kTimer, label)),
                                     kVoice, update) == ApplyResult::kMalformed);
                }
                const char* allowed_category_examples[] = {
                    "\xE1\x9E\xB4",          // U+17B4, Mn
                    "\xE2\x81\xA5",          // U+2065, Cn
                    "\xF0\x93\x91\x81",      // U+13441, Lo
                };
                for (const char* label : allowed_category_examples) {
                    Gate allowed_category_gate;
                    assert(ApplyJson(allowed_category_gate,
                                     SnapshotFrame(kVoice, kGalleyA, "0",
                                                   TimerJson(kTimer, label)),
                                     kVoice, update) == ApplyResult::kAccepted);
                }
                Gate deprecated_format_gate;
                assert(ApplyJson(deprecated_format_gate,
                                 SnapshotFrame(kVoice, kGalleyA, "0",
                                               TimerJson(kTimer,
                                                         "\xE2\x81\xAA")),
                                 kVoice, update) == ApplyResult::kMalformed);

                Gate duplicate_gate;
                const auto duplicate = TimerJson(kTimer, "one") + "," +
                                       TimerJson(kTimer, "two");
                assert(ApplyJson(duplicate_gate,
                                 SnapshotFrame(kVoice, kGalleyA, "0", duplicate),
                                 kVoice, update) == ApplyResult::kMalformed);
                Gate unsafe_integer_gate;
                assert(ApplyJson(unsafe_integer_gate,
                                 SnapshotFrame(kVoice, kGalleyA,
                                               "9007199254740992",
                                               TimerJson(kTimer, "x")),
                                 kVoice, update) == ApplyResult::kMalformed);
                Gate maximum_integer_gate;
                assert(ApplyJson(maximum_integer_gate,
                                 SnapshotFrame(
                                     kVoice, kGalleyA, "9007199254740991",
                                     TimerJson(kTimer, "x", "active",
                                               "9007199254740991"),
                                     "", "9007199254740991"),
                                 kVoice, update) == ApplyResult::kAccepted);
                for (const char* invalid_revision : {"-1", "1.5"}) {
                    Gate invalid_revision_gate;
                    assert(ApplyJson(
                               invalid_revision_gate,
                               SnapshotFrame(kVoice, kGalleyA,
                                             invalid_revision,
                                             TimerJson(kTimer, "x")),
                               kVoice, update) == ApplyResult::kMalformed);
                }
                Gate zero_server_time_gate;
                assert(ApplyJson(zero_server_time_gate,
                                 SnapshotFrame(kVoice, kGalleyA, "0",
                                               TimerJson(kTimer, "x"), "", "0"),
                                 kVoice, update) == ApplyResult::kMalformed);
                for (const char* invalid_deadline :
                     {"0", "1.5", "9007199254740992"}) {
                    Gate invalid_deadline_gate;
                    assert(ApplyJson(
                               invalid_deadline_gate,
                               SnapshotFrame(kVoice, kGalleyA, "0",
                                             TimerJson(kTimer, "x", "active",
                                                       invalid_deadline)),
                               kVoice, update) == ApplyResult::kMalformed);
                }
                return 0;
            }
            '''
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            source = temporary / "timer_snapshot_test.cc"
            cjson_object = temporary / "cJSON.o"
            executable = temporary / "timer_snapshot_test"
            source.write_text(test_source, encoding="utf-8")
            subprocess.run(
                [
                    shutil.which("cc"),
                    "-std=c11",
                    "-I",
                    str(CJSON_DIR),
                    "-c",
                    str(CJSON_DIR / "cJSON.c"),
                    "-o",
                    str(cjson_object),
                ],
                check=True,
                cwd=ROOT,
            )
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
                    "-I",
                    str(CJSON_DIR),
                    str(source),
                    str(ROOT / "main/provisions_timer_snapshot.cc"),
                    str(cjson_object),
                    "-o",
                    str(executable),
                ],
                check=True,
                cwd=ROOT,
            )
            subprocess.run([str(executable)], check=True, cwd=ROOT)

    @unittest.skipUnless(shutil.which("c++"), "host C++ compiler is unavailable")
    def test_orbit_engine_preserves_slots_collisions_and_attention(self):
        test_source = textwrap.dedent(
            r'''
            #include "orbit_dial.h"

            #include <algorithm>
            #include <cassert>
            #include <cmath>
            #include <string>
            #include <vector>

            using ProvisionsTimerSnapshot::Timer;
            using ProvisionsTimerSnapshot::TimerStatus;
            using namespace ProvisionsStopwatchOrbit;

            Timer Make(std::string id, int64_t deadline,
                       TimerStatus status = TimerStatus::kActive) {
                return Timer{.id = std::move(id), .label = "named timer",
                             .deadline_ms = deadline, .status = status};
            }

            int SlotFor(const SlotBoard& board, const std::string& id) {
                for (std::size_t index = 0; index < board.slots().size(); ++index) {
                    if (board.slots()[index].occupied &&
                        board.slots()[index].timer.id == id) {
                        return static_cast<int>(index);
                    }
                }
                return -1;
            }

            int main() {
                constexpr int64_t now = 1'000'000;
                AlarmState alarm;
                std::vector<Timer> finished = {
                    Make("alarm-a", now, TimerStatus::kAttention),
                    Make("alarm-b", now, TimerStatus::kAttention),
                };
                assert(alarm.Update(finished) == AlarmOutputChange::kStart);
                assert(alarm.active() && !alarm.silenced());
                assert(alarm.Silence() == AlarmOutputChange::kStop);
                assert(alarm.active() && alarm.silenced());
                finished.erase(finished.begin());
                assert(alarm.Update(finished) == AlarmOutputChange::kNone);
                assert(alarm.active() && alarm.silenced());
                finished.push_back(
                    Make("alarm-c", now, TimerStatus::kAttention));
                assert(alarm.Update(finished) == AlarmOutputChange::kStart);
                assert(alarm.active() && !alarm.silenced());
                assert(alarm.Silence() == AlarmOutputChange::kStop);
                alarm.BeginNewSession();
                finished.resize(1);
                assert(alarm.Update(finished) == AlarmOutputChange::kStart);
                assert(alarm.Reset() == AlarmOutputChange::kStop);
                assert(!alarm.active() && !alarm.silenced());

                std::vector<Timer> timers = {
                    Make("a", now + 30'000), Make("b", now + 90'000),
                    Make("c", now + 300'000),
                };
                SlotBoard board;
                board.Update(timers, now);
                const int a_slot = SlotFor(board, "a");
                const int b_slot = SlotFor(board, "b");
                assert(a_slot >= 0 && b_slot >= 0 && a_slot != b_slot);
                std::reverse(timers.begin(), timers.end());
                board.Update(timers, now + 1'000);
                assert(SlotFor(board, "a") == a_slot);
                assert(SlotFor(board, "b") == b_slot);

                auto collisions = CollidingIds(timers, now);
                assert(collisions.size() == 2);
                assert(std::find(collisions.begin(), collisions.end(), "a") !=
                       collisions.end());
                assert(std::find(collisions.begin(), collisions.end(), "b") !=
                       collisions.end());
                timers.push_back(Make("expired", now - 1));
                timers.push_back(Make("attention", now + 1,
                                      TimerStatus::kAttention));
                collisions = CollidingIds(timers, now);
                assert(std::find(collisions.begin(), collisions.end(), "expired") ==
                       collisions.end());
                assert(std::find(collisions.begin(), collisions.end(), "attention") ==
                       collisions.end());

                LatchDueTimers(timers, now);
                assert(timers[3].status == TimerStatus::kAttention);
                std::vector<Timer> refreshed = {Make("expired", now - 1)};
                PreserveAttention(timers, refreshed);
                assert(refreshed[0].status == TimerStatus::kAttention);
                assert(FinishedTimers(refreshed, now).size() == 1);
                refreshed = {Make("expired", now + 500'000)};
                PreserveAttention(timers, refreshed);
                assert(refreshed[0].status == TimerStatus::kActive);
                assert(FinishedTimers(refreshed, now).empty());

                std::vector<Timer> seven;
                for (int index = 0; index < 7; ++index) {
                    seven.push_back(Make(std::to_string(index),
                                         now + (index + 1) * 10'000));
                }
                board = SlotBoard{};
                board.Update(seven, now);
                assert(board.occupied_count() == 6);
                assert(board.overflow_count() == 1);

                for (int index = 0; index < kMaximumSlots; ++index) {
                    const auto center = SlotCenter(index);
                    const auto distance = std::hypot(center.x - 233.0,
                                                     center.y - 233.0);
                    assert(distance + kSlotRadius <= 229.0);
                }
                assert(FormatRemaining(now + 65'000, now) == "1:05");
                Slot slot{.occupied = true, .timer = Make("ring", now + 10'000),
                          .first_seen_ms = now};
                assert(RemainingFraction(slot, now) == 1.0F);
                assert(RemainingFraction(slot, now + 10'000) == 0.0F);
                return 0;
            }
            '''
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            source = temporary / "orbit_engine_test.cc"
            executable = temporary / "orbit_engine_test"
            source.write_text(test_source, encoding="utf-8")
            (temporary / "sdkconfig.h").write_text(
                "#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1\n", encoding="utf-8"
            )
            subprocess.run(
                [
                    shutil.which("c++"),
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(temporary),
                    "-I",
                    str(ROOT / "main"),
                    "-I",
                    str(BOARD_DIR),
                    str(source),
                    str(BOARD_DIR / "orbit_dial.cc"),
                    "-o",
                    str(executable),
                ],
                check=True,
                cwd=ROOT,
            )
            subprocess.run([str(executable)], check=True, cwd=ROOT)

    @unittest.skipUnless(shutil.which("c++"), "host C++ compiler is unavailable")
    def test_overview_tap_resolves_slots_and_pins_compact_focus(self):
        test_source = textwrap.dedent(
            r"""
            #include "orbit_dial.h"

            #include <cassert>
            #include <cmath>
            #include <string>
            #include <vector>

            using ProvisionsTimerSnapshot::Timer;
            using ProvisionsTimerSnapshot::TimerStatus;
            using namespace ProvisionsStopwatchOrbit;

            Timer Make(std::string id, std::string label, int64_t deadline,
                       TimerStatus status = TimerStatus::kActive) {
                return Timer{.id = std::move(id), .label = std::move(label),
                             .deadline_ms = deadline, .status = status};
            }

            int SlotFor(const SlotBoard& board, const std::string& id) {
                for (int index = 0; index < kMaximumSlots; ++index) {
                    if (board.slots()[index].occupied && board.slots()[index].timer.id == id) {
                        return index;
                    }
                }
                return -1;
            }

            // Display point -> the CST820 raw point that M5GFX's StopWatch
            // calibration (0..233 over the panel) would report for it.
            int RawFor(int display) {
                return static_cast<int>(std::lround(display * double(kTouchRawMaximum) /
                                                    double(kDisplaySize - 1)));
            }

            // What the board does with a tap: map the raw point, resolve, then
            // pin only an occupied slot.
            void Tap(const SlotBoard& board, int raw_x, int raw_y, std::string& pinned) {
                const int slot = SlotAtPoint(RawTouchToDisplay(raw_x), RawTouchToDisplay(raw_y));
                if (slot >= 0 && board.slots()[slot].occupied) {
                    pinned = board.slots()[slot].timer.id;
                }
            }

            void TapDisplay(const SlotBoard& board, int x, int y, std::string& pinned) {
                Tap(board, RawFor(x), RawFor(y), pinned);
            }

            int main() {
                // Geometry: every slot centre and every point just inside its
                // hit circle resolves to that slot; the dial centre, the corners
                // and the gap between rings do not. Hit circles never overlap.
                constexpr double kPi = 3.14159265358979;
                for (int index = 0; index < kMaximumSlots; ++index) {
                    const auto center = SlotCenter(index);
                    assert(SlotAtPoint(center.x, center.y) == index);
                    for (int step = 0; step < 16; ++step) {
                        const double angle = step * kPi / 8.0;
                        const int reach = kSlotHitRadius - 1;
                        const int x = center.x + static_cast<int>(std::lround(reach * std::cos(angle)));
                        const int y = center.y + static_cast<int>(std::lround(reach * std::sin(angle)));
                        assert(SlotAtPoint(x, y) == index);
                    }
                    assert(SlotAtPoint(center.x, center.y - kSlotHitRadius - 2) != index);
                    const auto next = SlotCenter((index + 1) % kMaximumSlots);
                    assert(std::hypot(next.x - center.x, next.y - center.y) >
                           2.0 * kSlotHitRadius);
                }
                assert(kSlotHitRadius >= kSlotRadius + 24);
                assert(SlotAtPoint(kDisplaySize / 2, kDisplaySize / 2) == -1);
                assert(SlotAtPoint(0, 0) == -1);
                assert(SlotAtPoint(kDisplaySize - 1, kDisplaySize - 1) == -1);
                assert(SlotAtPoint(-4095, 4095) == -1);

                // Raw CST820 axis -> panel pixel, as M5GFX calibrates the
                // StopWatch: 0..233 spans 0..465, rounded, clamped, monotonic.
                assert(kTouchRawMaximum == 233);
                assert(RawTouchToDisplay(0) == 0);
                assert(RawTouchToDisplay(kTouchRawMaximum) == kDisplaySize - 1);
                assert(RawTouchToDisplay(116) == 232 && RawTouchToDisplay(117) == 233);
                assert(RawTouchToDisplay(-1) == 0);
                assert(RawTouchToDisplay(-4095) == 0);
                assert(RawTouchToDisplay(234) == kDisplaySize - 1);
                assert(RawTouchToDisplay(4095) == kDisplaySize - 1);
                for (int raw = 0; raw <= kTouchRawMaximum; ++raw) {
                    const double exact = raw * double(kDisplaySize - 1) / kTouchRawMaximum;
                    assert(std::fabs(RawTouchToDisplay(raw) - exact) <= 0.5 + 1e-9);
                    if (raw > 0) {
                        const int step = RawTouchToDisplay(raw) - RawTouchToDisplay(raw - 1);
                        assert(step == 1 || step == 2);
                    }
                }

                // The bug this fixes: the raw point for rice's ring (slot 1,
                // centre 366,157) is about 183,78. Hit-tested unmapped it lands
                // on pasta's slot 0; mapped it is back on slot 1.
                assert(SlotCenter(1).x == 366 && SlotCenter(1).y == 157);
                assert(SlotAtPoint(183, 78) == 0);
                assert(RawTouchToDisplay(183) == 365 && RawTouchToDisplay(78) == 156);
                assert(SlotAtPoint(RawTouchToDisplay(183), RawTouchToDisplay(78)) == 1);
                // Every slot: the half-scale raw centre (both the calibration
                // inverse and a plain halving) resolves to that slot once mapped,
                // and never does unmapped; so do raw points near the hit edge.
                for (int index = 0; index < kMaximumSlots; ++index) {
                    const auto center = SlotCenter(index);
                    const int raw_x = RawFor(center.x);
                    const int raw_y = RawFor(center.y);
                    assert(SlotAtPoint(RawTouchToDisplay(raw_x), RawTouchToDisplay(raw_y)) == index);
                    assert(SlotAtPoint(RawTouchToDisplay(center.x / 2),
                                       RawTouchToDisplay(center.y / 2)) == index);
                    assert(SlotAtPoint(raw_x, raw_y) != index);
                    for (int step = 0; step < 16; ++step) {
                        const double angle = step * kPi / 8.0;
                        const int reach = kSlotHitRadius - 4;
                        const int x = center.x + static_cast<int>(std::lround(reach * std::cos(angle)));
                        const int y = center.y + static_cast<int>(std::lround(reach * std::sin(angle)));
                        assert(SlotAtPoint(RawTouchToDisplay(RawFor(x)),
                                           RawTouchToDisplay(RawFor(y))) == index);
                    }
                }
                // Raw dial centre and raw corners stay off every slot.
                assert(SlotAtPoint(RawTouchToDisplay(116), RawTouchToDisplay(116)) == -1);
                assert(SlotAtPoint(RawTouchToDisplay(0), RawTouchToDisplay(0)) == -1);
                assert(SlotAtPoint(RawTouchToDisplay(233), RawTouchToDisplay(233)) == -1);

                // Dennis's report: pasta then rice, both two minutes, pasta first.
                constexpr int64_t now = 5'000'000;
                std::vector<Timer> timers = {
                    Make("service-id", "Service", now + 3'600'000),
                    Make("pasta-id", "Pasta", now + 120'000),
                    Make("rice-id", "Rice", now + 125'000),
                };
                constexpr int kService = 0;
                std::vector<Timer> slot_timers(timers.begin() + 1, timers.end());
                SlotBoard board;
                board.Update(slot_timers, now);
                const int pasta_slot = SlotFor(board, "pasta-id");
                const int rice_slot = SlotFor(board, "rice-id");
                assert(pasta_slot == 0 && rice_slot == 1);

                // No pin: the soonest running timer, never the service ring.
                std::string pinned;
                assert(CompactFocusIndex(timers, kService, board, pinned) == 1);

                // Tap on slot N pins timer N: rice shows rice, pasta shows pasta.
                Tap(board, 183, 78, pinned);
                assert(pinned == "rice-id");
                assert(CompactFocusIndex(timers, kService, board, pinned) == 2);
                assert(pinned == "rice-id");
                auto rice_center = SlotCenter(rice_slot);
                const auto pasta_center = SlotCenter(pasta_slot);
                TapDisplay(board, pasta_center.x, pasta_center.y, pinned);
                assert(pinned == "pasta-id");
                assert(CompactFocusIndex(timers, kService, board, pinned) == 1);
                TapDisplay(board, rice_center.x + 30, rice_center.y - 30, pinned);
                assert(pinned == "rice-id");
                // Tap outside every slot (dial centre) or on an empty slot keeps the focus.
                TapDisplay(board, kDisplaySize / 2, kDisplaySize / 2, pinned);
                assert(pinned == "rice-id");
                int empty_slot = -1;
                for (int index = 0; index < kMaximumSlots; ++index) {
                    if (!board.slots()[index].occupied) empty_slot = index;
                }
                assert(empty_slot >= 0);
                const auto empty_center = SlotCenter(empty_slot);
                TapDisplay(board, empty_center.x, empty_center.y, pinned);
                assert(pinned == "rice-id");
                assert(CompactFocusIndex(timers, kService, board, pinned) == 2);

                // A pinned timer that is due stays in focus; the pin outlives
                // a sooner timer appearing in the same snapshot.
                timers[2].status = TimerStatus::kAttention;
                assert(CompactFocusIndex(timers, kService, board, pinned) == 2);
                timers[2].status = TimerStatus::kActive;
                timers.push_back(Make("egg-id", "Egg", now + 10'000));
                assert(CompactFocusIndex(timers, kService, board, pinned) == 2);
                timers.pop_back();

                // Pin cleared when its timer disappears: back to soonest.
                std::vector<Timer> without_rice(timers.begin(), timers.begin() + 2);
                assert(CompactFocusIndex(without_rice, kService, board, pinned) == 1);
                assert(pinned.empty());
                // ...and it stays cleared when the id shows up again.
                assert(CompactFocusIndex(timers, kService, board, pinned) == 1);

                // A pin never selects the service ring, and is dropped if tried.
                pinned = "service-id";
                assert(CompactFocusIndex(timers, kService, board, pinned) == 1);
                assert(pinned.empty());

                // A pinned timer pushed off the six-slot board (extended past
                // a seventh timer) has no slot colour or counting ring, so the
                // pin is dropped and the soonest seated timer takes focus.
                std::vector<Timer> seven;
                for (int index = 0; index < 7; ++index) {
                    seven.push_back(Make("t" + std::to_string(index), "T",
                                         now + 60'000 * (index + 1)));
                }
                SlotBoard full;
                full.Update(seven, now);
                assert(full.overflow_count() == 1 && SlotFor(full, "t6") < 0);
                const int t2_slot = SlotFor(full, "t2");
                assert(t2_slot >= 0);
                TapDisplay(full, SlotCenter(t2_slot).x, SlotCenter(t2_slot).y, pinned);
                assert(pinned == "t2");
                assert(CompactFocusIndex(seven, -1, full, pinned) == 2);
                seven[2].deadline_ms = now + 600'000;
                full.Update(seven, now + 1'000);
                assert(SlotFor(full, "t2") < 0 && SlotFor(full, "t6") >= 0);
                const int focus = CompactFocusIndex(seven, -1, full, pinned);
                assert(focus == 0);
                assert(pinned.empty());
                assert(SlotFor(full, seven[focus].id) >= 0);
                // Back on the board later, the dropped pin does not return.
                seven[2].deadline_ms = now + 150'000;
                full.Update(seven, now + 2'000);
                assert(SlotFor(full, "t2") >= 0);
                assert(CompactFocusIndex(seven, -1, full, pinned) == 0);
                // A pin that is seated but not in this snapshot is dropped too.
                pinned = "t3";
                std::vector<Timer> without_t3 = seven;
                without_t3.erase(without_t3.begin() + 3);
                assert(CompactFocusIndex(without_t3, -1, full, pinned) == 0);
                assert(pinned.empty());

                // Soonest rule unchanged without a pin: running beats due,
                // earlier deadline wins within a status, ties keep the first.
                std::vector<Timer> mixed = {
                    Make("due", "Due", now - 1'000, TimerStatus::kAttention),
                    Make("later", "Later", now + 90'000),
                    Make("sooner", "Sooner", now + 30'000),
                    Make("tie", "Tie", now + 30'000),
                };
                SlotBoard mixed_board;
                mixed_board.Update(mixed, now);
                assert(CompactFocusIndex(mixed, -1, mixed_board, pinned) == 2);
                mixed.resize(1);
                assert(CompactFocusIndex(mixed, -1, mixed_board, pinned) == 0);
                assert(CompactFocusIndex({}, -1, SlotBoard{}, pinned) == -1);
                pinned = "gone";
                assert(CompactFocusIndex({}, -1, SlotBoard{}, pinned) == -1);
                assert(pinned.empty());
                return 0;
            }
            """
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            source = temporary / "orbit_tap_test.cc"
            executable = temporary / "orbit_tap_test"
            source.write_text(test_source, encoding="utf-8")
            (temporary / "sdkconfig.h").write_text(
                "#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1\n", encoding="utf-8"
            )
            subprocess.run(
                [
                    shutil.which("c++"),
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer",
                    "-I",
                    str(temporary),
                    "-I",
                    str(ROOT / "main"),
                    "-I",
                    str(BOARD_DIR),
                    str(source),
                    str(BOARD_DIR / "orbit_dial.cc"),
                    "-o",
                    str(executable),
                ],
                check=True,
                cwd=ROOT,
            )
            subprocess.run([str(executable)], check=True, cwd=ROOT)


if __name__ == "__main__":
    unittest.main()
