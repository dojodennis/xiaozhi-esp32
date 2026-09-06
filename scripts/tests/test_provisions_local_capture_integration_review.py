"""Compile actual physical-control handlers with concurrent callback interleavings."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def method(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class LocalCaptureIntegrationReview(unittest.TestCase):
    def test_saved_failure_feedback_is_exact_current_and_cancelled_before_next_capture(self):
        source = (ROOT / "main/application.cc").read_text()
        handlers = "\n".join(method(source, signature) for signature in (
            "void Application::StartListening()",
            "void Application::StopListening()",
            "bool Application::BeginLocalRecordingOnMain()",
            "void Application::EndLocalRecordingOnMain()",
            "void Application::HandleVoiceRecordingResult(",
        ))
        program = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/time.h>
#include <thread>
#include <vector>
#include "provisions_reply_turn.h"
#define CONFIG_PROVISIONS_LOCAL_CAPTURE 1
#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1
#define MAIN_EVENT_START_LISTENING 1
#define MAIN_EVENT_STOP_LISTENING 2
constexpr int kDeviceStateIdle=0;
void xEventGroupSetBits(int,int){}
namespace Lang {namespace Sounds {constexpr std::string_view OGG_SUCCESS="success tone";}}
namespace provisions {
namespace feedback {
constexpr std::string_view kSaved="Saved on Orbit. I'll sync when connected.";
constexpr std::string_view kFailed="I couldn't save that. Please repeat it.";
}
struct VoiceRecorder {
    enum class Result {Saved,Failed,NeedsAttention,Synced,ContextReady,RetryQueued,RetryUnavailable};
    bool allow_begin=true;unsigned begun=0,released=0,replays=0;
    bool CanRetry() const {return false;}
    std::function<void()> before_begin;
    bool Begin(uint32_t press,uint64_t){if(before_begin)before_begin();if(!allow_begin)return false;begun=press;return true;}
    void Release(uint32_t press){released=press;}
    void RequestReplay(){++replays;}
};
}
struct Display {
    std::string role,text,status;
    void SetChatMessage(const char* r,const char* t){role=r;text=t;}
    void SetStatus(const char* value){status=value;}
};
struct Board {
    Display display;
    static Board& GetInstance(){static Board board;return board;}
    Display* GetDisplay(){return &display;}
};
struct WebsocketProtocol {
    bool open=false;std::function<void()> before_open;
    bool IsAudioChannelOpened(){if(before_open){auto callback=std::move(before_open);callback();}return open;}
    void InterruptStoredRecording(){}
};
struct AudioService {
    unsigned cancels=0,playing=0,mic=0;bool allow_feedback=true;
    std::string pending;std::vector<std::string> feedback;
    std::function<void()> on_play;
    void CancelLocalFeedback(){++cancels;pending.clear();}
    void StartLocalRecording(uint32_t press){assert(pending.empty());mic=press;}
    void StopLocalRecording(uint32_t expected=0){if(expected==0||mic==expected)mic=0;}
    void CloseVoiceUploadGate(){}
    void FenceLocalRecording(uint32_t){mic=0;}
    void ReleaseLocalRecordingFence(uint32_t){mic=0;}
    void ResetDecoder(){pending.clear();}
    void ReconcileLocalRecording(uint32_t){}
    bool PlayLocalFeedback(std::string_view sound){
        assert(mic==0);if(on_play)on_play();if(!allow_feedback)return false;
        ++playing;pending=std::string(sound);feedback.emplace_back(sound);return true;
    }
};
struct Application {
    std::mutex provisions_recording_control_mutex_;
    ProvisionsReplyTurn provisions_physical_press_;
    std::atomic<bool> manual_listening_requested_{false},has_server_time_{false};
    std::atomic<bool> provisions_recording_failed_{false},provisions_recording_saving_{false},provisions_recording_local_{false};
    std::shared_ptr<provisions::VoiceRecorder> provisions_recorder_=std::make_shared<provisions::VoiceRecorder>();
    std::shared_ptr<WebsocketProtocol> protocol=std::make_shared<WebsocketProtocol>();
    AudioService audio_service_;int event_group_=0,state=0;
    std::vector<std::function<void()>> scheduled;
    std::string alert_status,alert_text,alert_sound;
    Application(){Board::GetInstance().display={};}
    std::shared_ptr<WebsocketProtocol> GetProtocol(){return protocol;}
    int GetDeviceState(){return state;}void SetDeviceState(int value){state=value;}
    const char* GetProvisionsIdleStatus(){return "status";}
    void Schedule(std::function<void()> fn){scheduled.push_back(std::move(fn));}
    void Drain(){auto pending=std::move(scheduled);scheduled.clear();for(auto& fn:pending)fn();}
    void Alert(const char* status,const char* text,const char*,std::string_view sound){
        alert_status=status;alert_text=text;alert_sound=std::string(sound);
    }
    uint32_t provisions_recording_started_press_=0;
    bool BeginLocalRecordingOnMain();void EndLocalRecordingOnMain();
    void StartAndRun(){StartListening();BeginLocalRecordingOnMain();}
    void StopAndRun(){StopListening();EndLocalRecordingOnMain();}
    void StartListening();void StopListening();
    void HandleVoiceRecordingResult(provisions::VoiceRecorder::Result,uint32_t);
};
''' + handlers + r'''
using Result=provisions::VoiceRecorder::Result;
struct Signal {
    std::mutex mutex;std::condition_variable changed;bool set=false;
    void Send(){std::lock_guard<std::mutex> lock(mutex);set=true;changed.notify_all();}
    void Wait(){std::unique_lock<std::mutex> lock(mutex);assert(changed.wait_for(lock,std::chrono::seconds(2),[&]{return set;}));}
};
int main(){
    for(bool online:{false,true}) {
        Application app;app.protocol->open=online;
        app.audio_service_.pending="old cue";
        app.provisions_recorder_->before_begin=[&]{
            assert(app.audio_service_.cancels==1&&app.audio_service_.pending.empty());
        };
        app.StartAndRun();app.provisions_recorder_->before_begin={};
        assert(app.audio_service_.mic==1&&app.audio_service_.feedback.empty());
        app.StopAndRun();assert(app.audio_service_.feedback.empty()&&app.provisions_recording_saving_);
        app.HandleVoiceRecordingResult(Result::ContextReady,0);
        assert(app.audio_service_.feedback.empty()); // No success without a Saved result.
        app.HandleVoiceRecordingResult(Result::Saved,1);
        assert(!app.provisions_recording_saving_&&app.provisions_recording_local_);
        assert(app.provisions_recorder_->replays==1&&app.audio_service_.feedback.size()==1);
        assert(app.audio_service_.feedback.back()==(online?Lang::Sounds::OGG_SUCCESS:provisions::feedback::kSaved));
        if(!online)assert(Board::GetInstance().display.text==provisions::feedback::kSaved);
        app.HandleVoiceRecordingResult(Result::Synced,1);
        assert(!app.provisions_recording_local_&&app.audio_service_.feedback.size()==1);
        app.StartAndRun();assert(app.audio_service_.pending.empty()&&app.audio_service_.mic==2);
        app.HandleVoiceRecordingResult(Result::Saved,1);
        app.HandleVoiceRecordingResult(Result::Failed,1);
        assert(app.audio_service_.mic==2&&app.manual_listening_requested_&&app.audio_service_.feedback.size()==1);
        assert(!app.provisions_recording_failed_&&app.alert_text.empty());
        assert(app.provisions_recorder_->replays==2); // A stale saved record can still sync silently.
    }
    {
        Application app;app.provisions_recorder_->allow_begin=false;app.audio_service_.pending="old cue";
        app.StartAndRun();assert(app.audio_service_.pending.empty()&&app.audio_service_.mic==0);
        assert(app.scheduled.size()==1&&app.manual_listening_requested_&&app.provisions_recording_failed_);
        assert(app.audio_service_.feedback.empty());app.StopAndRun();app.Drain();
        assert(app.alert_status=="Couldn't save"&&app.alert_text==provisions::feedback::kFailed);
        assert(app.alert_sound.empty()&&app.audio_service_.feedback.size()==1);
        assert(app.audio_service_.feedback.back()==provisions::feedback::kFailed);
        assert(app.provisions_recorder_->replays==0&&!app.provisions_recording_local_);
    }
    {
        Application app;app.StartAndRun();
        app.HandleVoiceRecordingResult(Result::Failed,1);
        assert(app.audio_service_.mic==0&&app.manual_listening_requested_);
        assert(app.provisions_recording_failed_&&app.audio_service_.feedback.empty());
        app.StopAndRun();
        app.StartAndRun();assert(app.audio_service_.mic==2&&app.audio_service_.pending.empty());
    }
    {
        Application app;app.provisions_recorder_->allow_begin=false;app.StartAndRun();
        app.provisions_recorder_->allow_begin=true;app.StartAndRun();app.Drain();
        assert(app.audio_service_.mic==2&&app.manual_listening_requested_);
        assert(app.audio_service_.feedback.empty()&&app.alert_text.empty()); // Failed prior Begin cannot interrupt its successor.
    }
    {
        Application app;app.StartAndRun();app.StopAndRun();
        app.protocol->before_open=[&]{app.StartListening();};
        app.HandleVoiceRecordingResult(Result::Saved,1);
        app.BeginLocalRecordingOnMain();
        assert(app.audio_service_.mic==2&&app.manual_listening_requested_);
        assert(app.audio_service_.feedback.empty()&&Board::GetInstance().display.text.empty());
    }
    {
        Application app;app.StartAndRun();app.StopAndRun();app.audio_service_.allow_feedback=false;
        app.HandleVoiceRecordingResult(Result::Saved,1);
        assert(app.provisions_recording_local_&&app.provisions_recorder_->replays==1);
        assert(app.audio_service_.feedback.empty()&&Board::GetInstance().display.text.empty());
    }
    {
        Application app;app.StartAndRun();app.StopAndRun();
        Signal enqueuing,finish_enqueue,press_attempted;std::atomic<bool> press_finished{false};
        app.audio_service_.on_play=[&]{enqueuing.Send();finish_enqueue.Wait();};
        std::thread result([&]{app.HandleVoiceRecordingResult(Result::Saved,1);});enqueuing.Wait();
        std::thread press([&]{press_attempted.Send();app.StartAndRun();press_finished=true;});
        press_attempted.Wait();std::this_thread::sleep_for(std::chrono::milliseconds(20));
        assert(!press_finished); // Main-task Cancel/Begin shares the result gate; the timer edge itself does not.
        finish_enqueue.Send();result.join();press.join();
        assert(app.audio_service_.mic==2&&app.audio_service_.pending.empty());
        assert(app.manual_listening_requested_&&app.provisions_recorder_->begun==2);
        assert(app.audio_service_.feedback.size()==1&&app.audio_service_.cancels==2);
    }
}
'''
        with tempfile.TemporaryDirectory(prefix="orbit-local-feedback-integration-review-") as folder:
            path, binary = Path(folder) / "review.cc", Path(folder) / "review"
            path.write_text(program)
            compiled = subprocess.run([shutil.which("c++"), "-std=c++17", "-pthread",
                                       "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                                       "-I", str(ROOT / "main"), str(path), "-o", str(binary)],
                                      capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_disconnected_callback_pins_protocol_and_rejects_old_generation(self):
        source = (ROOT / "main/protocols/websocket_protocol.cc").read_text()
        callback = method(source, "websocket->OnDisconnected(") + ");"
        program = r'''
#include <atomic>
#include <cassert>
#include <functional>
#include <memory>
#define CONFIG_PROVISIONS_LOCAL_CAPTURE 1
#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1
#define ESP_LOGI(...) ((void)0)
#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT 1
namespace Lang {namespace Strings {constexpr const char* SERVER_NOT_CONNECTED="closed";}}
int events=0;
void xEventGroupSetBits(int,int){++events;}
struct Connection {
    std::function<void()> disconnected;
    void OnDisconnected(std::function<void()> fn){disconnected=std::move(fn);}
};
struct WebsocketProtocol:std::enable_shared_from_this<WebsocketProtocol> {
    bool& destroyed;
    explicit WebsocketProtocol(bool& value):destroyed(value){}
    ~WebsocketProtocol(){destroyed=true;}
    std::atomic<unsigned> connection_generation_{1};
    std::atomic<bool> gateway_authenticated_{true},gateway_hello_pending_{true};
    int event_group_handle_=1,errors=0;
    std::function<void()> on_audio_channel_closed_;
    void SetError(const char*){++errors;}
    void Install(std::shared_ptr<Connection> websocket){
        const unsigned connection_generation=connection_generation_.load();
''' + callback + r'''
    }
};
int main(){
    bool destroyed=false;int closed=0;
    auto wire=std::make_shared<Connection>();
    auto protocol=std::make_shared<WebsocketProtocol>(destroyed);
    protocol->on_audio_channel_closed_=[&]{++closed;};protocol->Install(wire);
    protocol->connection_generation_=2;wire->disconnected();
    assert(closed==0&&events==0&&protocol->gateway_authenticated_);
    // After final protocol disposal, a surviving I/O state must be inert.
    protocol.reset();assert(destroyed);wire->disconnected();assert(closed==0&&events==0);
    destroyed=false;protocol=std::make_shared<WebsocketProtocol>(destroyed);
    protocol->on_audio_channel_closed_=[&]{
        ++closed;protocol.reset();assert(!destroyed); // Callback owns the final temporary pin.
    };
    protocol->Install(wire);wire->disconnected();
    assert(destroyed&&closed==1&&events==1);
    wire->disconnected();assert(closed==1&&events==1);
}
'''
        with tempfile.TemporaryDirectory(prefix="orbit-callback-pin-review-") as folder:
            path, binary = Path(folder) / "review.cc", Path(folder) / "review"
            path.write_text(program)
            compiled = subprocess.run([shutil.which("c++"), "-std=c++17", "-pthread",
                                       "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                                       str(path), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_main_control_cannot_become_sync_when_upload_acquires_between_checks(self):
        source = (ROOT / "main/protocols/websocket_protocol.cc").read_text()
        handler = method(source, "bool WebsocketProtocol::SendText(const std::string& text)")
        program = r'''
#include <atomic>
#include <cassert>
#include <functional>
#include <memory>
#include <string>
#define CONFIG_PROVISIONS_LOCAL_CAPTURE 1
#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1
#define ESP_LOGE(...) ((void)0)
namespace Lang {namespace Strings {constexpr const char* SERVER_ERROR="error";}}
using TaskHandle_t=void*;
TaskHandle_t caller=reinterpret_cast<void*>(1);
TaskHandle_t xTaskGetCurrentTaskHandle(){return caller;}
struct Connection {
    std::function<void()> during_connected;
    int sync=0,queued=0;bool connected=true;
    bool IsConnected(){if(during_connected)during_connected();return connected;}
    bool Send(const std::string&){++sync;return true;}
    bool SendAsync(const std::string&){++queued;return true;}
};
struct WebsocketProtocol {
    std::atomic<TaskHandle_t> operation_owner_{nullptr};
    std::shared_ptr<Connection> websocket_=std::make_shared<Connection>();
    void SetError(const char*){}
    bool SendText(const std::string&);
};
''' + handler + r'''
int main(){
    // IsConnected sits between the initial owner gate and delivery selection
    // in the actual handler. Ownership can change here on a real upload task.
    WebsocketProtocol race;
    race.websocket_->during_connected=[&]{race.operation_owner_.store(reinterpret_cast<void*>(2));};
    race.SendText("abort");
    assert(race.websocket_->sync==0); // Rejecting or queuing is safe; blocking is not.
    WebsocketProtocol idle;assert(idle.SendText("ping"));
    assert(idle.websocket_->queued==1&&idle.websocket_->sync==0);
    WebsocketProtocol owned;owned.operation_owner_.store(caller);assert(owned.SendText("start"));
    assert(owned.websocket_->sync==1&&owned.websocket_->queued==0);
    WebsocketProtocol other;other.operation_owner_.store(reinterpret_cast<void*>(2));
    assert(!other.SendText("abort"));assert(other.websocket_->sync==0&&other.websocket_->queued==0);
}
'''
        with tempfile.TemporaryDirectory(prefix="orbit-control-owner-review-") as folder:
            path, binary = Path(folder) / "review.cc", Path(folder) / "review"
            path.write_text(program)
            compiled = subprocess.run([shutil.which("c++"), "-std=c++17", "-pthread",
                                       str(path), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_release_and_new_press_while_prior_begin_is_running(self):
        application = (ROOT / "main/application.cc").read_text()
        audio = (ROOT / "main/audio/audio_service.cc").read_text()
        header = (ROOT / "main/application.h").read_text()
        handlers = "\n".join(method(application, f"void Application::{name}()")
                             for name in ("StartListening", "StopListening"))
        handlers += "\n" + method(application, "bool Application::BeginLocalRecordingOnMain()")
        handlers += "\n" + method(application, "void Application::EndLocalRecordingOnMain()")
        audio_handlers = "\n".join(method(audio, signature) for signature in (
            "void AudioService::StartLocalRecording(uint32_t press)",
            "void AudioService::StopLocalRecording(uint32_t expected_press)",
            "void AudioService::FenceLocalRecording(",
            "void AudioService::ReleaseLocalRecordingFence(",
            "void AudioService::ReconcileLocalRecording(",
        ))
        import re
        mutexes = "\n".join(re.findall(r"std::mutex\s+\w+_;", header))
        program = r'''
#include <atomic>
#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <sys/time.h>
#include <vector>
#include "provisions_reply_turn.h"
#include "provisions_voice_recording.h"
#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1
#define CONFIG_PROVISIONS_LOCAL_CAPTURE 1
#define MAIN_EVENT_START_LISTENING 1
#define MAIN_EVENT_STOP_LISTENING 2
#define AS_EVENT_LOCAL_RECORDING_RUNNING 4
void xEventGroupSetBits(int,int) {}
void xEventGroupClearBits(int,int) {}
struct Latch {
    std::mutex mutex;std::condition_variable changed;bool set=false;
    void signal(){std::lock_guard<std::mutex> lock(mutex);set=true;changed.notify_all();}
    void wait(){std::unique_lock<std::mutex> lock(mutex);changed.wait(lock,[&]{return set;});}
    void wait_briefly(){std::unique_lock<std::mutex> lock(mutex);changed.wait_for(lock,std::chrono::milliseconds(50),[&]{return set;});}
};
namespace provisions {
struct VoiceRecorder {
    enum class Result {Failed};
    std::array<int16_t,VoiceRecording::kMaxSamples> a{},b{};
    VoiceRecording core{a.data(),b.data(),a.size()};
    VoiceContext context;
    std::function<void(uint32_t)> after_begin;
    VoiceRecorder(){context.conversation_id[0]=1;}
    bool Begin(uint32_t press,uint64_t time){bool ok=core.Begin(press,context,time);if(after_begin)after_begin(press);return ok;}
    void Release(uint32_t press){core.Release(press);}
    void Fail(uint32_t press){core.Fail(press);core.Release(press);}
};
}
struct AudioService {
    std::mutex local_recording_mutex_,audio_queue_mutex_;
    std::condition_variable audio_queue_cv_;
    std::vector<int> audio_decode_queue_,audio_playback_queue_,local_feedback_;
    uint32_t playback_generation_=0;bool local_feedback_active_=false;
    std::atomic<uint32_t> local_recording_press_{0},local_prepared_press_{0},local_physical_boundary_{0},local_output_boundary_{0};
    std::atomic<bool> service_stopped_{false};int event_group_=0;
    void StartLocalRecording(uint32_t);void StopLocalRecording(uint32_t expected_press=0);
    void FenceLocalRecording(uint32_t);void ReleaseLocalRecordingFence(uint32_t);void ReconcileLocalRecording(uint32_t);
    void ResetDecoder(){}
    void CloseVoiceUploadGate(){}
    void CancelLocalFeedback(){}
};
struct WebsocketProtocol {
    unsigned interruptions=0;
    void InterruptStoredRecording(){++interruptions;}
};
struct Application {
    ProvisionsReplyTurn provisions_physical_press_;
    std::atomic<bool> manual_listening_requested_{false},has_server_time_{false};
    std::atomic<bool> provisions_recording_failed_{false},provisions_recording_saving_{false},provisions_recording_local_{false};
    std::shared_ptr<provisions::VoiceRecorder> provisions_recorder_=std::make_shared<provisions::VoiceRecorder>();
    std::shared_ptr<WebsocketProtocol> protocol_=std::make_shared<WebsocketProtocol>();
    std::shared_ptr<WebsocketProtocol> GetProtocol(){return protocol_;}
    AudioService audio_service_;int event_group_=0;
''' + mutexes + r'''
    void Schedule(std::function<void()>){ }
    void HandleVoiceRecordingResult(provisions::VoiceRecorder::Result,uint32_t){}
    uint32_t provisions_recording_started_press_=0;
    bool BeginLocalRecordingOnMain();void EndLocalRecordingOnMain();
    void StartAndRun(){StartListening();BeginLocalRecordingOnMain();}
    void StopAndRun(){StopListening();EndLocalRecordingOnMain();}
    void StartListening();void StopListening();
};
''' + audio_handlers + "\n" + handlers + r'''
int main(){
    for(bool start_new:{false,true}) {
        Application app;unsigned begins=0;
        app.provisions_recorder_->after_begin=[&](uint32_t press){++begins;assert(press==2);};
        app.StartListening();app.StopListening();if(start_new)app.StartListening();
        assert(begins==0&&app.audio_service_.local_recording_press_==0);
        app.BeginLocalRecordingOnMain();app.EndLocalRecordingOnMain();
        assert(begins==(start_new?1U:0U));
        assert(app.protocol_->interruptions==(start_new?1U:0U));
        if(start_new){
            assert(app.manual_listening_requested_&&app.audio_service_.local_recording_press_==2);
            assert(app.provisions_recorder_->core.IsRecording(2)); // Coalesced old STOP did not close press 2.
            app.BeginLocalRecordingOnMain();assert(begins==1); // Duplicate START is idempotent.
            app.StopListening();app.EndLocalRecordingOnMain();
            assert(!app.provisions_recorder_->core.IsRecording(2));
        } else {
            assert(!app.manual_listening_requested_&&app.provisions_recording_started_press_==0);
            assert(!app.provisions_recorder_->core.IsRecording(1)); // Complete tap never opened the recorder.
        }
    }
    for(bool start_new:{false,true}) {
        Application app;Latch begin_entered,begin_release,second_started,second_finished;
        app.provisions_recorder_->after_begin=[&](uint32_t press){if(press==1){begin_entered.signal();begin_release.wait();}};
        std::thread first([&]{app.StartAndRun();});begin_entered.wait();
        std::thread second([&]{second_started.signal();app.StopListening();if(start_new)app.StartListening();second_finished.signal();});
        second_started.wait();second_finished.wait_briefly();
        assert(second_finished.set); // Both timer callbacks return while Begin is blocked.
        assert(app.audio_service_.local_recording_press_.load()==0);
        begin_release.signal();first.join();second.join();
        app.BeginLocalRecordingOnMain();app.EndLocalRecordingOnMain();
        if(start_new) {
            assert(app.protocol_->interruptions==2);
            assert(app.manual_listening_requested_.load());
            assert(app.provisions_physical_press_.id()==2);
            assert(app.audio_service_.local_recording_press_.load()==2);
            assert(app.provisions_recorder_->core.IsRecording(2));
        } else {
            assert(app.protocol_->interruptions==1);
            assert(!app.manual_listening_requested_.load());
            assert(app.audio_service_.local_recording_press_.load()==0);
            assert(!app.provisions_recorder_->core.IsRecording(1));
        }
    }
}
'''
        with tempfile.TemporaryDirectory(prefix="orbit-local-integration-review-") as folder:
            source, binary = Path(folder) / "review.cc", Path(folder) / "review"
            source.write_text(program)
            compiled = subprocess.run([shutil.which("c++"), "-std=c++17", "-pthread",
                                       "-I", str(ROOT / "main"), str(source),
                                       str(ROOT / "main/provisions_voice_recording.cc"), "-o", str(binary)],
                                      capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


    def test_actual_upload_aborts_on_new_press_and_disposes_closed_socket_on_worker(self):
        source = (ROOT / "main/protocols/websocket_protocol.cc").read_text()
        methods = "\n".join(method(source, signature) for signature in (
            "bool WebsocketProtocol::BeginOperation()",
            "void WebsocketProtocol::EndOperation()",
            "void WebsocketProtocol::CloseAudioChannel(bool send_goodbye)",
            "void WebsocketProtocol::InterruptStoredRecording()",
            "bool WebsocketProtocol::SendStoredRecording(",
        ))
        program = r"""
