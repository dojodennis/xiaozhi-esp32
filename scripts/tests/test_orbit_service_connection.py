"""Native Service connection regressions using the actual renderer and maintenance.

The transport supplies controlled socket/clock events; no network or device is used.
"""
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_provisions_audio_boundaries import method
from test_orbit_service_review import APP_STUB, PROGRAM, HEADERS

ROOT = Path(__file__).resolve().parents[2]


def native(program, model=False, arguments=()):
    with tempfile.TemporaryDirectory(prefix="orbit-service-connection-") as folder:
        path = Path(folder)
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        sources = []
        includes = []
        if model:
            for name, text in HEADERS.items():
                header = path / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text(text)
            cjson = ROOT / "managed_components/espressif__cjson/cJSON"
            subprocess.run([shutil.which("cc"), *flags, "-Wno-deprecated-declarations",
                            "-I", str(cjson), "-c", str(cjson / "cJSON.c"),
                            "-o", str(path / "cjson.o")], check=True, capture_output=True)
            includes = ["-I", str(path), "-I", str(ROOT / "main"), "-I", str(cjson)]
            sources = [str(ROOT / "main/provisions_service_review.cc"),
                       str(ROOT / "main/provisions_voice_wire.cc"),
                       str(ROOT / "main/provisions_voice_recording.cc"), str(path / "cjson.o")]
        (path / "test.cc").write_text(program)
        build = subprocess.run([shutil.which("c++"), "-std=c++17", "-pthread", "-Wall",
                                "-Wextra", "-Werror", *flags, *includes,
                                str(path / "test.cc"), *sources, "-o", str(path / "test")],
                               capture_output=True, text=True, timeout=60)
        if build.returncode:
            raise AssertionError(build.stderr)
        result = subprocess.run([str(path / "test"), *map(str, arguments)], capture_output=True, text=True, timeout=20,
                                env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)


