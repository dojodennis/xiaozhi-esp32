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


if __name__ == "__main__":
    unittest.main()