#include <atomic>
#include <cassert>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "provisions_reply_turn.h"
#include "provisions_voice_recording.h"
#define CONFIG_PROVISIONS_LOCAL_CAPTURE 1
#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1
#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT 1
using TaskHandle_t=void*;
TaskHandle_t current_task=reinterpret_cast<void*>(1);
TaskHandle_t xTaskGetCurrentTaskHandle(){return current_task;}
void xEventGroupSetBits(int,int){}
namespace provisions {
struct VoiceReplay {VoiceCapture capture;size_t bytes=0;uint8_t* frames=nullptr;};
std::string VoiceCaptureStart(const VoiceReplay&,const std::string&,uint32_t turn,bool deferred){
    return "start:"+std::to_string(turn)+(deferred?":deferred":":live");
}
}
struct Observed {
    std::vector<std::string> frames;TaskHandle_t disposal=nullptr;
    unsigned closes=0;bool connected=true;
    std::function<void(const std::string&)> during_send;
};
struct WebSocket {
    Observed& seen;explicit WebSocket(Observed& value):seen(value){}
    ~WebSocket(){seen.disposal=current_task;}
    void Close(){++seen.closes;seen.connected=false;}
    bool Send(const std::string& text){if(!seen.connected)return false;seen.frames.push_back(text);if(seen.during_send)seen.during_send(text);return seen.connected;}
    bool Send(const void*,size_t,bool){return Send("binary");}
};
struct WebsocketProtocol {
    using Connection=WebSocket;
    std::atomic<TaskHandle_t> operation_owner_{nullptr};
    std::atomic<bool> upload_active_{false};
    std::atomic<bool> close_requested_{false},gateway_authenticated_{true},capture_enabled_{true},gateway_hello_pending_{false};
    std::atomic<uint32_t> connection_generation_{1};
    std::shared_ptr<WebSocket> websocket_;
    ProvisionsReplyTurn voice_turn_;int event_group_handle_=0;
    bool BeginOperation();void EndOperation();void CloseAudioChannel(bool send_goodbye=false);
    void InterruptStoredRecording();
    bool SendStoredRecording(const provisions::VoiceReplay&,bool,const std::function<bool()>&);
    bool IsAudioChannelOpened(){auto ws=std::atomic_load(&websocket_);return gateway_authenticated_.load() && ws && ws->seen.connected;}
    bool GetCaptureContext(provisions::VoiceContext& out){out.conversation_id[0]=1;return capture_enabled_.load();}
    uint32_t voice_turn_id(){return voice_turn_.id();}
    std::string session_id(){return "session";}
    bool SendText(const std::string& text){auto ws=std::atomic_load(&websocket_);return ws && ws->Send(text);}
};
""" + methods + r"""
int main(){
    uint8_t frames[]={1,0,0xaa,1,0,0xbb};
    provisions::VoiceReplay replay;replay.capture.conversation_id[0]=1;replay.capture.packet_count=2;
    replay.frames=frames;replay.bytes=sizeof(frames);
    {
        Observed seen;WebsocketProtocol p;p.websocket_=std::make_shared<WebSocket>(seen);
        assert(p.SendStoredRecording(replay,false,[]{return true;}));
        assert(seen.frames.size()==4 && seen.frames[0]=="start:1:live" && seen.frames[1]=="binary" && seen.frames[2]=="binary");
        assert(seen.frames[3].find("\"stop\"")!=std::string::npos);assert(p.voice_turn_.IsCurrent(1));
        assert(p.operation_owner_.load()==nullptr);
        assert(!p.upload_active_.load());
    }
    {
        Observed seen;WebsocketProtocol p;p.websocket_=std::make_shared<WebSocket>(seen);bool current=true;
        seen.during_send=[&](const std::string& text){if(text=="binary")current=false;};
        assert(!p.SendStoredRecording(replay,false,[&]{return current;}));
        assert(seen.frames.size()==3 && seen.frames[1]=="binary");
        assert(seen.frames[2].find("\"abort\"")!=std::string::npos);
        assert(!p.voice_turn_.IsCurrent(1) && p.operation_owner_.load()==nullptr);
        assert(!p.upload_active_.load());
    }
    {
        Observed seen;WebsocketProtocol p;p.websocket_=std::make_shared<WebSocket>(seen);
        seen.during_send=[&](const std::string& text){if(text=="binary"){
            // Model a separate main-task disconnect while this worker owns an upload.
            current_task=reinterpret_cast<void*>(2);p.CloseAudioChannel();
            assert(seen.disposal==nullptr);current_task=reinterpret_cast<void*>(1);
        }};
        assert(!p.SendStoredRecording(replay,false,[]{return true;}));
        assert(seen.frames.size()==2 && seen.frames[1]=="binary");
        assert(seen.disposal==reinterpret_cast<void*>(1));assert(!p.websocket_);
        assert(seen.closes==1&&!p.upload_active_.load());
        assert(!p.voice_turn_.IsCurrent(1) && p.operation_owner_.load()==nullptr);
    }
    {
        Observed seen;WebsocketProtocol p;p.websocket_=std::make_shared<WebSocket>(seen);
        assert(p.BeginOperation());current_task=reinterpret_cast<void*>(2);
        assert(!p.SendStoredRecording(replay,true,[]{return true;}));
        assert(seen.frames.empty() && p.operation_owner_.load()==reinterpret_cast<void*>(1));
        current_task=reinterpret_cast<void*>(1);p.EndOperation();
        assert(p.SendStoredRecording(replay,true,[]{return true;}));
        assert(seen.frames.front()=="start:1:deferred");
    }
    {
        Observed seen;WebsocketProtocol p;p.websocket_=std::make_shared<WebSocket>(seen);
        p.InterruptStoredRecording();assert(seen.closes==0); // Idle control preserves the connection.
        seen.during_send=[&](const std::string& text){if(text=="binary"){
            assert(p.upload_active_.load());current_task=reinterpret_cast<void*>(2);
            p.InterruptStoredRecording();assert(seen.closes==1&&seen.disposal==nullptr);
            current_task=reinterpret_cast<void*>(1);
        }};
        assert(!p.SendStoredRecording(replay,false,[]{return true;}));
        assert(seen.frames.size()==2&&seen.frames[1]=="binary");
        assert(!p.voice_turn_.IsCurrent(1)&&!p.upload_active_.load()&&p.operation_owner_.load()==nullptr);
        // No stop frame can commit this partial upload; the immutable recording
        // remains the caller's property for an exact-ID retry after reconnect.
        assert(replay.frames==frames&&replay.bytes==sizeof(frames)&&frames[2]==0xaa&&frames[5]==0xbb);
    }
}
"""
        with tempfile.TemporaryDirectory(prefix="orbit-upload-integration-review-") as folder:
            path, binary = Path(folder) / "review.cc", Path(folder) / "review"
            path.write_text(program)
            compiled = subprocess.run([shutil.which("c++"), "-std=c++17", "-pthread",
                                       "-I", str(ROOT / "main"), str(path), "-o", str(binary)],
                                      capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


    def test_activation_completion_after_disconnect_does_not_play_into_held_recording(self):
        source = (ROOT / "main/application.cc").read_text()
        handler = method(source, "void Application::HandleActivationDoneEvent()")
        program = r"""
