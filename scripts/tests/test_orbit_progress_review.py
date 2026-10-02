"""Exercise production status methods; LVGL calls are observed, no hardware used."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/m5stack/stopwatch"


def method(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end].replace(" override", "")


COMMON = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <string>
#include "crest_motion.h"
int locked = 0;
struct DisplayLockGuard {
    template<typename T> explicit DisplayLockGuard(T*) { assert(locked++ == 0); }
    ~DisplayLockGuard() { assert(--locked == 0); }
};
void esp_timer_stop(void*) {}
'''

STOPWATCH = r'''
using State = OrbitCrest::State;
State crest_state_ = State::Idle;
OrbitCrest::Progress crest_progress_ = OrbitCrest::Progress::Command;
std::atomic<VisualState> resting_state_{VisualState::kReady};
std::atomic<bool> receipt_visible_{false}, reply_visible_{false};
bool shopping_focus_layout_=false, menu_layout_=false, crest_timer_active_=false,
     orbit_locked_ui_=false, dictation_saving_=false;
void* notification_timer_ = nullptr;
const char* crest_result_caption_ = "";
std::string caption;
int renders = 0;
std::chrono::system_clock::time_point last_status_update_time_;
void *status_label_=this, *notification_label_=this, *emoji_label_=this, *emoji_box_=this,
     *hint_label_=this, *hero_halo_=this, *hint_panel_=this, *reply_header_label_=this,
     *reply_panel_=this, *reply_label_=this;
struct Presentation { uint32_t color=0; const char *title="", *icon="", *hint=""; };
Presentation PresentationFor(VisualState) { return {}; }
void ClearReplyLocked() { assert(locked); }
void ApplyChromeLocked(VisualState, Presentation) { assert(locked); }
void ClearCrestResultLocked() { assert(locked); crest_result_caption_=""; }
void RenderCrestLocked() { assert(locked); ++renders; caption=OrbitCrest::Caption(crest_state_,crest_progress_); }
void ChangeCrestStateLocked(State state) { crest_state_=state; RenderCrestLocked(); }
void SetReplyLayoutLocked(bool) {}
void CancelVisualReset() {}
bool TimerFaceForced() { return false; }
void DismissSpokenFaceLocked() { receipt_visible_=false; }
void SetCrestResultLocked(const char*, int) { ChangeCrestStateLocked(State::Result); }
void RestartReplyFromTop() {}
bool ScheduleVisualReset(int) { return true; }
void ApplyRestingVisualState() { ApplyVisualState(resting_state_); }
'''

C152 = r'''
using State = OrbitCrest::State;
State state_ = State::Idle;
OrbitCrest::Progress progress_ = OrbitCrest::Progress::Command;
bool reply_received_=false, speech_seen_=false;
const char* result_caption_ = "";
uint32_t result_started_ms_=0, result_hold_ms_=0;
std::chrono::system_clock::time_point last_status_update_time_;
std::string caption;
int renders=0;
uint32_t NowMs() { return 100; }
void ClearResultLocked() { assert(locked); result_caption_=""; reply_received_=false; }
void RenderLocked() { assert(locked); ++renders; caption=state_==State::Result ? result_caption_ : OrbitCrest::Caption(state_,progress_); }
void ChangeStateLocked(State state) { state_=state; RenderLocked(); }
'''

CASES = r'''
template<typename T> void progress_cases() {
    T display;
    display.SetStatus("Listening"); assert(display.caption.empty());
    display.SetStatus("Working"); assert(display.caption=="Thinking");
    display.SetStatus("12:34"); assert(display.caption=="Thinking");
    display.SetStatus("Saving"); assert(display.caption=="Saving");
    display.SetStatus("Retry queued"); assert(display.caption=="Retry queued");
    display.SetStatus("Preparing microphone"); assert(display.caption=="Preparing mic");
    display.SetStatus("Working"); assert(display.caption=="Thinking");
    int renders=display.renders;
    display.SetStatus("Working"); assert(display.renders==renders+1);
    // A new press interrupts waiting; cancellation/timeout never leave Thinking.
    display.SetStatus("Listening"); assert(display.caption.empty());
    display.SetStatus("Ready"); assert(display.caption.empty());
    display.SetStatus("Working"); display.SetStatus("Unavailable");
    assert(display.caption=="Please try again");
    display.SetStatus("Working"); display.SetStatus("Connecting");
    assert(display.caption=="Connecting");
    display.SetStatus("Working"); display.SetStatus("Speaking");
    assert(display.caption.empty());
    display.SetStatus("Ready"); assert(display.caption.empty());
}
int main() {
    progress_cases<Watch>(); progress_cases<Bench>();
    Watch watch;
    watch.dictation_saving_=true;
    watch.SetStatus("Listening"); assert(watch.caption=="Saving");
    watch.dictation_saving_=false;
    watch.SetStatus("Working"); assert(watch.caption=="Thinking");
    Bench bench;
    bench.SetStatus("Working"); bench.reply_received_=true;
    bench.SetStatus("Ready"); assert(bench.caption=="Reply received");
    assert(locked==0);
}
'''


@unittest.skipUnless(shutil.which("c++"), "host C++ compiler unavailable")
class ProgressReview(unittest.TestCase):
    def test_production_status_transitions_and_immediate_caption_refresh(self):
        watch = (BOARD / "m5stack_stopwatch.cc").read_text()
        bench = (BOARD / "crest_display.h").read_text()
        watch_methods = "\n".join(method(watch, signature) for signature in [
            "static bool IsClockStatus", "static VisualState StateForStatus",
            "static OrbitCrest::State CrestStateFor", "void ApplyVisualStateLocked",
            "void ApplyVisualState(VisualState", "void SetStatus(const char*",
        ])
        bench_methods = "\n".join(method(bench, signature) for signature in [
            "static bool IsClockStatus", "static State StateForStatus", "void SetStatus(const char*",
        ])
        lang = sorted(set(re.findall(r"Lang::Strings::(\w+)", watch_methods + bench_methods)))
        strings = "namespace Lang::Strings {" + "".join(
            f'constexpr const char* {name} = "lang:{name}";' for name in lang
        ) + "}\n"
        enum = method(watch, "enum class VisualState") + ";"
        stubs = r'''
using lv_color_t=uint32_t;
uint32_t lv_color_hex(uint32_t color) { return color; }
constexpr uint32_t kColorCream=0;
constexpr int LV_OBJ_FLAG_HIDDEN=1, kReplyHoldAfterSpeechMs=12000;
void lv_label_set_text(void*,const char*){}
void lv_obj_set_style_text_color(void*,uint32_t,int){}
void lv_obj_remove_flag(void*,int){}
void lv_obj_add_flag(void*,int){}
'''
        program = (COMMON + strings + stubs + "struct Watch {\n" + enum + STOPWATCH +
                   watch_methods + "\n};\nstruct Bench {\n" + C152 + bench_methods + "\n};\n" + CASES)
        with tempfile.TemporaryDirectory() as folder:
            source, binary = Path(folder) / "progress.cc", Path(folder) / "progress"
            source.write_text(program)
            result = subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                     "-fsanitize=address,undefined", "-I", str(BOARD),
                                     str(source), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == "__main__":
    unittest.main()
