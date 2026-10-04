"""Compile the real theme storage, toggle and blue-long-press dispatch."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_provisions_audio_boundaries import method

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/m5stack/stopwatch"


class OrbitMenuThemeTests(unittest.TestCase):
    def test_persistence_and_queued_gesture_priority(self):
        path = "main/boards/m5stack/stopwatch/m5stack_stopwatch.cc"
        toggle = method(path, "void ToggleMenuTheme()")
        gesture = method(path, "button2_.OnLongPress(") + ");"
        program = r'''
#include <atomic>
#include <cassert>
#include <deque>
#include <functional>
#include "menu_theme.h"
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
using namespace ProvisionsStopWatch;
struct RoundLcdDisplay {
    MenuTheme menu_theme_ = LoadMenuTheme();
    bool busy = false, menu_layout_ = true, alarm = false;
    int menu_page_ = 2, paints = 0, releases = 0, silenced = 0;
    bool Lock(int timeout) { assert(timeout == 400); return !busy; }
    void Unlock() { ++releases; }
    void ShowOrbitMenuLocked(int page) { assert(page == menu_page_); ++paints; }
    bool SilenceTimerAlarm() { if (!alarm) return false; alarm=false; ++silenced; return true; }
    __TOGGLE__
};
struct Application {
    bool menu=true, wifi=false, dictation=false, service=false, mode=false;
    int retries=0, aborts=0, pairs=0;
    std::deque<std::function<void()>> queue;
    static Application& GetInstance(){static Application app; return app;}
    bool IsOrbitMenuFace(){return menu;}
    bool IsOrbitModeChoice(){return mode;}
    bool IsOrbitService(){return service;}
    void OrbitServicePairing(){++pairs;}
    bool IsOrbitWifiSetup(){return wifi;}
    bool IsDictationScreen(){return dictation;}
    void RetrySavedVoiceRecording(){++retries;}
    void AbortAlarmListening(){++aborts;}
    void Schedule(std::function<void()> fn){queue.push_back(fn);}
    void Drain(){while(!queue.empty()){auto fn=queue.front();queue.pop_front();fn();}}
};
struct Button {std::function<void()> hold; void OnLongPress(std::function<void()> fn){hold=fn;}};
struct Board {
    Button button2_;
    RoundLcdDisplay display;
    RoundLcdDisplay* display_=&display;
    std::atomic<bool> orbit_locked_{false};
    bool chord=false;
    bool BlueGestureInChord(){return chord;}
    void ResetDisplayIdleTimer(){}
    void Register(){__GESTURE__}
};
int main(){
    assert(LoadMenuTheme()==MenuTheme::Crest);
    Settings::stored=42;assert(LoadMenuTheme()==MenuTheme::Crest);
    Settings::stored=0;
    auto& app=Application::GetInstance();Board board;board.Register();
    board.button2_.hold();
    assert(board.display.menu_theme_==MenuTheme::Crest && Settings::writes==0);
    app.Drain();assert(board.display.menu_theme_==MenuTheme::Nausicaa);
    assert(LoadMenuTheme()==MenuTheme::Nausicaa && board.display.paints==1);
    RoundLcdDisplay reboot;assert(reboot.menu_theme_==MenuTheme::Nausicaa);
    board.button2_.hold();app.Drain();assert(LoadMenuTheme()==MenuTheme::Crest);
    assert(board.display.paints==2 && board.display.releases==2 && app.retries==0);
    // A failed lock neither persists a change nor claims a repaint.
    board.display.busy=true;board.button2_.hold();app.Drain();
    assert(LoadMenuTheme()==MenuTheme::Crest && Settings::writes==2);
    board.display.busy=false;
    board.chord=true;board.button2_.hold();assert(app.queue.empty());board.chord=false;
    // State is rechecked when queued work runs, not when the button fires.
    board.button2_.hold();board.orbit_locked_=true;app.Drain();
    assert(Settings::writes==2);board.orbit_locked_=false;
    board.button2_.hold();board.display.alarm=true;app.Drain();
    assert(board.display.silenced==1 && app.aborts==1 && Settings::writes==2);
    app.wifi=true;board.button2_.hold();app.Drain();assert(Settings::writes==2);
    app.wifi=false;app.menu=false;app.dictation=true;
    int retries=app.retries;board.button2_.hold();app.Drain();assert(app.retries==retries);
    app.dictation=false;board.button2_.hold();app.Drain();assert(app.retries==retries+1);
    // Theme stays on the Chef menu; the separate mode chooser cannot trigger pairing.
    app.menu=true;app.dictation=true;
    const int writes=Settings::writes;board.button2_.hold();app.Drain();
    assert(Settings::writes==writes+1&&app.pairs==0);
    app.service=true;app.menu=false;app.mode=true;
    board.button2_.hold();app.Drain();assert(Settings::writes==writes+1&&app.pairs==0);
    app.mode=false;
    app.menu=false;board.button2_.hold();app.Drain();assert(app.pairs==1);
    board.display.alarm=true;board.button2_.hold();app.Drain();
    assert(app.pairs==1&&board.display.silenced==2);
    board.orbit_locked_=true;board.button2_.hold();app.Drain();assert(app.pairs==1);
}
'''.replace("__TOGGLE__", toggle).replace("__GESTURE__", gesture)
        with tempfile.TemporaryDirectory(prefix="orbit-menu-theme-") as directory:
            directory = Path(directory)
            (directory / "settings.h").write_text(r'''
#pragma once
#include <cassert>
#include <string>
struct Settings {
    static inline int stored=0,writes=0;
    bool writable;
    Settings(const std::string& ns,bool rw):writable(rw){assert(ns=="orbit_ui");}
    int GetInt(const std::string& key,int fallback){assert(key=="menu_theme"&&fallback==0);return stored;}
    void SetInt(const std::string& key,int value){assert(writable&&key=="menu_theme");stored=value;++writes;}
};
''')
            source, binary = directory / "test.cc", directory / "test"
            source.write_text(program)
            subprocess.run(["c++", "-std=c++17", "-fsanitize=address,undefined",
                            "-I", str(directory), "-I", str(BOARD), str(source),
                            "-o", str(binary)], check=True, capture_output=True, text=True)
            subprocess.run([str(binary)], check=True, capture_output=True, text=True)