RETRY = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>
#define CONFIG_PROVISIONS_LOCAL_CAPTURE 1
#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
int64_t now_us=1000000;
int64_t esp_timer_get_time(){return now_us;}
int restarts=0;
void esp_restart(){++restarts;}
void vTaskDelete(void*){}
using Worker=std::pair<void(*)(void*),void*>;
std::vector<Worker> workers;
constexpr int pdPASS=1;
int xTaskCreate(void(*run)(void*),const char*,int,void* arg,int,void*){
    workers.emplace_back(run,arg);return pdPASS;
}
void drain_workers(){auto work=std::move(workers);workers.clear();for(auto [run,arg]:work)run(arg);}
constexpr int kDeviceStateIdle=1,kDeviceStateSpeaking=2;
enum class PowerSaveLevel{LOW_POWER};
namespace Lang{namespace Sounds{constexpr char OGG_EXCLAMATION[]="error";}}
struct Display{
    std::string generic_status;
    void SetStatus(const char* value){generic_status=value;}
};
struct Board{
    Display display;
    static Board& GetInstance(){static Board value;return value;}
    Display* GetDisplay(){return &display;}
    void SetPowerSaveLevel(PowerSaveLevel){}
};
struct Socket{bool connected=false;bool IsConnected()const{return connected;}};
struct WebsocketProtocol{
    std::shared_ptr<Socket> websocket_=std::make_shared<Socket>();
    std::atomic<bool> gateway_authenticated_{false},error_occurred_{false};
    std::atomic<int64_t> last_gateway_activity_us_{0};
    bool lite=false,busy=false,server_available=false;
    unsigned opens=0,pings=0,closes=0;
    std::vector<int64_t> open_times;
    bool IsLiteMode()const{return lite;}
    bool IsTransportBusy()const{return busy;}
    std::string session_id()const{return "test-session";}
    bool IsAudioChannelOpened()const;
    bool IsGatewayHeartbeatExpired()const;
    bool SendGatewayHeartbeat();
    bool SendText(const std::string&){++pings;last_gateway_activity_us_=now_us;return true;}
    bool OpenAudioChannel(){
        ++opens;open_times.push_back(now_us);
        websocket_->connected=server_available;gateway_authenticated_=server_available;
        error_occurred_=false;
        if(server_available)last_gateway_activity_us_=now_us;
        return server_available;
    }
    void CloseAudioChannel(){++closes;websocket_->connected=false;gateway_authenticated_=false;}
};
using Protocol=WebsocketProtocol;
struct Recorder{unsigned replays=0;void RequestReplay(){++replays;}};
struct Ota{
    bool pending=false;
    bool HasServerTime(){return true;}
    void MarkCurrentVersionValid(){}
    bool IsCurrentVersionPendingVerification(){return pending;}
};
struct Audio{bool IsPlaybackIdle(){return true;}void ResetDecoder(){}};
struct Fence{bool Fenced(){return false;}};
struct Application{
    bool service=true,wifi_setup=false,aborted_=false;
    int state=kDeviceStateIdle;
    std::atomic<bool> manual_listening_requested_{false},network_connected_{true},
        provisions_network_busy_{false},provisions_response_pending_{false},has_server_time_{false};
    std::atomic<int64_t> provisions_tts_deadline_us_{0};
    int provisions_response_ticks_=0,provisions_heartbeat_ticks_=0;
    int provisions_reconnect_attempts_=0,provisions_reconnect_wait_ticks_=0;
    void* activation_task_handle_=nullptr;
    std::shared_ptr<Protocol> protocol=std::make_shared<Protocol>();
    std::shared_ptr<Recorder> provisions_recorder_=std::make_shared<Recorder>();
    std::unique_ptr<Ota> ota_;
    Audio audio_service_;Fence timer_player_;
    std::vector<std::function<void()>> scheduled;
    bool IsOrbitService(){return service;}
    bool IsOrbitWifiSetup(){return wifi_setup;}
    bool IsLiteMode(){return protocol->IsLiteMode();}
    int GetDeviceState(){return state;}
    void SetDeviceState(int value){state=value;}
    auto GetProtocol(){return protocol;}
    void Schedule(std::function<void()> work){scheduled.push_back(std::move(work));}
    void Drain(){auto work=std::move(scheduled);scheduled.clear();for(auto& fn:work)fn();}
    void SetProvisionsResponsePending(bool value){provisions_response_pending_=value;}
    void InvalidateProvisionsTtsTurn(){}
    void FinishLiteTurn(){}
    void RenderLiteFace(int,const std::string&){}
    const char* GetProvisionsIdleStatus(){return "Ready";}
    void Alert(const char*,const char*,const char*,const char*){}
    void ReconnectVoiceGateway();void HandleProvisionsGatewayMaintenance();
    void Step(){HandleProvisionsGatewayMaintenance();drain_workers();Drain();now_us+=1000000;}
};
namespace provisions{namespace lite{struct Face{static constexpr int kFailed=1;};}}
__CONSTANTS__
__METHODS__
int main(){__SCENARIO__}
'''


def retry_program(scenario, local_capture=True):
    app = (ROOT / "main/application.cc").read_text()
    constants = "\n".join(re.findall(r"constexpr int(?:64_t)? kProvisions(?:HeartbeatIntervalSeconds|ResponseTimeoutSeconds|MaximumReconnectAttempts|LiteReplyTimeoutSeconds|ServiceReconnectIntervalSeconds) = [^;]+;", app))
    methods = [
        method("main/protocols/websocket_protocol.cc", "bool WebsocketProtocol::IsAudioChannelOpened() const"),
        method("main/protocols/websocket_protocol.cc", "bool WebsocketProtocol::IsGatewayHeartbeatExpired() const"),
        method("main/protocols/websocket_protocol.cc", "bool WebsocketProtocol::SendGatewayHeartbeat()"),
        method("main/application.cc", "void Application::HandleProvisionsGatewayMaintenance()"),
    ]
    if local_capture:
        methods.insert(-1, method("main/application.cc", "void Application::ReconnectVoiceGateway()"))
    else:
        constants = re.sub(r"constexpr int kProvisionsServiceReconnectIntervalSeconds = [^;]+;", "", constants)
    program = RETRY.replace("__CONSTANTS__", constants).replace("__METHODS__", "\n".join(methods)).replace("__SCENARIO__", scenario)
    return program if local_capture else program.replace("#define CONFIG_PROVISIONS_LOCAL_CAPTURE 1", "#define CONFIG_PROVISIONS_LOCAL_CAPTURE 0")


def service_program(scenario, frame=False):
    stub = APP_STUB.replace("int64_t esp_timer_get_time(){return 1000;}",
                            "int64_t now_us=1000000;int64_t esp_timer_get_time(){return now_us;}int64_t ServiceNow(){return now_us/1000;}")
    stub = stub.replace("std::string display_body;", "ProvisionsServicePhase display_phase;std::string display_status,display_help,display_body;")
    stub = stub.replace("bool ProvisionsShowOrbitService(ProvisionsServicePhase,const std::string&,const std::string& body,const std::string&)",
                        "bool ProvisionsShowOrbitService(ProvisionsServicePhase phase,const std::string& status,const std::string& body,const std::string& help)")
    stub = stub.replace("display_body=body;", "display_phase=phase;display_status=status;display_help=help;display_body=body;")
    stub = stub.replace("unsigned starts=0;", "unsigned starts=0,replays=0,pending_count=0;bool busy=false;")
    stub = stub.replace("bool DictationBusy(){return false;}", "bool DictationBusy(){return busy;}")
    stub = stub.replace("unsigned PendingCount(){return 0;}", "unsigned PendingCount(){return pending_count;}")
    stub = stub.replace("void SetContinuousDictation(bool){}", "bool DictationDiscardPending(){return false;}bool DictationCapped(unsigned){return false;}void RequestReplay(){++replays;}void SetContinuousDictation(bool){}")
    stub = stub.replace("bool opened=true;", "std::vector<std::string> sent;bool opened=true;")
    stub = stub.replace("bool DictationNegotiated()", "bool SendServiceReview(const std::string& text){sent.push_back(text);return opened;}bool DictationNegotiated()")
    stub = stub.replace("out.conversation_id=binding;", "out.conversation_id=approved;")
    stub = stub.replace("struct Audio {", "struct Audio {bool IsLocalInputIdle(){return true;}")
    stub = stub.replace("bool orbit_service_ready_=true", "bool has_server_time_=false;std::atomic<bool> provisions_network_busy_{false};bool orbit_service_ready_=true")
    stub = stub.replace("void OrbitServiceReviewFrame", "void StartListening(){manual_listening_requested_=true;}void TickOrbitService();void OrbitServiceReviewFrame")
    signatures = ['void Application::TickOrbitService()', 'void Application::PaintOrbitService()',
                  'void Application::OrbitServiceTap()']
    if frame:
        signatures.append('void Application::OrbitServiceReviewFrame(')
    methods = '\n'.join(method('main/provisions_service_application.cc', signature) for signature in signatures)
    return PROGRAM.split('int main(', 1)[0] + stub + methods + scenario


class ServiceConnectionTests(unittest.TestCase):
    def test_actual_gateway_projector_rejections_parse_and_preserve_connection(self):
        paths = [ROOT / "scripts/tests/fixtures" / f"orbit_service_gateway_rejected_{error}.json"
                 for error in ("wrong_recording", "conflict", "update_required")]
        native(service_program(r'''
