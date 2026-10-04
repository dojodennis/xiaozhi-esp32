import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

from test_provisions_audio_boundaries import method, run_cpp


ROOT = Path(__file__).resolve().parents[2]
BOARD_DIR = ROOT / "main/boards/m5stack/stopwatch"


class ProvisionsButtonChordTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "host C++ compiler is unavailable")
    def test_chord_never_fires_single_button_actions(self):
        test_source = textwrap.dedent(
            r'''
            #include "button_chord.h"
            #include <cassert>

            using ProvisionsStopWatch::ButtonChord;
            using Edge = ButtonChord::Edge;
            constexpr int64_t W = ButtonChord::kWindowUs;

            int main() {
                {
                    // A plain Talk hold starts after the window and stops on release.
                    ButtonChord c;
                    assert(c.TalkDown(0) == Edge::kArmTalk);
                    assert(c.TalkWindowElapsed());
                    assert(!c.TalkWindowElapsed());  // Exactly one start.
                    assert(c.TalkUp());
                    assert(!c.SwallowBlueGesture());
                    assert(!c.TakeTalkClick());  // A hold is not a click.
                }
                {
                    // A tap shorter than the window never opens the microphone,
                    // but it is a click: the menu confirms on it, exactly once.
                    ButtonChord c;
                    assert(c.TalkDown(0) == Edge::kArmTalk);
                    assert(!c.TalkUp());
                    assert(!c.TalkWindowElapsed());
                    assert(c.TakeTalkClick());
                    assert(!c.TakeTalkClick());
                }
                {
                    // Talk then blue inside the window: chord, no Talk, blue swallowed.
                    ButtonChord c;
                    assert(c.TalkDown(0) == Edge::kArmTalk);
                    assert(c.BlueDown(W) == Edge::kChord);
                    assert(!c.TalkWindowElapsed());
                    assert(c.SwallowBlueGesture());
                    c.BlueUp();
                    assert(c.SwallowBlueGesture());  // The blue click fires after release.
                    assert(!c.TalkUp());
                    assert(!c.TakeTalkClick());  // A chord release is not a click.
                    assert(!c.BothHeld());
                }
                {
                    // Both still down: the lock may commit. A release ends it.
                    ButtonChord c;
                    assert(c.TalkDown(0) == Edge::kArmTalk);
                    assert(c.BlueDown(10) == Edge::kChord);
                    assert(c.BothHeld());
                    c.BlueUp();
                    assert(!c.BothHeld());
                    assert(!c.TalkUp());
                    // Next blue press on its own acts normally again.
                    assert(c.BlueDown(10 * W) == Edge::kNone);
                    assert(!c.SwallowBlueGesture());
                    c.BlueUp();
                }
                {
                    // Blue then Talk inside the window: chord either way round.
                    ButtonChord c;
                    assert(c.BlueDown(0) == Edge::kNone);
                    assert(c.TalkDown(W) == Edge::kChord);
                    assert(!c.TalkWindowElapsed());
                    assert(c.SwallowBlueGesture());
                    assert(!c.TalkUp());
                    c.BlueUp();
                    // A new Talk hold afterwards is ordinary.
                    assert(c.TalkDown(100 * W) == Edge::kArmTalk);
                    assert(c.TalkWindowElapsed());
                    assert(c.TalkUp());
                }
                {
                    // Outside the window both actions stay ordinary: a Talk hold
                    // that already started is not cancelled by a later blue press.
                    ButtonChord c;
                    assert(c.TalkDown(0) == Edge::kArmTalk);
                    assert(c.TalkWindowElapsed());
                    assert(c.BlueDown(W + 1) == Edge::kNone);
                    assert(!c.SwallowBlueGesture());
                    c.BlueUp();
                    assert(c.TalkUp());
                    assert(c.BlueDown(0) == Edge::kNone);
                    assert(c.TalkDown(W + 1) == Edge::kArmTalk);
                    assert(!c.SwallowBlueGesture());
                    c.BlueUp();
                    assert(c.TalkWindowElapsed());
                    assert(c.TalkUp());
                }
                {
                    // Re-pressing one button while the other is still held from a
                    // chord is not a second chord and not a single action.
                    ButtonChord c;
                    assert(c.TalkDown(0) == Edge::kArmTalk);
                    assert(c.BlueDown(10) == Edge::kChord);
                    assert(!c.TalkUp());
                    assert(c.TalkDown(20) == Edge::kNone);
                    assert(!c.TalkWindowElapsed());
                    assert(!c.TalkUp());
                    c.BlueUp();
                    assert(c.TalkDown(100 * W) == Edge::kArmTalk);
                }
                return 0;
            }
            '''
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            source = temporary / "button_chord_test.cc"
            executable = temporary / "button_chord_test"
            source.write_text(test_source, encoding="utf-8")
            sanitize = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            result = subprocess.run(
                [shutil.which("c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror", *sanitize,
                 "-I", str(BOARD_DIR), str(source), "-o", str(executable)],
                capture_output=True, text=True, cwd=ROOT,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            run = subprocess.run(
                [str(executable)], capture_output=True, text=True, cwd=ROOT, timeout=15,
                env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0" if sys.platform == "darwin" else "detect_leaks=1"},
            )
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    @unittest.skipUnless(shutil.which("c++"), "host C++ compiler is unavailable")
    def test_actual_short_chord_mode_choice_and_yellow_ownership(self):
        board = "main/boards/m5stack/stopwatch/m5stack_stopwatch.cc"
        source = (ROOT / board).read_text()
        production = "\n".join(method(board, signature) for signature in [
            "void ArmTalkStart(", "bool TalkReleased()", "bool TalkClicked()",
            "void BluePressed()", "void BlueReleased()", "void QueueOrbitModeChoice(",
            "bool BlueGestureInChord()", "void OnButtonChord()",
        ])
        callbacks = "\n".join(method(board, signature) + ");" for signature in [
            "button1_.OnPressDown([this]()", "button1_.OnPressUp([this]()",
            "button2_.OnPressDown([this]()", "button2_.OnPressUp([this]()",
            "button2_.OnDoubleClick([this]()", "button2_.OnLongPress([this]()",
            "button2_.OnClick([this]() {\n            if (BlueGestureInChord())",
        ])
        lock_timer = source.split("esp_timer_create_args_t lock_args = {", 1)[1]
        lock_timer = lock_timer.split(".callback =", 1)[1].split(".arg = this", 1)[0].strip().rstrip(",")
        wifi_timer = source.split("esp_timer_create_args_t wifi_args = {", 1)[1]
        wifi_timer = wifi_timer.split(".callback =", 1)[1].split(".arg = this", 1)[0].strip().rstrip(",")
        program = r'''
#include <atomic>
#include <cassert>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>
#include "__ROOT__/main/boards/m5stack/stopwatch/button_chord.h"
#include "__ROOT__/main/boards/m5stack/stopwatch/menu_wifi_hold.h"
#define CONFIG_PROVISIONS_LOCAL_CAPTURE 1
#define ESP_LOGW(...) ((void)0)
constexpr int ESP_OK=0;
int64_t now_us=0;
int timer_error=ESP_OK;
int64_t esp_timer_get_time() { return now_us; }
int esp_timer_stop(void*) { return ESP_OK; }
int esp_timer_start_once(void*, int64_t) { return timer_error; }
struct Application {
    bool menu=false, mode=false, setup=false, service=false;
    int starts=0, stops=0, confirms=0, choices=0, wifi_entries=0;
    int taps=0, blues=0, retries=0, pairs=0, aborts=0;
    std::vector<std::function<void()>> queue;
    static Application& GetInstance() { static Application app; return app; }
    void Schedule(std::function<void()> fn) { queue.push_back(std::move(fn)); }
    void Drain() {
        while(!queue.empty()) {
            auto batch=std::move(queue); queue.clear();
            for(auto& fn:batch) fn();
        }
    }
    bool IsOrbitMenuFace() { return menu; }
    bool IsOrbitModeChoice() { return mode; }
    bool IsOrbitWifiSetup() { return setup; }
    bool IsOrbitService() { return service; }
    bool IsDictationScreen() { return service; }
    void ShowOrbitModeChoice(std::function<bool()> allowed) {
        // Execute the board's token guard on the scheduled application owner.
        Schedule([this, allowed]() { if(allowed() && !setup) { ++choices; mode=true; } });
    }
    void StartOrbitWifiSetup(std::function<bool()> allowed) {
        Schedule([this, allowed]() {
            if(allowed() && (menu || mode) && !setup) { ++wifi_entries; setup=true; }
        });
    }
    bool ConfirmOrbitMenu() { if(!menu && !mode) return false; ++confirms; return true; }
    void OrbitServiceTap() { ++taps; }
    void NoteTalkPressDown(int64_t) {}
    void StartListening() { ++starts; }
    void StopListening() { ++stops; }
    void HandleOrbitMenuBlue() { ++blues; }
    void RetrySavedVoiceRecording() { ++retries; }
    void OrbitServicePairing() { ++pairs; }
    void AbortAlarmListening() { ++aborts; }
};
struct RoundLcdDisplay {
    bool alarm=false;
    int themes=0;
    bool HasTimerAlarm() { return alarm; }
    bool SilenceTimerAlarm() { if(!alarm) return false; alarm=false; return true; }
    void KeepTimerFaceAwake() {}
    void ToggleMenuTheme() { ++themes; }
};
struct Button {
    std::function<void()> down, up, click, twice, hold;
    void OnPressDown(std::function<void()> fn) { down=fn; }
    void OnPressUp(std::function<void()> fn) { up=fn; }
    void OnClick(std::function<void()> fn) { click=fn; }
    void OnDoubleClick(std::function<void()> fn) { twice=fn; }
    void OnLongPress(std::function<void()> fn) { hold=fn; }
};
struct M5StackStopwatchBoard {
    Button button1_, button2_;
    RoundLcdDisplay display;
    RoundLcdDisplay* display_=&display;
    std::atomic<bool> orbit_locked_{false};
    std::mutex chord_mutex_;
    ProvisionsStopWatch::ButtonChord chord_;
    ProvisionsStopWatch::MenuWifiHold wifi_hold_;
    std::function<void()> chord_talk_start_;
    void* chord_timer_=this;
    void* lock_timer_=this;
    void* wifi_hold_timer_=this;
    static constexpr int64_t kLockHoldUs=ProvisionsStopWatch::ButtonChord::kLockHoldUs;
    int locks=0;
    void CancelLockHold() { if(lock_timer_) esp_timer_stop(lock_timer_); }
    void ToggleOrbitLock() { orbit_locked_=!orbit_locked_.load(); ++locks; }
    void ResetDisplayIdleTimer() {}
    void Window() { if(chord_.TalkWindowElapsed()) chord_talk_start_(); }
    void FireLock() { auto callback=__LOCK_TIMER__; callback(this); }
    void FireWifi() { auto callback=__WIFI_TIMER__; callback(this); }
    void Register() { __CALLBACKS__ }
    __PRODUCTION__
};

int main() {
    auto& app=Application::GetInstance();
    // Both press/release orders, from either role: nothing fires until BOTH up.
    for(bool mode:{false,true}) for(bool service:{false,true}) for(bool yellow_first:{false,true})
    for(bool yellow_up_first:{false,true}) {
        app=Application{}; app.mode=mode; app.service=service;
        M5StackStopwatchBoard board; board.Register(); now_us=0;
        if(yellow_first) board.button1_.down(); else board.button2_.down();
        now_us=100000;
        if(yellow_first) board.button2_.down(); else board.button1_.down();
        board.Window();
        now_us=300000;
        if(yellow_up_first) board.button1_.up(); else board.button2_.up();
        app.Drain(); assert(app.choices==0);
        ++now_us;
        if(yellow_up_first) board.button2_.up(); else board.button1_.up();
        assert(app.choices==0); app.Drain(); assert(app.choices==1);
        board.button2_.click(); board.button2_.twice(); board.button2_.hold(); app.Drain();
        assert(app.starts==0 && app.stops==0 && app.confirms==0 && app.taps==0);
        assert(app.blues==0 && app.retries==0 && app.pairs==0 && board.display.themes==0);
        assert(board.chord_.TakeModeChoice()==0 && board.locks==0);
    }
    // At 600 ms the original hold still locks/unlocks exactly once, never chooses.
    for(bool locked:{false,true}) {
        app=Application{}; M5StackStopwatchBoard board; board.Register(); board.orbit_locked_=locked;
        now_us=0; board.button2_.down(); now_us=100000; board.button1_.down();
        now_us=699999; board.FireLock(); assert(board.locks==0);
        now_us=700000; board.FireLock(); board.FireLock();
        assert(board.locks==1 && board.orbit_locked_.load()!=locked);
        now_us=800000; board.button1_.up(); board.button2_.up(); app.Drain();
        assert(app.choices==0 && app.starts==0 && app.confirms==0);
    }
    // An exact-boundary release or a slow second release must not become a tap.
    for(bool delayed_second:{false,true}) {
        app=Application{}; M5StackStopwatchBoard board; board.Register();
        now_us=0; board.button1_.down(); now_us=100000; board.button2_.down();
        now_us=delayed_second?200000:700000; board.button1_.up();
        now_us=700000; board.button2_.up(); board.FireLock(); app.Drain();
        assert(app.choices==0 && board.locks==0 && app.starts==0 && app.confirms==0);
    }
    // Fresh presses, locks and alarms revoke already queued selector work.
    for(int revoke=0;revoke<4;++revoke) {
        app=Application{}; M5StackStopwatchBoard board; board.Register();
        now_us=0; board.button1_.down(); now_us=100000; board.button2_.down();
        now_us=200000; board.button1_.up(); board.button2_.up();
        if(revoke==0) { ++now_us; board.button1_.down(); }
        if(revoke==1) { ++now_us; board.button2_.down(); }
        if(revoke==2) board.orbit_locked_=true;
        if(revoke==3) board.display.alarm=true;
        app.Drain(); assert(app.choices==0);
    }
    // A partial release/repress cannot create a second action or revive an old timer.
    app=Application{};
    {
        M5StackStopwatchBoard board; board.Register();
        now_us=0; board.button1_.down(); now_us=100000; board.button2_.down();
        now_us=200000; board.button1_.up(); now_us=300000; board.button1_.down();
        now_us=900000; board.FireLock(); board.button1_.up(); board.button2_.up(); app.Drain();
        assert(app.choices==0 && board.locks==0 && app.starts==0 && app.confirms==0);
    }
    // A short chord while locked, or unavailable/failed timers, leaves actions closed.
    for(int failure=0;failure<3;++failure) {
        app=Application{}; M5StackStopwatchBoard board; board.Register();
        if(failure==0) board.orbit_locked_=true;
        if(failure==1) board.lock_timer_=nullptr;
        timer_error=failure==2?-1:ESP_OK;
        now_us=0; board.button2_.down(); now_us=100000; board.button1_.down();
        now_us=200000; board.button2_.up(); board.button1_.up(); app.Drain();
        assert(app.choices==0 && app.starts==0 && app.confirms==0 && board.locks==0);
    }
    timer_error=ESP_OK;
    // A late old timer cannot lock a new chord before its own 600 ms interval.
    app=Application{};
    {
        M5StackStopwatchBoard board; board.Register();
        now_us=0; board.button1_.down(); now_us=100000; board.button2_.down();
        now_us=200000; board.button1_.up(); board.button2_.up();
        now_us=300000; board.button2_.down(); now_us=400000; board.button1_.down();
        now_us=700000; board.FireLock(); app.Drain();
        assert(board.locks==0 && app.choices==0);
        now_us=1000000; board.FireLock(); assert(board.locks==1);
        board.button1_.up(); board.button2_.up(); app.Drain(); assert(app.choices==0);
    }
    // Short selector taps confirm on release; deliberate five-second holds open Wi-Fi.
    for(bool service:{false,true})
    for(int64_t duration:{100000LL,200000LL,4999999LL,5000000LL,6000000LL}) {
        app=Application{}; app.mode=true; app.service=service;
        M5StackStopwatchBoard board; board.Register(); now_us=0; board.button1_.down();
        if(duration>150000) { now_us=150000; board.Window(); }
        now_us=duration; board.FireWifi(); app.Drain();
        const bool wifi=duration>=5000000;
        assert(app.confirms==0 && app.starts==0 && app.taps==0 && app.wifi_entries==int(wifi));
        board.button1_.up(); app.Drain();
        assert(app.confirms==int(!wifi) && app.starts==0 && app.stops==0 && app.taps==0);
        board.FireWifi(); app.Drain(); assert(app.wifi_entries==int(wifi));
    }
    // Release/repress, blue, lock, alarm and leaving Mode revoke queued Wi-Fi.
    for(bool service:{false,true}) for(int revoke=0;revoke<6;++revoke) {
        app=Application{}; app.mode=true; app.service=service;
        M5StackStopwatchBoard board; board.Register(); now_us=0; board.button1_.down();
        now_us=150000; board.Window(); now_us=5000000; board.FireWifi();
        if(revoke==0) board.button1_.up();
        if(revoke==1) { board.button1_.up(); ++now_us; board.button1_.down(); }
        if(revoke==2) board.button2_.down();
        if(revoke==3) board.orbit_locked_=true;
        if(revoke==4) board.display.alarm=true;
        if(revoke==5) app.mode=false;
        app.Drain();
        assert(app.wifi_entries==0 && app.confirms==0 && app.starts==0 && app.taps==0);
    }
    // Mode owns blue holds; normal Service pairing and Chef-menu theme holds remain.
    for(bool service:{false,true}) {
        app=Application{}; app.mode=true; app.service=service;
        M5StackStopwatchBoard board; board.Register(); now_us=0; board.button2_.down();
        board.button2_.hold(); app.Drain();
        assert(app.pairs==0 && app.retries==0 && board.display.themes==0);
    }
    app=Application{}; app.service=true;
    {
        M5StackStopwatchBoard board; board.Register(); now_us=0; board.button2_.down();
        board.button2_.hold(); app.Drain(); assert(app.pairs==1 && board.display.themes==0);
    }
    app=Application{}; app.menu=true;
    {
        M5StackStopwatchBoard board; board.Register(); now_us=0; board.button2_.down();
        board.button2_.hold(); app.Drain(); assert(board.display.themes==1 && app.pairs==0);
    }
}
'''
        run_cpp(program.replace("__ROOT__", str(ROOT))
                .replace("__PRODUCTION__", production).replace("__CALLBACKS__", callbacks)
                .replace("__LOCK_TIMER__", lock_timer).replace("__WIFI_TIMER__", wifi_timer))

    @unittest.skipUnless(shutil.which("c++"), "host C++ compiler is unavailable")
    def test_mode_choice_owns_direct_and_queued_timer_taps(self):
        board = "main/boards/m5stack/stopwatch/m5stack_stopwatch.cc"
        touch = method(board, "void InitializeTouch()")
        menu = "const bool menu =" + touch.split("const bool menu =", 1)[1].split(";", 1)[0] + ";"
        tap = method(board, "if (!pressed && self->touch_was_pressed_")
        program = r'''
#include <atomic>
#include <cassert>
#include <functional>
#include <utility>
#include <vector>
struct Application {
    bool menu=false, mode=false;
    std::vector<std::function<void()>> queue;
    static Application& GetInstance() { static Application app; return app; }
    bool IsOrbitMenuFace() { return menu; }
    bool IsOrbitModeChoice() { return mode; }
    void Schedule(std::function<void()> fn) { queue.push_back(std::move(fn)); }
    void Drain() { auto batch=std::move(queue); queue.clear(); for(auto& fn:batch) fn(); }
};
struct Display {
    bool alarm=false;
    unsigned toggles=0;
    bool HasTimerAlarm() { return alarm; }
    bool ToggleTimerFocus() { ++toggles; return true; }
};
struct Board {
    bool touch_was_pressed_=true, touch_swiped_=false, touch_alarm_press_=false;
    std::atomic<bool> touch_dismiss_queued_{false}, orbit_locked_{false};
    Display display;
    Display* display_=&display;
    unsigned wakes=0;
    void ResetDisplayIdleTimer() { ++wakes; }
    void TapRelease() {
        auto* self=this;
        const bool pressed=false, shopping=false, notes=false;
        __MENU__
        __TAP__
    }
};
int main() {
    auto& app=Application::GetInstance();
    for(bool mode:{false,true}) {
        app=Application{}; app.mode=mode; app.menu=!mode;
        Board board; board.TapRelease();
        assert(app.queue.empty() && !board.touch_dismiss_queued_);
        app.Drain(); assert(board.display.toggles==0);
    }
    // Timer tap queued before a chooser/menu paint cannot take ownership afterward.
    for(int takeover=0;takeover<4;++takeover) {
        app=Application{}; Board board; board.TapRelease(); assert(app.queue.size()==1);
        if(takeover==0) app.mode=true;
        if(takeover==1) app.menu=true;
        if(takeover==2) board.display.alarm=true;
        if(takeover==3) board.orbit_locked_=true;
        app.Drain(); assert(board.display.toggles==0 && !board.touch_dismiss_queued_);
    }
    // An alarm-consumed press stays spent; the ordinary timer tap still focuses.
    app=Application{};
    {
        Board board; board.touch_alarm_press_=true; board.TapRelease(); app.Drain();
        assert(board.display.toggles==0);
    }
    {
        Board board; board.TapRelease(); app.Drain();
        assert(board.display.toggles==1 && board.wakes==1);
    }
}
'''
        run_cpp(program.replace("__MENU__", menu).replace("__TAP__", tap))

    def test_board_wires_mode_choice_and_lock_and_documents_gestures(self):
        board = (BOARD_DIR / "m5stack_stopwatch.cc").read_text(encoding="utf-8")
        readme = (BOARD_DIR / "README.md").read_text(encoding="utf-8")
        self.assertIn('#include "button_chord.h"', board)
        self.assertIn("button2_.OnPressDown", board)
        self.assertIn("button2_.OnPressUp", board)
        self.assertIn("ButtonChord::kWindowUs", board)
        self.assertIn("ShowOrbitModeChoice", board)
        self.assertIn("CommitLockHold", board)
        # A yellow click (release inside the window) confirms the menu without
        # opening the microphone; only a started capture is stopped.
        release = board.split("button1_.OnPressUp", 1)[1].split("button2_.OnPressDown", 1)[0]
        self.assertIn("if (TalkReleased()) {", release)
        self.assertIn("StopListening();", release)
        self.assertIn("TalkClicked()", release)
        self.assertIn("!orbit_locked_.load()", release)
        self.assertIn("ConfirmOrbitMenu();", release)
        self.assertNotIn("StartListening", release)
        self.assertIn("kTimerFaceIdleUs = 30LL * 1000 * 1000", board)
        # Every blue gesture checks the chord before silencing, dictation, retry or volume.
        buttons = board.split("void InitializeButtons()", 1)[1].split("button1_.OnPressDown", 1)[1]
        buttons = buttons.split("#else\n        // Button1", 1)[0]
        for gesture in ("button2_.OnClick", "button2_.OnDoubleClick", "button2_.OnLongPress"):
            body = buttons.split(gesture, 1)[1].split("});", 1)[0]
            self.assertIn("BlueGestureInChord()", body, gesture)
        self.assertIn("Two-button chord", readme)
        self.assertIn("| Talk + blue together", readme)


if __name__ == "__main__":
    unittest.main()