#include <atomic>
#include <cassert>
#include <functional>
#include <memory>
#include <string_view>
#define CONFIG_PROVISIONS_LOCAL_CAPTURE 1
#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
enum class PowerSaveLevel {LOW_POWER};
constexpr int kDeviceStateIdle=0;
namespace Lang {namespace Sounds {constexpr std::string_view OGG_EXCLAMATION="error",OGG_SUCCESS="success";}}
namespace SystemInfo {void PrintHeapStats(){}}
struct Display {void SetStatus(const char*){} void SetChatMessage(const char*,const char*){}};
struct Board {Display display;static Board& GetInstance(){static Board b;return b;} Display* GetDisplay(){return &display;}void SetPowerSaveLevel(PowerSaveLevel){}};
struct Protocol {bool open=false;bool IsAudioChannelOpened(){return open;}};
struct Ota {bool HasServerTime(){return true;}void MarkCurrentVersionValid(){}};
struct Audio {int sounds=0;void PlaySound(std::string_view){++sounds;}};
struct Application {
    std::shared_ptr<Protocol> protocol=std::make_shared<Protocol>();
    std::unique_ptr<Ota> ota_=std::make_unique<Ota>();
    std::atomic<bool> manual_listening_requested_{true},has_server_time_{false};
    int provisions_reconnect_attempts_=0,provisions_reconnect_wait_ticks_=0,alerts=0,state=1;
    Audio audio_service_;
    std::shared_ptr<Protocol> GetProtocol(){return protocol;}
    void Alert(const char*,const char*,const char*,std::string_view sound){++alerts;audio_service_.PlaySound(sound);}
    void SetDeviceState(int value){state=value;}
    const char* GetProvisionsIdleStatus(){return "Ready";}
    void Schedule(std::function<void()> fn){fn();}
    void HandleActivationDoneEvent();
};
""" + handler + r"""
int main(){
    // NETWORK_DISCONNECTED is processed first in the real event loop, after
    // activation has queued its done bit. The already held local mic survives.
    Application app;app.protocol->open=false;app.HandleActivationDoneEvent();
    assert(app.audio_service_.sounds==0 && app.alerts==0 && app.state==1);
    app.manual_listening_requested_.store(false);app.HandleActivationDoneEvent();
    assert(app.audio_service_.sounds==1 && app.alerts==1);
    Application success;success.protocol->open=true;success.HandleActivationDoneEvent();
    assert(success.audio_service_.sounds==0 && success.state==1);
}
"""
        with tempfile.TemporaryDirectory(prefix="orbit-activation-integration-review-") as folder:
            path, binary = Path(folder) / "review.cc", Path(folder) / "review"
            path.write_text(program)
            compiled = subprocess.run([shutil.which("c++"), "-std=c++17", str(path), "-o", str(binary)],
                                      capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


    def test_actual_hello_requires_capture_only_for_local_profile_and_status_matches(self):
        websocket = (ROOT / "main/protocols/websocket_protocol.cc").read_text()
        application = (ROOT / "main/application.cc").read_text()
        handlers = "\n".join(method(websocket, signature) for signature in (
            "void WebsocketProtocol::ParseServerHello(const cJSON* root)",
            "void WebsocketProtocol::RejectServerHello(const char* message)",
        )) + "\n" + method(application, "const char* Application::GetProvisionsIdleStatus() const")
        program = r"""