int main(int argc,char** argv){
    assert(argc==4);
    const ReviewError errors[]={ReviewError::WrongRecording,ReviewError::Conflict,ReviewError::UpdateRequired};
    for(int i=1;i<argc;++i){
        std::ifstream file(argv[i]);assert(file);std::stringstream buffer;buffer<<file.rdbuf();
        auto frame=parse(buffer.str());auto value=cJSON_GetObjectItemCaseSensitive(frame.get(),"session_id");assert(cJSON_IsString(value));
        const std::string sid=value->valuestring;Rejection rejection;
        assert(ParseRejection(frame.get(),sid,rejection));assert(rejection.error==errors[i-1]);
        Application app;app.protocol->sid=sid;app.protocol->approved=rejection.binding_id;
        auto& record=app.provisions_recorder_->record;
        record.id=rejection.recording_id;record.conversation_id=rejection.binding_id;record.state=provisions::dictation::State::Reviewed;
        app.orbit_service_review_.Reset(record.id,record.conversation_id,sid);app.orbit_service_review_.Navigate(rejection.cursor);
        app.TickOrbitService();assert(app.protocol->sent.size()==1);
        app.OrbitServiceReviewFrame(frame.get(),sid);app.Drain();
        assert(app.orbit_service_review_.error()==errors[i-1]);
        for(unsigned tick=0;tick<90;++tick){now_us+=1000000;app.TickOrbitService();}
        assert(app.protocol->sent.size()==1&&app.protocol->opened&&app.orbit_service_ready_);
        assert(record.id==rejection.recording_id&&record.conversation_id==rejection.binding_id);
        assert(record.state==provisions::dictation::State::Reviewed&&app.provisions_recorder_->starts==0);
    }
}
''', frame=True), model=True, arguments=paths)

    def test_non_capture_gateway_retains_existing_retry_budget(self):
        native(retry_program(r'''
    Application legacy;
    for(unsigned tick=0;tick<180;++tick)legacy.Step();
    assert(legacy.protocol->opens==5&&legacy.provisions_reconnect_attempts_==5);
    legacy.protocol->server_available=true;
    for(unsigned tick=0;tick<90;++tick)legacy.Step();
    assert(!legacy.protocol->IsAudioChannelOpened()&&legacy.protocol->opens==5);
