import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CJSON_DIR = ROOT / "managed_components/espressif__cjson/cJSON"
BOARD = ROOT / "main/boards/m5stack/stopwatch/m5stack_stopwatch.cc"


class ProvisionsTimerDismissalTests(unittest.TestCase):
    def test_application_reports_dismissals_and_routes_acks_before_the_player(self):
        application = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        header = (ROOT / "main/application.h").read_text(encoding="utf-8")
        cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
        board = BOARD.read_text(encoding="utf-8")

        self.assertIn('"provisions_timer_dismissal.cc"', cmake)
        self.assertIn("provisions::timers::Dismissals timer_dismissals_", header)
        self.assertIn("void DismissDueTimers();", header)
        service = application.split("void Application::ServiceTimers()", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("timer_dismissals_.Service(session, negotiated", service)
        self.assertIn("timer_dismissals_.Filter(snapshot)", service)
        # The dial and the text line both see the filtered snapshot.
        self.assertLess(service.index("timer_dismissals_.Filter(snapshot)"),
                        service.index("SetTimerText("))
        self.assertLess(service.index("timer_dismissals_.Filter(snapshot)"),
                        service.index("timer_dial_link_.Reconcile("))
        incoming = application.split("const bool timer_frame =", 1)[1].split(
            "timer_player_.OnJson(", 1)[0]
        self.assertIn('"dismiss_ack"', incoming)
        self.assertIn("timer_dismissals_.OnAck(root, protocol->session_id())", incoming)
        dismiss = application.split("void Application::DismissDueTimers()", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("TimersNegotiated()", dismiss)
        self.assertIn("timer_dismissals_.Dismiss(", dismiss)
        self.assertIn("esp_fill_random", dismiss)
        # The takeover only covered the face; dismissal must repaint it, or a
        # status frozen before the alarm ("Please try again") is uncovered.
        self.assertIn("GetDeviceState() == kDeviceStateIdle", dismiss)
        self.assertIn("SetStatus(GetProvisionsIdleStatus())", dismiss)

        silence = board.split("bool SilenceTimerAlarm()", 1)[1].split("bool HasTimerAlarm()", 1)[0]
        self.assertIn("timer_alarm_state_.silenced()", silence)
        self.assertIn("DismissDueTimersLocked()", silence)
        self.assertIn("timer_dismiss_callback_()", silence)
        self.assertIn("Application::GetInstance().DismissDueTimers()", board)
        self.assertNotIn("timer.acknowledge", board)

    @unittest.skipUnless(
        shutil.which("c++") and shutil.which("cc"),
        "host C and C++ compilers are unavailable",
    )
    def test_dismiss_frames_retries_acks_and_suppression(self):
        test_source = textwrap.dedent(
            r'''
            #include "provisions_timer_dismissal.h"
            #include <cJSON.h>
            #include <cassert>
            #include <string>
            #include <vector>

            using provisions::timers::Dismissals;
            using provisions::timers::Snapshot;
            using provisions::timers::State;
            using provisions::timers::Timer;

            const std::string kSession = "11111111-1111-4111-8111-111111111111";
            const std::string kOther = "22222222-2222-4222-8222-222222222222";
            const std::string kTimerA = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
            const std::string kTimerB = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
            constexpr int64_t kNow = 1900000000000;

            Timer Make(const std::string& id, int64_t deadline, uint64_t revision, State state) {
                Timer t; t.id = id; t.spoken_number = 1; t.label = "Rice";
                t.deadline_ms = deadline; t.revision = revision; t.state = state; return t;
            }
            Snapshot Snap(const std::string& session, std::vector<Timer> timers) {
                Snapshot s; s.session_id = session; s.request_id = "r"; s.timers = std::move(timers); return s;
            }
            int ids = 0;
            std::string NextId() {
                std::array<uint8_t, 16> bytes{}; bytes[15] = static_cast<uint8_t>(++ids);
                return Dismissals::FormatUuidV4(bytes);
            }
            struct Wire {
                std::vector<std::string> frames;
                bool ok = true;
                bool operator()(const std::string& f) { frames.push_back(f); return ok; }
            };
            cJSON* Parse(const std::string& text) { auto* r = cJSON_Parse(text.c_str()); assert(r); return r; }
            std::string Str(cJSON* r, const char* k) { return cJSON_GetObjectItemCaseSensitive(r, k)->valuestring; }
            std::string Ack(const std::string& session, const std::string& timer, const std::string& request,
                            const char* status = "settled") {
                return std::string("{\"type\":\"timer\",\"action\":\"dismiss_ack\",\"session_id\":\"") + session +
                       "\",\"timer_id\":\"" + timer + "\",\"request_id\":\"" + request +
                       "\",\"status\":\"" + status + "\"}";
            }
            bool OnAck(Dismissals& d, const std::string& text, const std::string& session) {
                auto* r = Parse(text); const bool ok = d.OnAck(r, session); cJSON_Delete(r); return ok;
            }

            int main() {
                assert(Dismissals::FormatUuidV4({}) == "00000000-0000-4000-8000-000000000000");
                {
                    // Exact contract frame, one per due timer; a future timer is untouched.
                    Dismissals d; Wire wire;
                    auto snap = Snap(kSession, {Make(kTimerA, kNow - 1000, 7, State::Expired),
                                                Make(kTimerB, kNow + 60000, 3, State::Active)});
                    assert(d.Dismiss(snap, kSession, true, kNow, 0, NextId) == 1);
                    d.Service(kSession, true, 0, std::ref(wire));
                    assert(wire.frames.size() == 1);
                    auto* f = Parse(wire.frames[0]);
                    assert(cJSON_GetArraySize(f) == 6);
                    assert(Str(f, "session_id") == kSession && Str(f, "type") == "timer");
                    assert(Str(f, "action") == "dismiss" && Str(f, "timer_id") == kTimerA);
                    assert(cJSON_GetObjectItemCaseSensitive(f, "revision")->valuedouble == 7);
                    const std::string request = Str(f, "request_id");
                    assert(request.size() == 36 && request[14] == '4');
                    cJSON_Delete(f);

                    // The dismissed timer is hidden; the future timer stays.
                    auto shown = snap; d.Filter(shown);
                    assert(shown.timers.size() == 1 && shown.timers[0].id == kTimerB);

                    // No ack within 5 s: same frame (same request_id), at most twice more.
                    d.Service(kSession, true, 4999999, std::ref(wire)); assert(wire.frames.size() == 1);
                    d.Service(kSession, true, 5000000, std::ref(wire)); assert(wire.frames.size() == 2);
                    assert(wire.frames[1] == wire.frames[0]);
                    d.Service(kSession, true, 10000000, std::ref(wire)); assert(wire.frames.size() == 3);
                    assert(wire.frames[2] == wire.frames[0]);
                    d.Service(kSession, true, 15000000, std::ref(wire)); assert(wire.frames.size() == 3);
                    assert(d.PendingCount() == 0);
                    d.Service(kSession, true, 60000000, std::ref(wire)); assert(wire.frames.size() == 3);

                    // Still suppressed while snapshots repeat the same revision.
                    shown = snap; d.Filter(shown); assert(shown.timers.size() == 1);
                    // A disconnected (empty) snapshot proves nothing.
                    Snapshot empty; d.Filter(empty);
                    shown = snap; d.Filter(shown); assert(shown.timers.size() == 1);
                    // A higher revision releases it.
                    auto bumped = Snap(kSession, {Make(kTimerA, kNow + 300000, 8, State::Active)});
                    d.Filter(bumped); assert(bumped.timers.size() == 1);
                    shown = snap; d.Filter(shown); assert(shown.timers.size() == 2);
                }
                {
                    // An ack stops retries; malformed acks are rejected, unknown ones accepted.
                    Dismissals d; Wire wire;
                    auto snap = Snap(kSession, {Make(kTimerA, kNow - 1, 2, State::Active)});
                    assert(d.Dismiss(snap, kSession, true, kNow, 0, NextId) == 1);
                    d.Service(kSession, true, 0, std::ref(wire));
                    auto* f = Parse(wire.frames[0]); const std::string request = Str(f, "request_id"); cJSON_Delete(f);
                    assert(!OnAck(d, Ack(kOther, kTimerA, request), kSession));
                    assert(!OnAck(d, Ack(kSession, kTimerA, request, "done"), kSession));
                    assert(!OnAck(d, "{\"type\":\"timer\",\"action\":\"dismiss_ack\"}", kSession));
                    assert(d.PendingCount() == 1);
                    assert(OnAck(d, Ack(kSession, kTimerA, "33333333-3333-4333-8333-333333333333"), kSession));
                    assert(d.PendingCount() == 1);
                    assert(OnAck(d, Ack(kSession, kTimerA, request, "unavailable"), kSession));
                    assert(d.PendingCount() == 0);
                    d.Service(kSession, true, 5000000, std::ref(wire)); assert(wire.frames.size() == 1);
                    // The snapshot that follows no longer contains it: suppression ends.
                    auto after = Snap(kSession, {}); d.Filter(after);
                    auto again = snap; d.Filter(again); assert(again.timers.size() == 1);
                    assert(OnAck(d, Ack(kSession, kTimerA, request), kSession));  // Late duplicate.
                }
                {
                    // Not negotiated, no session, or another session's snapshot: nothing extra.
                    Dismissals d; Wire wire;
                    auto snap = Snap(kSession, {Make(kTimerA, kNow - 1, 2, State::Expired)});
                    assert(d.Dismiss(snap, kSession, false, kNow, 0, NextId) == 0);
                    assert(d.Dismiss(snap, "", true, kNow, 0, NextId) == 0);
                    assert(d.Dismiss(snap, kOther, true, kNow, 0, NextId) == 0);
                    d.Service(kSession, true, 0, std::ref(wire));
                    assert(wire.frames.empty() && d.PendingCount() == 0);
                    auto shown = snap; d.Filter(shown); assert(shown.timers.size() == 1);
                }
                {
                    // While disconnected nothing is sent or counted; the reconnect sends
                    // the same request under the new session. Repeat gestures do not duplicate.
                    Dismissals d; Wire wire;
                    auto snap = Snap(kSession, {Make(kTimerA, kNow - 1, 2, State::Expired)});
                    assert(d.Dismiss(snap, kSession, true, kNow, 0, NextId) == 1);
                    assert(d.Dismiss(snap, kSession, true, kNow, 0, NextId) == 1);
                    assert(d.PendingCount() == 1);
                    d.Service("", false, 0, std::ref(wire)); assert(wire.frames.empty());
                    d.Service(kOther, true, 100, std::ref(wire)); assert(wire.frames.size() == 1);
                    auto* f = Parse(wire.frames[0]); assert(Str(f, "session_id") == kOther); cJSON_Delete(f);
                    // A failed send still consumes an attempt and retries after the timeout.
                    wire.ok = false;
                    d.Service(kOther, true, 5000100, std::ref(wire)); assert(wire.frames.size() == 2);
                }
                return 0;
            }
            '''
        )
        if not (CJSON_DIR / "cJSON.c").exists():
            raise unittest.SkipTest("Prepare the canonical firmware dependencies first")
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            source = temporary / "timer_dismissal_test.cc"
            cjson_object = temporary / "cJSON.o"
            executable = temporary / "timer_dismissal_test"
            source.write_text(test_source, encoding="utf-8")
            sanitize = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run(
                [shutil.which("cc"), "-std=c11", *sanitize, "-I", str(CJSON_DIR),
                 "-c", str(CJSON_DIR / "cJSON.c"), "-o", str(cjson_object)],
                check=True, cwd=ROOT,
            )
            result = subprocess.run(
                [shutil.which("c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-Wno-deprecated-declarations", "-pthread", *sanitize,
                 "-I", str(ROOT / "main"), "-I", str(CJSON_DIR),
                 str(source), str(ROOT / "main/provisions_timer_dismissal.cc"),
                 str(ROOT / "main/provisions_timers.cc"), str(cjson_object),
                 "-o", str(executable)],
                capture_output=True, text=True, cwd=ROOT,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            run = subprocess.run(
                [str(executable)], capture_output=True, text=True, cwd=ROOT, timeout=15,
                env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0" if sys.platform == "darwin" else "detect_leaks=1"},
            )
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