#include <atomic>
#include <cassert>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include "provisions_voice_wire.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT 1
void xEventGroupSetBits(int,int){}
int64_t esp_timer_get_time(){return 100;}
namespace ProvisionsEndpointPolicy {
// All fixtures retain this valid session; UUID policy is unchanged by this delta.
bool IsCanonicalUuid(const std::string& text){return text=="11111111-2222-4333-8444-555555555555";}
}
namespace provisions {VoiceReplay::~VoiceReplay()=default;}
struct WebsocketProtocol {
    std::atomic<bool> gateway_authenticated_{false},gateway_hello_pending_{true},capture_enabled_{false};
    std::atomic<int64_t> last_gateway_activity_us_{0};
    std::mutex capture_context_mutex_;provisions::VoiceContext capture_context_;
    int server_sample_rate_=0,server_frame_duration_=0,event_group_handle_=0,rejected=0;
    std::string session;
    void SetSessionId(std::string text){session=text;} std::string session_id(){return session;}
    void SetError(const char*){++rejected;}
    void ParseServerHello(const cJSON*);void RejectServerHello(const char*);
    bool IsAudioChannelOpened(){return true;}
};
struct Recorder {
    bool ready=false,context=false,attention=false;unsigned count=0;
    bool IsReady(){return ready;}bool HasContext(){return context;}
    bool NeedsAttention(){return attention;}unsigned PendingCount(){return count;}
    bool CanRetry(){return false;}bool RetryPending(){return false;}
};
struct Application {
    std::atomic<bool> provisions_recording_failed_{false},provisions_recording_saving_{false},provisions_response_pending_{false};
    std::shared_ptr<Recorder> provisions_recorder_=std::make_shared<Recorder>();
    std::shared_ptr<WebsocketProtocol> protocol=std::make_shared<WebsocketProtocol>();
    std::shared_ptr<WebsocketProtocol> GetProtocol() const{return protocol;}
    const char* GetProvisionsIdleStatus() const;
};
""" + handlers + r"""
const char* hello=R"({"transport":"websocket","version":1,"session_id":"11111111-2222-4333-8444-555555555555","audio_params":{"format":"opus","sample_rate":24000,"channels":1,"frame_duration":60},"provisions":{"authenticated":true,"turn_ids":true}})";
const char* context=R"({"conversation_id":"22222222-3333-4444-8555-666666666666","source_request_id":null,"source_revision":null})";
int main(){
    for(int variant=0;variant<7;++variant){
        cJSON* root=cJSON_Parse(hello);assert(root);
        cJSON* capabilities=cJSON_GetObjectItemCaseSensitive(root,"provisions");
        if(variant>0) {
            if(variant==1)cJSON_AddFalseToObject(capabilities,"audio_capture");
            else if(variant==2)cJSON_AddStringToObject(capabilities,"audio_capture","true");
            else cJSON_AddTrueToObject(capabilities,"audio_capture");
        }
        if(variant>=4){
            cJSON* capture=cJSON_Parse(context);assert(capture);
            if(variant==4)cJSON_DeleteItemFromObjectCaseSensitive(capture,"conversation_id");
            if(variant==5)cJSON_AddNullToObject(capture,"source_revision");
            cJSON_AddItemToObject(capabilities,"capture_context",capture);
        }
        WebsocketProtocol p;p.ParseServerHello(root);
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
        assert(p.gateway_authenticated_.load()==(variant==6));
        assert(p.capture_enabled_.load()==(variant==6));
        assert(p.rejected==(variant==6?0:1));
        if(variant==6)assert(provisions::VoiceIdText(p.capture_context_.conversation_id)=="22222222-3333-4444-8555-666666666666");
#else
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        assert(p.gateway_authenticated_.load());
#endif
        assert(p.rejected==0);assert(p.session=="11111111-2222-4333-8444-555555555555");
#endif
        cJSON_Delete(root);
    }
    Application app;
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
    assert(std::string(app.GetProvisionsIdleStatus())=="Capture unavailable");
    app.provisions_recorder_->ready=true;
    assert(std::string(app.GetProvisionsIdleStatus())=="Capture unavailable");
    app.provisions_recorder_->context=true;
#endif
    assert(std::string(app.GetProvisionsIdleStatus())=="Ready");
}
"""
        cjson = ROOT / "managed_components/espressif__cjson/cJSON"
        self.assertTrue((cjson / "cJSON.c").exists(), "Canonical dependencies must supply cJSON")
        with tempfile.TemporaryDirectory(prefix="orbit-hello-integration-review-") as folder:
            path = Path(folder)
            for name, text in {
                "freertos/FreeRTOS.h": "#pragma once\n",
                "freertos/task.h": "#pragma once\nusing TaskHandle_t=void*;\n",
                "esp_partition.h": "#pragma once\nstruct esp_partition_t;\n",
            }.items():
                header = path / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text(text)
            source = path / "review.cc"
            source.write_text(program)
            subprocess.run([shutil.which("cc"), "-c", str(cjson / "cJSON.c"), "-o", str(path / "cjson.o")], check=True)
            for required, local in ((1, 1), (1, 0), (0, 0)):
                with self.subTest(gateway_required=required, local_capture=local):
                    binary = path / f"review-{required}-{local}"
                    compiled = subprocess.run([shutil.which("c++"), "-std=c++17", "-pthread",
                                              f"-DCONFIG_PROVISIONS_GATEWAY_REQUIRED={required}",
                                              f"-DCONFIG_PROVISIONS_LOCAL_CAPTURE={local}",
                                              "-I", str(path), "-I", str(ROOT / "main"), "-I", str(cjson),
                                              str(source), str(ROOT / "main/provisions_voice_wire.cc"),
                                              str(ROOT / "main/provisions_voice_recording.cc"),
                                              str(path / "cjson.o"), "-o", str(binary)], capture_output=True, text=True)
                    self.assertEqual(compiled.returncode, 0, compiled.stderr)
                    result = subprocess.run([str(binary)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
