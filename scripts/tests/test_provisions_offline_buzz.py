"""Host checks for the once-per-transition OFFLINE buzz on the StopWatch."""

import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/m5stack/stopwatch/m5stack_stopwatch.cc"


def method(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def run_cpp(program: str) -> None:
    compiler = shutil.which("c++")
    if compiler is None:
        raise unittest.SkipTest("c++ compiler unavailable")
    with tempfile.TemporaryDirectory(prefix="orbit-offline-buzz-") as directory:
        source = Path(directory) / "buzz.cc"
        binary = Path(directory) / "buzz"
        source.write_text(program, encoding="utf-8")
        built = subprocess.run(
            [compiler, "-std=c++17", "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
             str(source), "-o", str(binary)],
            capture_output=True, text=True, check=False,
        )
        if built.returncode != 0:
            raise AssertionError(built.stderr)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10,
                                check=False)
        if result.returncode != 0:
            raise AssertionError(result.stdout + result.stderr)


class OfflineSignalTests(unittest.TestCase):
    def test_signal_fires_once_per_transition_and_is_armed_only_by_hello(self) -> None:
        run_cpp(
            r"""
#include <cassert>
#include "provisions_offline_signal.h"

int main() {
    provisions::OfflineSignal signal;
    // Cold boot without a gateway: the face shows OFFLINE, nothing buzzes.
    assert(!signal.armed());
    for (int i = 0; i < 5; ++i) assert(!signal.Offline());
    // First hello arms it and reports a recovery.
    assert(signal.Online());
    assert(signal.armed());
    assert(!signal.Online());  // a second hello while online is not a recovery
    // Loss: exactly one cue, then silence through every retry.
    assert(signal.Offline());
    for (int retry = 0; retry < 50; ++retry) assert(!signal.Offline());
    assert(!signal.armed());
    // Recovery re-arms; the next loss cues again.
    assert(signal.Online());
    assert(signal.Offline());
    assert(!signal.Offline());
}
"""
        )

    def test_actual_offline_notes_buzz_once_through_the_board(self) -> None:
        application = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        offline = method(application, "void Application::NoteProvisionsOffline()")
        online = method(application, "void Application::NoteProvisionsOnline()")
        buzz_ms = re.search(r"kProvisionsOfflineBuzzMs = (\d+);", application)
        self.assertIsNotNone(buzz_ms)
        self.assertLessEqual(int(buzz_ms.group(1)), 300)
        self.assertGreaterEqual(int(buzz_ms.group(1)), 60)
        run_cpp(
            r"""
#include <cassert>
#include <cstdint>
#include "provisions_offline_signal.h"

#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
constexpr uint32_t kProvisionsOfflineBuzzMs = __BUZZ_MS__;

struct Board {
    int pulses = 0;
    uint32_t last_ms = 0;
    bool has_motor = true;
    static Board& GetInstance() { static Board board; return board; }
    bool PulseHaptic(uint32_t duration_ms) {
        ++pulses;
        last_ms = duration_ms;
        return has_motor;
    }
};

struct Application {
    provisions::OfflineSignal provisions_offline_signal_;
    void NoteProvisionsOffline();
    void NoteProvisionsOnline();
};

__OFFLINE__

__ONLINE__

int main() {
    auto& board = Board::GetInstance();
    Application app;
    // Boot: OFFLINE notes before any hello are silent.
    app.NoteProvisionsOffline();
    app.NoteProvisionsOffline();
    assert(board.pulses == 0);
    // Hello, then a loss and its retries: one buzz of the configured length.
    app.NoteProvisionsOnline();
    for (int i = 0; i < 6; ++i) app.NoteProvisionsOffline();
    assert(board.pulses == 1);
    assert(board.last_ms == kProvisionsOfflineBuzzMs);
    // Recovery without a reboot, then a second loss: one more buzz.
    app.NoteProvisionsOnline();
    app.NoteProvisionsOnline();
    app.NoteProvisionsOffline();
    assert(board.pulses == 2);
    // A board without a motor still consumes the transition exactly once.
    board.has_motor = false;
    app.NoteProvisionsOnline();
    app.NoteProvisionsOffline();
    app.NoteProvisionsOffline();
    assert(board.pulses == 3);
}
""".replace("__OFFLINE__", offline).replace("__ONLINE__", online)
            .replace("__BUZZ_MS__", buzz_ms.group(1))
        )

    def test_every_gateway_loss_site_notes_offline_and_every_hello_re_arms(self) -> None:
        application = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        header = (ROOT / "main/application.h").read_text(encoding="utf-8")
        self.assertIn('#include "provisions_offline_signal.h"', header)
        self.assertIn("provisions::OfflineSignal provisions_offline_signal_;", header)

        # Preprocessor branches leave this handler's braces unbalanced in raw
        # text, so slice to the next definition instead of scanning braces.
        start = application.index("void Application::HandleNetworkDisconnectedEvent()")
        disconnected = application[start:application.index("\nvoid Application::", start + 1)]
        self.assertIn("NoteProvisionsOffline();", disconnected)
        self.assertLess(disconnected.index("NoteProvisionsOffline();"),
                        disconnected.index("CloseAudioChannel();"))

        closed = application[application.index("protocol->OnAudioChannelClosed(["):]
        closed = closed[:closed.index("protocol->OnIncomingJson(")]
        self.assertIn("NoteProvisionsOffline();", closed)

        maintenance = method(application, "void Application::HandleProvisionsGatewayMaintenance()")
        for marker in ("Provisions TTS turn timed out", "Provisions gateway response timed out",
                       "Provisions gateway heartbeat expired",
                       "Failed to send Provisions gateway heartbeat"):
            tail = maintenance[maintenance.index(marker):]
            self.assertIn("NoteProvisionsOffline();", tail[:tail.index("return;")], marker)
        self.assertLess(maintenance.index("if (GetProtocol()->OpenAudioChannel()) {"),
                        maintenance.index("NoteProvisionsOnline();"))
        self.assertIn("NoteProvisionsOffline();\n    provisions_gateway_rejections_ =", maintenance)
        # The Wi-Fi-not-yet-connected tick is not a transition; it must stay silent.
        wifi_wait = maintenance[maintenance.index("if (!network_connected_.load()) {"):]
        self.assertNotIn("NoteProvisionsOffline", wifi_wait[:wifi_wait.index("return;")])

        reconnect = method(application, "void Application::ReconnectVoiceGateway()")
        self.assertIn("app->NoteProvisionsOnline();", reconnect)
        self.assertIn("app->NoteProvisionsOffline();", reconnect)
        self.assertEqual(reconnect.count("NoteProvisionsOffline"), 1)

        press = method(application, "void Application::ContinueOpenAudioChannel(")
        self.assertIn("NoteProvisionsOffline();", press)
        self.assertIn("NoteProvisionsOnline();", press)

    def test_board_capability_defaults_to_a_no_op_without_a_motor(self) -> None:
        board = (ROOT / "main/boards/common/board.h").read_text(encoding="utf-8")
        default = method(board, "virtual bool PulseHaptic(uint32_t duration_ms)")
        self.assertIn("(void)duration_ms;", default)
        self.assertIn("return false;", default)
        # Core never reaches for a concrete board or a motor pin.
        application = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        self.assertNotIn("IOE_PIN_MOTOR", application)
        self.assertIn("Board::GetInstance().PulseHaptic(kProvisionsOfflineBuzzMs)", application)

    def test_stopwatch_pulses_the_ioe_motor_and_never_cuts_a_timer_alarm(self) -> None:
        board = BOARD.read_text(encoding="utf-8")
        alarm_output = board.split("SetTimerAlarmOutputCallback", 1)[1].split(
            "RegisterProvisionsTimerSnapshotCallback", 1)[0]
        self.assertIn("motor_alarm_active_.store(active);", alarm_output)
        self.assertIn("active ? HIGH : LOW", alarm_output)
        self.assertIn("InitializeHapticTimer();", board)
        self.assertIn('.name = "orbit_haptic"', board)
        pulse = method(board, "bool PulseHaptic(uint32_t duration_ms) override")
        release = method(board, "void EndHapticPulse()")
        run_cpp(
            r"""
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <vector>

#define HIGH 1
#define LOW 0
#define IOE_PIN_MOTOR 9
#define ESP_OK 0
using esp_timer_handle_t = int*;
int timer_slot = 0;
int start_result = ESP_OK;
int stops = 0;
uint64_t started_us = 0;
int esp_timer_stop(esp_timer_handle_t) { ++stops; return ESP_OK; }
int esp_timer_start_once(esp_timer_handle_t, uint64_t us) { started_us = us; return start_result; }

struct Ioe {
    std::vector<int> writes;
    void digitalWrite(int pin, int level) { assert(pin == IOE_PIN_MOTOR); writes.push_back(level); }
};

struct BoardUnderTest {
    Ioe ioe_;
    esp_timer_handle_t haptic_timer_ = &timer_slot;
    std::atomic<bool> motor_alarm_active_{false};
    std::atomic<bool> haptic_pulse_active_{false};
    __PULSE__
    __RELEASE__
};

int main() {
    // Plain pulse: HIGH now, LOW when the one-shot fires, exactly once.
    BoardUnderTest plain;
    assert(plain.PulseHaptic(120));
    assert(plain.ioe_.writes == std::vector<int>{HIGH});
    assert(started_us == 120000);
    assert(plain.PulseHaptic(120));  // re-entrant call is absorbed, no second HIGH
    assert(plain.ioe_.writes == std::vector<int>{HIGH});
    plain.EndHapticPulse();
    assert(plain.ioe_.writes == (std::vector<int>{HIGH, LOW}));
    plain.EndHapticPulse();  // a stale release never writes again
    assert(plain.ioe_.writes == (std::vector<int>{HIGH, LOW}));

    // Alarm already ringing: the pulse defers to it and touches nothing.
    BoardUnderTest ringing;
    ringing.motor_alarm_active_ = true;
    assert(ringing.PulseHaptic(120));
    assert(ringing.ioe_.writes.empty());

    // Alarm starts while the pulse runs: the release must not cut it.
    BoardUnderTest overlap;
    assert(overlap.PulseHaptic(120));
    overlap.motor_alarm_active_ = true;  // alarm callback drove HIGH itself
    overlap.EndHapticPulse();
    assert(overlap.ioe_.writes == std::vector<int>{HIGH});
    assert(!overlap.haptic_pulse_active_);

    // No timer or zero length: reports no haptic, writes nothing.
    BoardUnderTest missing;
    missing.haptic_timer_ = nullptr;
    assert(!missing.PulseHaptic(120));
    assert(missing.ioe_.writes.empty());
    BoardUnderTest zero;
    assert(!zero.PulseHaptic(0));
    assert(zero.ioe_.writes.empty());

    // Timer refuses to start: the pin is released and the pulse is not stuck.
    BoardUnderTest refused;
    start_result = -1;
    assert(!refused.PulseHaptic(120));
    assert(refused.ioe_.writes == (std::vector<int>{HIGH, LOW}));
    assert(!refused.haptic_pulse_active_);
    start_result = ESP_OK;
    assert(refused.PulseHaptic(120));
}
""".replace("__PULSE__", pulse.replace(" override", "")).replace("__RELEASE__", release)
        )


if __name__ == "__main__":
    unittest.main()
