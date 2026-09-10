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


class ProvisionsTimerDialLinkTests(unittest.TestCase):
    def test_application_feeds_dial_from_player_and_guards_phone_source(self):
        application = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        header = (ROOT / "main/application.h").read_text(encoding="utf-8")
        cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn("provisions::timers::DialLink timer_dial_link_", header)
        self.assertIn('"provisions_timer_dial_link.cc"', cmake)
        service = application.split("void Application::ServiceTimers()", 1)[1].split(
            "\n}\n", 1
        )[0]
        self.assertIn("SetTimerText(", service)
        self.assertIn("timer_dial_link_.Reconcile(snapshot, session, negotiated", service)
        self.assertIn("callback = provisions_timer_snapshot_callback_", service)
        gate = application.split('strcmp(state->valuestring, "timer_snapshot") == 0', 1)[1].split(
            'strcmp(state->valuestring, "heartbeat")', 1
        )[0]
        self.assertIn("TimersNegotiated()", gate)
        self.assertLess(gate.index("TimersNegotiated()"), gate.index("Schedule("))

    @unittest.skipUnless(
        shutil.which("c++") and shutil.which("cc"),
        "host C and C++ compilers are unavailable",
    )
    def test_link_maps_states_and_repaints_only_on_change(self):
        test_source = textwrap.dedent(
            r'''
            #include "provisions_timer_dial_link.h"

            #include <cassert>
            #include <functional>
            #include <string>
            #include <vector>

            using provisions::timers::DialLink;
            using provisions::timers::Snapshot;
            using provisions::timers::State;
            using provisions::timers::Timer;
            using ProvisionsTimerSnapshot::TimerStatus;
            using ProvisionsTimerSnapshot::Update;

            const std::string kSession = "11111111-1111-4111-8111-111111111111";
            constexpr int64_t kNow = 1900000000000;

            Timer Make(const std::string& id, uint32_t number, const std::string& label,
                       int64_t deadline, State state) {
                Timer timer;
                timer.id = id;
                timer.spoken_number = number;
                timer.label = label;
                timer.deadline_ms = deadline;
                timer.revision = 1;
                timer.state = state;
                return timer;
            }

            struct Consumer {
                std::vector<Update> received;
                DialLink link;
                bool Tick(const Snapshot& snapshot, const std::string& session,
                          bool negotiated, int64_t now) {
                    Update update;
                    const bool changed = link.Reconcile(snapshot, session, negotiated, now, update);
                    if (changed) received.push_back(update);
                    return changed;
                }
            };

            int main() {
                Consumer consumer;
                Snapshot snapshot;
                snapshot.session_id = kSession;
                snapshot.timers.push_back(Make("t1", 1, "Pasta", kNow + 600000, State::Active));
                snapshot.timers.push_back(Make("t2", 2, "Sauce", kNow - 1000, State::Expired));

                // Two timers reach the dial as one apply with the mapped statuses.
                assert(consumer.Tick(snapshot, kSession, true, kNow));
                assert(consumer.received.size() == 1);
                const auto& apply = consumer.received.back();
                assert(apply.kind == Update::Kind::kSnapshot);
                assert(apply.snapshot.session_id == kSession);
                assert(apply.snapshot.revision == 1);
                assert(apply.snapshot.server_now_ms == kNow);
                assert(apply.snapshot.timers.size() == 2);
                assert(apply.snapshot.timers[0].id == "t1");
                assert(apply.snapshot.timers[0].label == "Pasta");
                assert(apply.snapshot.timers[0].deadline_ms == kNow + 600000);
                assert(apply.snapshot.timers[0].status == TimerStatus::kActive);
                assert(apply.snapshot.timers[1].id == "t2");
                assert(apply.snapshot.timers[1].status == TimerStatus::kAttention);

                // Unchanged content on later ticks never repaints, even as time moves.
                assert(!consumer.Tick(snapshot, kSession, true, kNow + 1000));
                assert(!consumer.Tick(snapshot, kSession, true, kNow + 2000));
                assert(consumer.received.size() == 1);

                // A state change (active -> ringing) repaints with a higher revision.
                snapshot.timers[0].state = State::Expired;
                assert(consumer.Tick(snapshot, kSession, true, kNow + 3000));
                assert(consumer.received.size() == 2);
                assert(consumer.received.back().snapshot.revision == 2);
                assert(consumer.received.back().snapshot.timers[0].status ==
                       TimerStatus::kAttention);

                // Acknowledged/cancelled timers vanish from the player snapshot: removed.
                snapshot.timers.erase(snapshot.timers.begin());
                assert(consumer.Tick(snapshot, kSession, true, kNow + 4000));
                assert(consumer.received.back().snapshot.timers.size() == 1);
                assert(consumer.received.back().snapshot.timers[0].id == "t2");

                // An empty snapshot yields exactly one reset, then silence.
                Snapshot empty;
                empty.session_id = kSession;
                assert(consumer.Tick(empty, kSession, true, kNow + 5000));
                assert(consumer.received.back().kind == Update::Kind::kReset);
                assert(!consumer.Tick(empty, kSession, true, kNow + 6000));
                assert(!consumer.Tick(empty, kSession, true, kNow + 7000));
                assert(consumer.received.size() == 4);

                // Nothing applied yet: losing negotiation or the session emits nothing.
                Consumer idle;
                assert(!idle.Tick(snapshot, kSession, false, kNow));
                assert(!idle.Tick(snapshot, "other", true, kNow));
                assert(!idle.Tick(snapshot, "", true, kNow));
                assert(!idle.Tick(snapshot, kSession, true, 0));
                assert(idle.received.empty());

                // Applied, then a session change resets once and the next session
                // restarts the revision sequence.
                Consumer roaming;
                assert(roaming.Tick(snapshot, kSession, true, kNow));
                assert(roaming.Tick(snapshot, "other", true, kNow));
                assert(roaming.received.back().kind == Update::Kind::kReset);
                snapshot.session_id = "other";
                assert(roaming.Tick(snapshot, "other", true, kNow));
                assert(roaming.received.back().kind == Update::Kind::kSnapshot);
                assert(roaming.received.back().snapshot.session_id == "other");
                assert(roaming.received.back().snapshot.revision == 1);

                // Losing negotiation or the trusted clock while applied resets once.
                Consumer dropped;
                assert(dropped.Tick(snapshot, "other", true, kNow));
                assert(dropped.Tick(snapshot, "other", false, kNow));
                assert(dropped.received.back().kind == Update::Kind::kReset);
                assert(!dropped.Tick(snapshot, "other", false, kNow));
                assert(dropped.Tick(snapshot, "other", true, kNow));
                assert(dropped.Tick(snapshot, "other", true, 0));
                assert(dropped.received.back().kind == Update::Kind::kReset);
                return 0;
            }
            '''
        )
        if not (CJSON_DIR / "cJSON.c").exists():
            raise unittest.SkipTest("Prepare the canonical firmware dependencies first")
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            source = temporary / "timer_dial_link_test.cc"
            cjson_object = temporary / "cJSON.o"
            executable = temporary / "timer_dial_link_test"
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
                 str(source), str(ROOT / "main/provisions_timer_dial_link.cc"),
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