''', local_capture=False))

    def test_review_rejection_keeps_connection_audio_and_requires_explicit_retry(self):
        native(service_program(r'''
std::string rejected(const char* error){return std::string(R"({"type":"dojo_service","state":"review_rejected","session_id":"current-service-session","binding_id":"22222222-3333-4444-8555-666666666666","recording_id":"11111111-2222-4333-8444-555555555555","sequence":0,"text_offset":0,"alias_offset":0,"error":")")+error+R"("})";}
int main(){
    Application app;auto& record=app.provisions_recorder_->record;
    record.id=recording;record.conversation_id=binding;record.state=provisions::dictation::State::Reviewed;
    app.TickOrbitService();assert(app.protocol->sent.size()==1);
    auto rejection=parse(rejected("wrong_recording"));Rejection parsed;
    assert(ParseRejection(rejection.get(),session,parsed));
    for(const char* field:{"type","state","session_id","binding_id","recording_id","sequence","text_offset","alias_offset","error"}){
        auto bad=parse(rejected("wrong_recording"));cJSON_AddNullToObject(bad.get(),field);
        assert(!ParseRejection(bad.get(),session,parsed));
        bad=parse(rejected("wrong_recording"));cJSON_DeleteItemFromObjectCaseSensitive(bad.get(),field);
        assert(!ParseRejection(bad.get(),session,parsed));
    }
    for(const char* key:{"sequence","text_offset","alias_offset"})for(const char* value:{"-1","1.5","100001","null","\"0\""}){
        auto bad=parse(rejected("wrong_recording"));replace(bad.get(),key,value);assert(!ParseRejection(bad.get(),session,parsed));
    }
    auto oversized=parse(rejected("wrong_recording"));replace(oversized.get(),"text_offset","6001");
    assert(!ParseRejection(oversized.get(),session,parsed));
    oversized=parse(rejected("wrong_recording"));replace(oversized.get(),"alias_offset","10001");
    assert(!ParseRejection(oversized.get(),session,parsed));
    auto bad=parse(rejected("unknown"));assert(!ParseRejection(bad.get(),session,parsed));
    bad=parse(rejected("wrong_recording"));cJSON_AddStringToObject(bad.get(),"target","general");assert(!ParseRejection(bad.get(),session,parsed));
    // Wrong recording/binding/cursor/session responses cannot clear current review.
    for(const char* key:{"recording_id","binding_id","sequence","text_offset","alias_offset"}){
        auto stale=parse(rejected("wrong_recording"));
        replace(stale.get(),key,std::string(key).find("_id")!=std::string::npos?"\""+VoiceIdText(other)+"\"":"1");
        app.OrbitServiceReviewFrame(stale.get(),session);app.Drain();assert(app.orbit_service_review_.error()==ReviewError::None);
    }
    app.OrbitServiceReviewFrame(rejection.get(),"old-session");app.Drain();assert(app.orbit_service_review_.error()==ReviewError::None);
    app.OrbitServiceReviewFrame(rejection.get(),session);app.protocol->sid="renewed-session";
    app.Drain();assert(app.orbit_service_review_.error()==ReviewError::None);app.protocol->sid=session;
    app.OrbitServiceReviewFrame(rejection.get(),session);app.protocol->approved=other;
    app.Drain();assert(app.orbit_service_review_.error()==ReviewError::None);app.protocol->approved=binding;
    app.OrbitServiceReviewFrame(rejection.get(),session);app.orbit_service_review_.Navigate({1,0,0});
    app.Drain();assert(app.orbit_service_review_.error()==ReviewError::None);
    app.orbit_service_review_.Reset(recording,binding,session);
    app.OrbitServiceReviewFrame(rejection.get(),session);app.Drain();
    assert(app.orbit_service_review_.error()==ReviewError::WrongRecording);
    assert(display_phase==ProvisionsServicePhase::Ready&&display_status=="Review unavailable. Recording kept");
    assert(display_help=="yellow requests a new recording");
    app.protocol->opened=false;app.PaintOrbitService();
    assert(display_phase==ProvisionsServicePhase::Lost&&display_status=="Connection lost. Recording kept");
    app.protocol->opened=true;app.PaintOrbitService();assert(display_phase==ProvisionsServicePhase::Ready);
    const auto sent=app.protocol->sent.size();
    for(int i=0;i<90;++i){now_us+=1000000;app.TickOrbitService();}
    assert(app.protocol->sent.size()==sent&&app.protocol->opened&&app.orbit_service_ready_);
    assert(record.id==recording&&record.conversation_id==binding&&record.state==provisions::dictation::State::Reviewed);
    auto incoming=parse(fixture());app.OrbitServiceReviewFrame(incoming.get(),session);app.Drain();
    assert(app.orbit_service_review_.stage()==Stage::None); // A late preview cannot lift rejection.
    app.provisions_recorder_->pending_count=1;app.OrbitServiceTap();app.Drain();assert(app.provisions_recorder_->starts==0);
    app.provisions_recorder_->pending_count=0;app.OrbitServiceTap();app.Drain();assert(app.provisions_recorder_->starts==1);
    // A transient conflict permits one explicit review request, not repeated409 polling.
    app.orbit_service_recording_=false;app.orbit_service_review_.Reset(recording,binding,session);
    auto conflict=parse(rejected("conflict"));app.OrbitServiceReviewFrame(conflict.get(),session);app.Drain();
    app.TickOrbitService();assert(app.protocol->sent.size()==sent);
    app.OrbitServiceTap();app.Drain();app.TickOrbitService();assert(app.protocol->sent.size()==sent+1);
    app.OrbitServiceReviewFrame(conflict.get(),session);app.Drain();
    now_us+=2000000;app.TickOrbitService();assert(app.protocol->sent.size()==sent+1);
    // An update rejection neither starts capture nor restores review automatically.
    app.orbit_service_review_.Reset(recording,binding,session);auto update=parse(rejected("update_required"));
    app.OrbitServiceReviewFrame(update.get(),session);app.Drain();app.OrbitServiceTap();app.Drain();
    app.TickOrbitService();assert(app.provisions_recorder_->starts==1&&app.protocol->sent.size()==sent+1);
    // Rejection arriving after an explicit route cannot retire pending persistence.
    Review review;Snapshot snapshot;assert(ParseSnapshot(incoming.get(),session,snapshot));
    review.Reset(recording,binding,session);assert(review.Accept(snapshot,session));review.MarkTextPage(0,1);
    assert(review.Choose());review.NextTarget();review.MarkTargetPage(0,1);assert(review.Choose());assert(review.Confirm(request));
    assert(ParseRejection(rejection.get(),session,parsed));assert(!review.Reject(parsed,session));
    assert(review.Saved(review.pending(),session));assert(!review.Reject(parsed,session));
}
''', frame=True), model=True)

    def test_completed_record_is_not_reviewed_under_a_renewed_binding(self):
        native(service_program(r'''
int main(){
    Application app;auto& record=app.provisions_recorder_->record;
    record.id=recording;record.conversation_id=binding;record.state=provisions::dictation::State::Reviewed;
    auto incoming=parse(fixture());app.OrbitServiceReviewFrame(incoming.get(),session);app.Drain();
    assert(app.orbit_service_review_.stage()==Stage::Preview);
    // Final completed preview has its independent keepalive, with no review poll.
    for(int i=0;i<75;++i){app.TickOrbitService();now_us+=1000000;}
    assert(app.protocol->sent.empty());
    // A valid new hello changes binding/capture context, not the persisted record.
    app.protocol->approved=other;app.protocol->sid="renewed-session";
    for(int i=0;i<10;++i){app.TickOrbitService();now_us+=1000000;}
    if(!app.protocol->sent.empty())std::fprintf(stderr,"old completed record auto-reviewed under new binding; requests=%zu\n",app.protocol->sent.size());
    assert(app.protocol->sent.empty());
    assert(record.id==recording&&record.conversation_id==binding&&record.state==provisions::dictation::State::Reviewed);
    assert(app.protocol->opened&&app.orbit_service_ready_&&app.provisions_recorder_->starts==0);
    // Unsolicited preview claiming the new binding cannot reattribute old audio.
    replace(incoming.get(),"binding_id","\""+VoiceIdText(other)+"\"");
    replace(incoming.get(),"session_id","\"renewed-session\"");
    app.OrbitServiceReviewFrame(incoming.get(),"renewed-session");app.Drain();
    assert(app.orbit_service_review_.stage()==Stage::None);
    // Existing journal guards still block pending/busy recordings. No reset.
    app.provisions_recorder_->pending_count=1;app.OrbitServiceTap();app.Drain();
    assert(app.provisions_recorder_->starts==0);
    app.provisions_recorder_->pending_count=0;app.provisions_recorder_->busy=true;
    app.OrbitServiceTap();app.Drain();assert(app.provisions_recorder_->starts==0);
    app.provisions_recorder_->busy=false;app.OrbitServiceTap();app.Drain();
    assert(app.provisions_recorder_->starts==1&&app.orbit_service_recording_);
    assert(record.id==recording&&record.conversation_id==binding);
}
''', frame=True), model=True)

    def test_connected_ready_caption_does_not_retain_disconnect_warning(self):
        stub = APP_STUB.replace("std::string display_body;", "ProvisionsServicePhase display_phase;std::string display_status;std::string display_body;")
        stub = stub.replace("bool ProvisionsShowOrbitService(ProvisionsServicePhase,const std::string&,", "bool ProvisionsShowOrbitService(ProvisionsServicePhase phase,const std::string& status,")
        stub = stub.replace("display_body=body;", "display_phase=phase;display_status=status;display_body=body;")
        scenario = r'''
int main(){
    Application app;
    app.orbit_service_status_="Connection lost. Recording kept";
    app.protocol->opened=false;app.PaintOrbitService();
    assert(display_phase==ProvisionsServicePhase::Lost);
    assert(display_status=="Connection lost. Recording kept");
    // A late current-session frame can restore liveness without a new hello.
    app.protocol->opened=true;app.PaintOrbitService();
    assert(display_phase==ProvisionsServicePhase::Ready);
    if(display_status.find("Connection lost")!=std::string::npos)
        std::fprintf(stderr,"connected Ready phase retained stale caption: %s\n",display_status.c_str());
    assert(display_status.find("Connection lost")==std::string::npos);
    app.orbit_service_status_="Discarded at desk";app.PaintOrbitService();
    assert(display_status=="Discarded at desk");
}
'''
        prefix = PROGRAM.split("int main(int argc,char** argv){", 1)[0]
        native(prefix + stub + method("main/provisions_service_application.cc", "void Application::PaintOrbitService()") + scenario, model=True)

    def test_service_retry_recovers_after_fast_attempt_budget_without_pairing(self):
        native(retry_program(r'''
    Application app;
    // Five real production reconnect worker failures while Wi-Fi stays up.
    for(unsigned tick=0;tick<180&&app.protocol->opens<5;++tick)app.Step();
    assert(app.protocol->opens==5&&app.provisions_reconnect_attempts_==5);
    const auto failed_at=now_us;
    app.protocol->server_available=true;
    for(unsigned tick=0;tick<90&&!app.protocol->IsAudioChannelOpened();++tick)app.Step();
    if(!app.protocol->IsAudioChannelOpened())
        std::fprintf(stderr,"Service remained closed after server recovery; opens=%u attempts=%d wait=%d elapsed=%lld\n",app.protocol->opens,app.provisions_reconnect_attempts_,app.provisions_reconnect_wait_ticks_,static_cast<long long>(now_us-failed_at));
    assert(app.protocol->IsAudioChannelOpened());
    assert(app.protocol->opens==6);
    assert(app.provisions_reconnect_attempts_==0);
    assert(now_us-failed_at>=1000000); // A bounded delay, never a retry spin.
    // Sustained failure keeps saturated state and only one worker per cooldown.
    Application outage;
    for(unsigned tick=0;tick<300;++tick)outage.Step();
    assert(outage.provisions_reconnect_attempts_==5);
    assert(outage.protocol->opens>=6&&outage.protocol->opens<=15);
    for(size_t i=5;i<outage.protocol->open_times.size();++i)
        assert(outage.protocol->open_times[i]-outage.protocol->open_times[i-1]>=30000000);
'''))

    def test_idle_keepalive_and_real_failure_are_independent_of_review_polling(self):
        native(retry_program(r'''
    Application app;app.protocol->server_available=true;
    assert(app.protocol->OpenAudioChannel());
    for(unsigned tick=0;tick<75;++tick)app.Step();
    assert(app.protocol->IsAudioChannelOpened());
    assert(app.protocol->pings==5&&app.protocol->opens==1);
    // Authentication and protocol errors remain closed even with a live socket.
    app.protocol->error_occurred_=true;assert(!app.protocol->IsAudioChannelOpened());
    app.protocol->error_occurred_=false;
    app.protocol->gateway_authenticated_=false;assert(!app.protocol->IsAudioChannelOpened());
    app.protocol->gateway_authenticated_=true;
    now_us+=46000000;assert(app.protocol->IsGatewayHeartbeatExpired());
    assert(!app.protocol->IsAudioChannelOpened());
    app.HandleProvisionsGatewayMaintenance();assert(app.protocol->closes==1);
'''))

    def test_chef_limit_and_pending_firmware_rollback_remain_bounded(self):
        native(retry_program(r'''
    Application chef;chef.service=false;
    for(unsigned tick=0;tick<180;++tick)chef.Step();
    assert(chef.protocol->opens==5);
    chef.protocol->server_available=true;
    for(unsigned tick=0;tick<90;++tick)chef.Step();
    assert(chef.protocol->opens==5&&!chef.protocol->IsAudioChannelOpened());
    Application pending;pending.ota_=std::make_unique<Ota>();pending.ota_->pending=true;
    pending.provisions_reconnect_attempts_=5;pending.Step();
    assert(restarts==1&&pending.protocol->opens==0);
'''))


if __name__ == "__main__":
    unittest.main()
