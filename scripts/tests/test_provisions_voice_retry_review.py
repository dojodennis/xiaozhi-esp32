"""Compile the actual blue-button registration and Application retry guards."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def method(source, signature):
    start = source.index(signature)
    cursor = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[cursor] == "{") - (source[cursor] == "}")
        cursor += 1
    return source[start:cursor]


class VoiceRetryUiReview(unittest.TestCase):
    def test_actual_blue_button_is_explicit_queued_and_checks_current_capture(self):
        application = (ROOT / "main/application.cc").read_text()
        board = (ROOT / "main/boards/m5stack/stopwatch/m5stack_stopwatch.cc").read_text()
        handlers = "\n".join(method(application, signature) for signature in (
            "void Application::RetrySavedVoiceRecording()",
            "const char* Application::GetProvisionsIdleStatus() const",
        ))
        buttons = method(board, "void InitializeButtons()")
        silence = method(board, "bool SilenceTimerAlarm()")
        program = r'''
#include <atomic>
#include <cassert>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include "orbit_dial.h"
#define CONFIG_PROVISIONS_LOCAL_CAPTURE 1
#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1
using ProvisionsStopwatchOrbit::AlarmOutputChange;
constexpr int kDeviceStateIdle=0,kDeviceStateListening=1,kDeviceStateSpeaking=2;
struct DisplayLockGuard {template<typename T>explicit DisplayLockGuard(T*){}};
void lv_label_set_text(void*,const char*){}
struct Display {
    std::string text;int silences=0,alarm_id=0;void* alarm_hint_label_=nullptr;
    std::atomic<bool> timer_alarm_active_{false};
    ProvisionsStopwatchOrbit::AlarmState timer_alarm_state_;
    void SetChatMessage(const char*,const char* value){text=value;}
    void RaiseAlarm(){
        ProvisionsTimerSnapshot::Timer timer;timer.id=std::to_string(++alarm_id);
        timer_alarm_state_.Update({timer});timer_alarm_active_=timer_alarm_state_.active();
    }
    bool Audible(){return timer_alarm_state_.active()&&!timer_alarm_state_.silenced();}
    void ApplyAlarmOutputChange(AlarmOutputChange change){if(change==AlarmOutputChange::kStop)++silences;}
    std::function<void()> timer_dismiss_callback_;int dismissals=0;
    AlarmOutputChange DismissDueTimersLocked(){
        ++dismissals;const auto change=timer_alarm_state_.Update({});
        timer_alarm_active_=timer_alarm_state_.active();return change;
    }
''' + silence + r'''
};
struct Board {Display display;static Board& GetInstance(){static Board value;return value;}Display* GetDisplay(){return &display;}};
struct Protocol {bool open=true;bool IsAudioChannelOpened(){return open;}};
struct Recorder {
    int retries=0;bool can_retry=true,attention=true,pending=false,ready=true,context=true;
    bool RequestRetry(){++retries;return can_retry;}
    bool CanRetry(){return can_retry;}bool NeedsAttention(){return attention;}bool RetryPending(){return pending;}
    bool IsReady(){return ready;}bool HasContext(){return context;}unsigned PendingCount(){return 1;}
};
struct Application {
    bool dictation=false;int controls=0;
    bool IsDictationScreen()const{return dictation;}void ToggleDictationScreen(){dictation=!dictation;}void DictationButton(){++controls;}

    std::atomic<bool> manual_listening_requested_{false},provisions_network_busy_{false},provisions_response_pending_{false};
    std::atomic<bool> provisions_recording_failed_{false},provisions_recording_saving_{false};
    std::shared_ptr<Recorder> provisions_recorder_=std::make_shared<Recorder>();
    std::shared_ptr<Protocol> protocol=std::make_shared<Protocol>();
    std::deque<std::function<void()>> scheduled;int state=0,reconnect=0,provisions_reconnect_attempts_=4,provisions_reconnect_wait_ticks_=8;
    static Application& GetInstance(){static Application value;return value;}
    std::shared_ptr<Protocol> GetProtocol()const{return protocol;}int GetDeviceState()const{return state;}
    void ReconnectVoiceGateway(){++reconnect;}void RetrySavedVoiceRecording();const char* GetProvisionsIdleStatus() const;
    void Schedule(std::function<void()> fn){scheduled.push_back(std::move(fn));}
    void Drain(){while(!scheduled.empty()){auto fn=std::move(scheduled.front());scheduled.pop_front();fn();}}
    void StartListening(){manual_listening_requested_=true;state=kDeviceStateListening;}
    void StopListening(){manual_listening_requested_=false;state=kDeviceStateIdle;}
};
struct Button {
    std::function<void()> press,release,long_press,click,double_click;
    void OnPressDown(std::function<void()> fn){press=std::move(fn);}void OnPressUp(std::function<void()> fn){release=std::move(fn);}
    void OnDoubleClick(std::function<void()> fn){double_click=std::move(fn);}
    void OnLongPress(std::function<void()> fn){long_press=std::move(fn);}void OnClick(std::function<void()> fn){click=std::move(fn);}
};
struct Volume {int value=50;int output_volume(){return value;}void SetOutputVolume(int next){value=next;}};
struct StopWatchBoard {
    Button button1_,button2_;Volume volume;int wakes=0;
    Display* display_=&Board::GetInstance().display;
    static constexpr int kDefaultOutputVolume=50,kMaximumOutputVolume=100;
    void ResetDisplayIdleTimer(){++wakes;}Volume* GetAudioCodec(){return &volume;}
''' + buttons + r'''
};
''' + handlers + r'''
int main(){
    auto& app=Application::GetInstance();StopWatchBoard board;board.InitializeButtons();
    assert(board.button2_.long_press&&board.button2_.click&&app.provisions_recorder_->retries==0);
    // The actual queued alarm branch consumes this gesture before volume or retry.
    board.display_->RaiseAlarm();board.button2_.click();
    assert(board.display_->Audible()&&board.display_->silences==0&&board.volume.value==50);
    app.Drain();
    assert(!board.display_->Audible()&&board.display_->silences==1&&board.volume.value==50);
    assert(board.display_->timer_alarm_state_.active()); // Finished timer remains on the face.
    assert(app.provisions_recorder_->retries==0&&!app.manual_listening_requested_);
    for(auto gesture:{board.button2_.long_press,board.button2_.double_click}){
        board.display_->RaiseAlarm();gesture();assert(board.display_->Audible());app.Drain();
        assert(!board.display_->Audible()&&board.volume.value==50&&!app.dictation);
        assert(app.provisions_recorder_->retries==0&&!app.manual_listening_requested_);
    }
    // The next blue gesture on the silenced takeover dismisses it, not volume.
    board.button2_.click();app.Drain();
    assert(board.display_->dismissals==1&&!board.display_->timer_alarm_state_.active());
    assert(!board.display_->timer_alarm_active_&&board.volume.value==50&&app.provisions_recorder_->retries==0);
    board.button2_.click();assert(app.provisions_recorder_->retries==0);app.Drain();assert(board.volume.value==100&&app.provisions_recorder_->retries==0);
    // A new Talk press before the scheduled blue action runs must win.
    board.button2_.long_press();assert(app.provisions_recorder_->retries==0);
    board.button1_.press();app.Drain();assert(app.provisions_recorder_->retries==0&&app.manual_listening_requested_);board.button1_.release();
    for(int blocked=0;blocked<3;++blocked){
        app.provisions_network_busy_=blocked==0;app.provisions_response_pending_=blocked==1;app.state=blocked==2?kDeviceStateSpeaking:kDeviceStateIdle;
        board.button2_.long_press();app.Drain();assert(app.provisions_recorder_->retries==0);
    }
    app.provisions_network_busy_=false;app.provisions_response_pending_=false;app.state=kDeviceStateIdle;
    app.protocol->open=false;board.button2_.long_press();app.Drain();
    assert(app.reconnect==1&&app.provisions_recorder_->retries==0&&app.provisions_reconnect_attempts_==0&&app.provisions_reconnect_wait_ticks_==0);
    assert(Board::GetInstance().display.text=="Connecting. Hold blue again to retry.");
    app.protocol->open=true;app.Drain();assert(app.provisions_recorder_->retries==0); // Reconnection does not imply another gesture.
    board.button2_.long_press();app.Drain();assert(app.provisions_recorder_->retries==1);
    assert(Board::GetInstance().display.text=="Retry queued. Recording stays saved.");
    assert(std::string(app.GetProvisionsIdleStatus())=="Hold blue to retry");
    app.provisions_recorder_->pending=true;assert(std::string(app.GetProvisionsIdleStatus())=="Retry queued");
    app.provisions_recorder_->pending=false;app.provisions_recorder_->can_retry=false;assert(std::string(app.GetProvisionsIdleStatus())=="Recording kept");
    board.button2_.long_press();app.Drain();assert(Board::GetInstance().display.text=="No saved recording is ready to retry.");
    const auto volume_before=board.volume.value;const auto retries_before=app.provisions_recorder_->retries;
    board.button2_.double_click();app.Drain();assert(app.dictation&&app.controls==0&&board.volume.value==volume_before);
    board.button2_.click();board.button2_.long_press();app.Drain();assert(app.controls==1&&app.provisions_recorder_->retries==retries_before&&board.volume.value==volume_before);
    board.button2_.double_click();assert(app.dictation);app.Drain();assert(!app.dictation&&app.controls==1);
    app.provisions_recorder_.reset();board.button2_.long_press();app.Drain();assert(Board::GetInstance().display.text=="No saved recording is ready to retry.");
}
'''
        with tempfile.TemporaryDirectory(prefix="orbit-retry-ui-review-") as directory:
            path = Path(directory)
            source, binary = path / "review.cc", path / "review"
            source.write_text(program)
            (path / "sdkconfig.h").write_text("#pragma once\n#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1\n")
            built = subprocess.run([shutil.which("c++"), "-std=c++17", "-pthread", "-fsanitize=address,undefined",
                                    "-I", str(path), "-I", str(ROOT / "main"),
                                    "-I", str(ROOT / "main/boards/m5stack/stopwatch"),
                                    str(ROOT / "main/boards/m5stack/stopwatch/orbit_dial.cc"),
                                    str(source), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
