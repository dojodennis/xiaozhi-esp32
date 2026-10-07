"""Exercise production role navigation, queued confirmation and Service UI ownership."""

import unittest

from test_provisions_audio_boundaries import ROOT, method, run_cpp

NAVIGATION = "main/provisions_dictation_application.cc"
SERVICE = "main/provisions_service_application.cc"


def production_navigation():
    signatures = [
        "bool Application::IsOrbitShoppingFace()",
        "bool Application::IsOrbitNotesFace()",
        "bool Application::IsOrbitStockFace()",
        "bool Application::IsOrbitMenuFace()",
        "bool Application::IsOrbitModeChoice()",
        "void Application::ShowOrbitModeChoice(",
        "void Application::StartOrbitWifiSetup(",
        "void Application::CancelOrbitWifiSetup()",
        "void Application::HandleOrbitMenuBlueOnMain()",
        "bool Application::ConfirmOrbitMenu()",
        "void Application::ConfirmOrbitMenuOnMain()",
        "void Application::HandleOrbitFaceSwipe(bool right)",
        "void Application::OpenOrbitShoppingOnMain()",
        "void Application::OpenOrbitNotesOnMain()",
        "void Application::OpenOrbitStockOnMain()",
        "void Application::OpenOrbitTimersOnMain()",
    ]
    production = "\n".join(method(NAVIGATION, name) for name in signatures)
    production += "\n" + "\n".join(
        method(SERVICE, name)
        for name in [
            "bool Application::CanSelectOrbitMode()",
            "void Application::SelectOrbitService()",
            "void Application::OrbitServiceFrame(",
            "void Application::OrbitServiceTap()",
            "void Application::OrbitServicePairing()",
            "void Application::StopOrbitServiceCapture()",
            "void Application::PaintOrbitService()",
        ]
    )
    # Execute the unchanged saved-role portion of Initialize, without board setup.
    boot = method("main/application.cc", "void Application::Initialize()")
    boot = boot.split("    auto& board = Board::GetInstance();", 1)[0] + "}"
    production += "\n" + boot.replace("Application::Initialize()", "Application::LoadSavedRole()")
    return production


PROGRAM = r'''
#include <atomic>
#include <array>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#define CONFIG_PROVISIONS_LOCAL_CAPTURE 1
#define ESP_LOGI(...) ((void)0)
constexpr int kDeviceStateIdle=1,kDeviceStateStarting=2,kDeviceStateWifiConfiguring=3;
int g_shopping_scroll=0,timer_shown=0;
namespace provisions {
struct VoiceId { unsigned value=0;std::array<uint8_t,16> bytes{};
uint8_t* data(){return bytes.data();}size_t size()const{return bytes.size();}
uint8_t& operator[](size_t i){return bytes[i];}};
bool operator==(const VoiceId& a,const VoiceId& b){return a.value==b.value;}
bool operator!=(const VoiceId& a,const VoiceId& b){return a.value!=b.value;}
bool ParseVoiceId(const char*,VoiceId&){return false;}
struct VoiceContext { unsigned conversation_id=1; };
namespace dictation {
enum class State { Empty,Open,Stopped,Reviewed };
enum class Action { None,Start,Stop };
struct Record {
    State state=State::Empty;Action pending=Action::None;unsigned count=0;
    VoiceId id;unsigned conversation_id=1;bool authorized=true;
};
}
struct Recorder {
    dictation::Record record;
    bool continuous=false,capture=false,busy=false,faulted=false,discard=false;
    unsigned pending=0,stops=0,starts=0;
    void SetContinuousDictation(bool enabled){continuous=enabled;}
    auto DictationRecord(){return record;}
    bool RequestDictationControl(dictation::Action action){
        if(action==dictation::Action::Stop)++stops;else ++starts;
        return true;
    }
    bool RequestEmptyDictationReplacement(unsigned){++starts;return true;}
    bool CaptureOpen(){return capture;}
    unsigned PendingCount(){return pending;}
    bool DictationBusy(){return busy;}
    bool DictationFaulted(){return faulted;}
    bool DictationDiscardPending(){return discard;}
    bool RequestDiscardedDictation(const VoiceId&){return false;}
};
}
namespace provisions::service {
enum class Stage {None,Preview,Target,Confirm,Saving,Saved};
enum class ReviewError {None,WrongRecording,Conflict,UpdateRequired};
struct Cursor{unsigned sequence=0,text_offset=0,alias_offset=0;};
struct Snapshot{Cursor cursor;int next_alias_offset=-1,next_sequence=-1,next_text_offset=-1;
    bool complete=false;std::string text;};
struct Review{
    Snapshot value;
    void Reset(){} Stage stage()const{return Stage::None;}
    ReviewError error()const{return ReviewError::None;}bool RetryRejected(){return false;}
    bool MoreAliasesSelected()const{return false;}
    Cursor cursor()const{return {};}
    const Snapshot& snapshot()const{return value;}
    void Navigate(Cursor){}bool Choose(){return false;}
    bool Confirm(const VoiceId&){return false;}
    std::string TargetLabel()const{return "Unassigned";}
    bool GuestAllowed()const{return false;}
    bool text_reviewed()const{return false;}
    void MarkTextPage(unsigned,unsigned){}void MarkTargetPage(unsigned,unsigned){}
};
std::string PreviewPage(const std::string& text,unsigned,unsigned& pages){pages=1;return text;}
}
void esp_fill_random(void*,size_t){}
enum class ProvisionsServicePhase{Ready,Preparing,Recording,Processing,Received,Saving,Saved,Lost};
struct Paint {std::string title,focus;int menu_page=-1;};
std::string service_heading;
Paint painted;unsigned paint_count=0,qr_shown=0,qr_hidden=0;
bool ProvisionsShowOrbitService(ProvisionsServicePhase,const std::string& status,const std::string&,const std::string&){
    service_heading="Dojo · Service";painted={"Service",status,-1};++paint_count;return true;
}

std::string shown_qr;
void ProvisionsHideOrbitWifiSetup(){++qr_hidden;}
void ProvisionsShowOrbitWifiSetup(const std::string&){++qr_shown;}
void ProvisionsShowOrbitServicePair(const std::string& qr){shown_qr=qr;++qr_shown;}
void ProvisionsShowOrbitMenu(uint8_t page){painted={"Chef menu","",page};++paint_count;}
void ProvisionsShowTimerFace(){++timer_shown;}
void ProvisionsShowShoppingFocus(const std::string& heading,const std::string& focus,
                                const std::string&,const std::string& title){
    service_heading=heading;painted={title,focus,-1};++paint_count;
}
struct Display {
    std::string notification;
    void ShowNotification(const char* text,int=3000){notification=text;}
    void SetDictationScreen(bool,const char*,const char*){}
};
struct Board {
    unsigned wifi_starts=0;bool accepts_setup=true;
    std::function<void(std::string)> ready;std::function<void(bool)> finished;
    Display display;static Board& GetInstance(){static Board b;return b;}
    Display* GetDisplay(){return &display;}
    bool StartWifiSetup(std::function<void(std::string)> on_ready,std::function<void(bool)> on_finish){
        ++wifi_starts;ready=on_ready;finished=on_finish;return accepts_setup;
    }
    void CancelWifiSetup(){finished(false);}
};
struct Settings {
    static inline unsigned writes=0,reads=0;static inline int saved_role=0;
    Settings(const char*,bool){}
    int GetInt(const char* key,int){assert(std::string(key)=="dojo_service");++reads;return saved_role;}
    void SetInt(const char* key,int value){
        assert(std::string(key)=="dojo_service"&&(value==0||value==1));
        saved_role=value;++writes;
    }
};
struct WebsocketProtocol {
    bool opened=true,chef_send_ok=true;unsigned chef_requests=0,pair_requests=0,closed=0;
    std::string sid="current-session";std::string session_id(){return sid;}
    bool IsAudioChannelOpened(){return opened;}
    bool RequestChefMode(){++chef_requests;return opened&&chef_send_ok;}
    bool RequestServicePair(){++pair_requests;return opened;}
    void CloseAudioChannel(){opened=false;++closed;}
    void InterruptStoredRecording(){}
    bool DictationNegotiated(){return true;}
    bool GetCaptureContext(provisions::VoiceContext&){return true;}
};
struct Audio {
    bool playback_idle=true,input_idle=true;
    bool IsPlaybackIdle(){return playback_idle;}
    bool IsLocalInputIdle(){return input_idle;}
    bool IsLocalRecordingReady(unsigned){return true;}
};
struct Press {unsigned id(){return 1;}};
struct Timer {bool fenced=false;bool Fenced(){return fenced;}};
struct Application {
    enum class OrbitView:uint8_t {Home,Menu,Shopping,Notes,Stock,Service,ModeChoice};
    std::atomic<OrbitView> orbit_view_{OrbitView::Home};
    std::atomic<bool> orbit_service_mode_{false},orbit_service_recording_{false};
    std::atomic<bool> manual_listening_requested_{false},provisions_network_busy_{false};
    std::atomic<bool> provisions_response_pending_{false},provisions_recording_saving_{false};
    std::atomic<bool> orbit_wifi_setup_{false};
    std::atomic<bool> dictation_screen_{false};
    bool wifi=false,orbit_service_ready_=true,orbit_service_recovery_=false;
    bool orbit_mode_choice_service_=false,provisions_timer_ringing_=false;
    std::string orbit_service_code_,orbit_service_status_,orbit_service_table_="3";
    provisions::service::Review orbit_service_review_;
    provisions::VoiceId orbit_service_review_dismissed_{};
    unsigned orbit_service_text_page_=0,orbit_service_target_page_=0;int64_t orbit_service_review_send_us_=0,orbit_service_review_received_us_=0;
    void OrbitServiceNavigate(bool,bool=false){}
    bool orbit_mode_switch_pending_=false;std::string orbit_mode_switch_session_;
    uint8_t orbit_menu_index_=0;int state=kDeviceStateIdle;
    unsigned provisions_reconnect_wait_ticks_=3,fenced=0,ended=0,left=0,cancelled=0;
    std::shared_ptr<provisions::Recorder> provisions_recorder_=std::make_shared<provisions::Recorder>();
    std::shared_ptr<WebsocketProtocol> protocol=std::make_shared<WebsocketProtocol>();
    Audio audio_service_;Press provisions_physical_press_;Timer timer_player_;
    std::vector<std::function<void()>> queue;
    bool IsOrbitService() const{return orbit_service_mode_.load();}
    bool IsOrbitWifiSetup() const{return wifi||orbit_wifi_setup_.load();}
    int GetDeviceState(){return state;}
    void SetDeviceState(int next){state=next;}
    void LeaveDictationScreenOnMain(){dictation_screen_=false;++left;}
    void StopListening(){manual_listening_requested_=false;++fenced;}
    void EndLocalRecordingOnMain(){++ended;}
    auto GetProtocol(){return protocol;}
    void Schedule(std::function<void()> fn){queue.push_back(std::move(fn));}
    void DrainBatch(){auto work=std::move(queue);queue.clear();for(auto& fn:work)fn();}
    void Drain(){while(!queue.empty())DrainBatch();}
    void PaintOrbitView(){
        const auto view=orbit_view_.load();
        __SERVICE_VIEW__
        __CHOICE_VIEW__
        __MENU_VIEW__
    }
    bool IsOrbitShoppingFace() const;bool IsOrbitNotesFace() const;
    bool IsOrbitStockFace() const;bool IsOrbitMenuFace() const;bool IsOrbitModeChoice() const;
    void ShowOrbitModeChoice(std::function<bool()> = {});
    void StartOrbitWifiSetup(std::function<bool()>);void CancelOrbitWifiSetup();
    bool CanSelectOrbitMode();void LoadSavedRole();
    void OpenOrbitShoppingOnMain();void OpenOrbitNotesOnMain();void OpenOrbitStockOnMain();
    void OpenOrbitTimersOnMain();void HandleOrbitMenuBlueOnMain();void HandleOrbitFaceSwipe(bool);
    bool ConfirmOrbitMenu();void ConfirmOrbitMenuOnMain();
    void StopOrbitServiceCapture();void PaintOrbitService();void SelectOrbitService();
    void OrbitServiceTap();void OrbitServicePairing();
    void OrbitServiceFrame(const std::string&,const std::string& detail="");
};
__PRODUCTION__
using View=Application::OrbitView;
void set_guard(Application& app,unsigned guard){
    auto& r=*app.provisions_recorder_;
    switch(guard){
        case 0:app.wifi=true;break;
        case 1:app.manual_listening_requested_=true;break;
        case 2:app.provisions_network_busy_=true;break;
        case 3:r.capture=true;break;
        case 4:r.pending=1;break;
        case 5:r.busy=true;break;
        case 6:r.faulted=true;break;
        case 7:r.record.pending=provisions::dictation::Action::Start;break;
        case 8:r.record.state=provisions::dictation::State::Open;break;
        case 9:r.record.state=provisions::dictation::State::Stopped;r.record.count=1;break;
        case 10:app.orbit_service_recording_=true;break;
        case 11:app.provisions_response_pending_=true;break;
        case 12:app.provisions_recording_saving_=true;break;
        case 13:app.provisions_timer_ringing_=true;break;
        case 14:app.timer_player_.fenced=true;break;
        case 15:app.state=99;break;
        case 16:app.audio_service_.playback_idle=false;break;
        case 17:app.audio_service_.input_idle=false;break;
        case 18:r.discard=true;break;
        case 19:app.provisions_recorder_.reset();break;
        default:assert(false);
    }
}
int main(){__SCENARIO__}
'''


def run_navigation(scenario):
    program = PROGRAM.replace("__PRODUCTION__", production_navigation())
    for tag, signature in [
        ("__SERVICE_VIEW__", "if (view == OrbitView::Service)"),
        ("__CHOICE_VIEW__", "if (view == OrbitView::ModeChoice)"),
        ("__MENU_VIEW__", "if (view == OrbitView::Menu)"),
    ]:
        program = program.replace(tag, method(NAVIGATION, signature))
    run_cpp(program.replace("__SCENARIO__", scenario))


class ServiceNavigationTests(unittest.TestCase):
    def test_shift_capture_has_no_table_and_keeps_recovery_read_only(self):
        run_navigation(r'''
        Application app;app.orbit_service_mode_=true;app.orbit_view_=View::Service;
        app.OrbitServiceFrame("ready", "");app.Drain();
        assert(app.orbit_service_ready_ && app.orbit_service_table_.empty());
        assert(service_heading=="Dojo · Service");
        app.OrbitServiceTap();app.Drain();
        assert(app.orbit_service_recording_ && app.provisions_recorder_->starts==1);
        app.OrbitServiceTap();app.Drain();assert(!app.orbit_service_recording_);
        app.OrbitServiceFrame("recovery", "");app.Drain();
        const auto starts=app.provisions_recorder_->starts;
        app.OrbitServiceTap();app.Drain();
        assert(!app.orbit_service_recording_ && app.provisions_recorder_->starts==starts);
        ''')

    def test_pending_chef_ack_serializes_setup_and_preserves_a_later_active_setup(self):
        run_navigation(r'''
        auto& board=Board::GetInstance();
        for(unsigned action=0;action<3;++action){
            Application app;app.orbit_service_mode_=true;app.orbit_view_=View::Service;
            app.ShowOrbitModeChoice();app.Drain();app.HandleOrbitMenuBlueOnMain();
            app.ConfirmOrbitMenu();app.Drain();assert(app.orbit_mode_switch_pending_);
            const auto starts=board.wifi_starts,requests=app.protocol->chef_requests;
            const auto paints=paint_count;Settings::writes=0;
            if(action==0)app.StartOrbitWifiSetup([]{return true;});
            if(action==1)app.ShowOrbitModeChoice();
            if(action==2)app.ConfirmOrbitMenu();
            app.Drain();assert(!app.IsOrbitWifiSetup() && app.IsOrbitModeChoice());
            assert(board.wifi_starts==starts && paint_count==paints);
            assert(app.protocol->chef_requests==requests && Settings::writes==0 && app.IsOrbitService());
            app.OrbitServiceFrame("chef");app.Drain();
            assert(!app.IsOrbitService() && app.IsOrbitMenuFace() && !app.orbit_mode_switch_pending_);
        }
        // A disconnect releases the pending request. A previously authenticated,
        // already queued ACK must preserve Wi-Fi's surface and state if it arrives later.
        for(bool cancelled:{false,true}){
            Application app;app.orbit_service_mode_=true;app.orbit_view_=View::Service;
            app.ShowOrbitModeChoice();app.Drain();app.HandleOrbitMenuBlueOnMain();
            app.ConfirmOrbitMenu();app.Drain();assert(app.orbit_mode_switch_pending_);
            app.protocol->opened=false;app.StartOrbitWifiSetup([]{return true;});app.Drain();
            assert(app.IsOrbitWifiSetup() && !app.orbit_mode_switch_pending_);
            const auto paints=paint_count,hidden=qr_hidden;Settings::writes=0;
            app.OrbitServiceFrame("chef");app.Drain();
            assert(!app.IsOrbitService() && app.IsOrbitWifiSetup() && app.IsOrbitModeChoice());
            assert(app.state==kDeviceStateWifiConfiguring && paint_count==paints && qr_hidden==hidden);
            assert(Settings::writes==1 && Settings::saved_role==0);
            if(cancelled)app.CancelOrbitWifiSetup();else board.finished(true);app.Drain();
            assert(!app.IsOrbitWifiSetup() && app.state==kDeviceStateIdle && app.IsOrbitMenuFace());
            assert(painted.title=="Chef menu" && !app.dictation_screen_ && !app.IsOrbitService());
        }
        // Rejection by the live send queue does not latch a pending switch or change role.
        Application failed;failed.orbit_service_mode_=true;failed.orbit_view_=View::Service;
        Settings::writes=0;Settings::saved_role=1;failed.protocol->chef_send_ok=false;
        failed.ShowOrbitModeChoice();failed.Drain();failed.HandleOrbitMenuBlueOnMain();
        failed.ConfirmOrbitMenu();failed.Drain();
        assert(failed.IsOrbitService() && failed.IsOrbitModeChoice() && !failed.orbit_mode_switch_pending_);
        assert(failed.protocol->chef_requests==1 && Settings::writes==0);
        failed.StartOrbitWifiSetup([]{return true;});failed.Drain();
        assert(failed.IsOrbitWifiSetup() && failed.IsOrbitService());board.finished(false);failed.Drain();
        assert(failed.orbit_view_==View::Service && Settings::saved_role==1 && Settings::writes==0);
        // A lost session also permits Wi-Fi retry without inferring Chef acceptance.
        Application lost;lost.orbit_service_mode_=true;lost.orbit_view_=View::Service;
        lost.ShowOrbitModeChoice();lost.Drain();lost.HandleOrbitMenuBlueOnMain();
        lost.ConfirmOrbitMenu();lost.Drain();assert(lost.orbit_mode_switch_pending_);
        lost.protocol->opened=false;lost.StartOrbitWifiSetup([]{return true;});lost.Drain();
        assert(lost.IsOrbitWifiSetup() && lost.IsOrbitService() && !lost.orbit_mode_switch_pending_);
        board.finished(true);lost.Drain();
        assert(lost.orbit_view_==View::Service && Settings::saved_role==1 && Settings::writes==0);
        // A new authenticated transport generation releases the stale request too.
        Application replaced;replaced.orbit_service_mode_=true;replaced.orbit_view_=View::Service;
        replaced.ShowOrbitModeChoice();replaced.Drain();replaced.HandleOrbitMenuBlueOnMain();
        replaced.ConfirmOrbitMenu();replaced.Drain();assert(replaced.orbit_mode_switch_pending_);
        replaced.protocol->sid="fresh-session";replaced.ShowOrbitModeChoice();replaced.Drain();
        assert(replaced.IsOrbitModeChoice() && !replaced.orbit_mode_switch_pending_);
        assert(replaced.orbit_mode_choice_service_ && replaced.IsOrbitService());
        ''')

    def test_service_chooser_wifi_preserves_saved_role_and_recording_guards(self):
        run_navigation(r'''
        auto& board=Board::GetInstance();
        for(bool saved:{false,true})for(bool cancelled:{false,true}){
            Application app;app.orbit_service_mode_=true;app.orbit_view_=View::Service;
            Settings::saved_role=1;Settings::writes=0;
            app.ShowOrbitModeChoice();app.Drain();app.HandleOrbitMenuBlueOnMain();
            assert(!app.orbit_mode_choice_service_ && app.IsOrbitService());
            const auto starts=board.wifi_starts;
            app.StartOrbitWifiSetup([]{return true;});app.Drain();
            assert(board.wifi_starts==starts+1 && app.IsOrbitWifiSetup());
            assert(app.state==kDeviceStateWifiConfiguring && app.IsOrbitService());
            assert(Settings::saved_role==1 && Settings::writes==0);
            board.ready("WIFI:synthetic");app.Drain();
            if(cancelled)app.CancelOrbitWifiSetup();else board.finished(saved);
            app.Drain();assert(!app.IsOrbitWifiSetup() && app.state==kDeviceStateIdle);
            assert(app.orbit_view_==View::Service && app.IsOrbitService() && app.dictation_screen_);
            assert(Settings::saved_role==1 && Settings::writes==0 && app.protocol->closed==0);
            const auto shown=qr_shown;board.ready("late");app.Drain();assert(qr_shown==shown);
        }
        for(unsigned guard=0;guard<20;++guard){
            Application app;app.orbit_service_mode_=true;app.orbit_view_=View::Service;
            app.ShowOrbitModeChoice();app.Drain();const auto starts=board.wifi_starts;
            app.StartOrbitWifiSetup([]{return true;});set_guard(app,guard);app.Drain();
            assert(board.wifi_starts==starts && !app.orbit_wifi_setup_ && app.IsOrbitService());
        }
        ''')

    def test_saved_role_browsing_and_same_role_confirmation(self):
        run_navigation(r'''
        const View targets[]={View::Shopping,View::Home,View::Notes,View::Stock};
        for(bool service:{false,true}){
            Settings::saved_role=service;Settings::writes=Settings::reads=0;
            Application app;app.LoadSavedRole();
            assert(app.IsOrbitService()==service && Settings::reads==1 && Settings::writes==0);
            assert(app.orbit_view_==(service?View::Service:View::Home));
            app.ShowOrbitModeChoice();assert(!app.IsOrbitModeChoice());app.Drain();
            assert(app.IsOrbitModeChoice() && app.orbit_mode_choice_service_==service);
            assert(app.IsOrbitService()==service && Settings::writes==0 && app.protocol->closed==0);
            assert(app.fenced==0 && app.ended==0 && app.provisions_recorder_->starts==0);
            assert(painted.focus==(service?"Service":"Chef"));
            app.HandleOrbitMenuBlueOnMain();assert(app.orbit_mode_choice_service_!=service);
            app.HandleOrbitFaceSwipe(false);app.Drain();assert(app.orbit_mode_choice_service_==service);
            for(bool right:{true,false}){
                app.HandleOrbitFaceSwipe(right);app.Drain();app.HandleOrbitFaceSwipe(right);app.Drain();
                assert(app.orbit_mode_choice_service_==service && Settings::writes==0);
            }
            assert(app.ConfirmOrbitMenu());assert(app.IsOrbitModeChoice());app.Drain();
            assert(app.orbit_view_==(service?View::Service:View::Menu));
            assert(app.IsOrbitService()==service && Settings::writes==0 && app.protocol->closed==0);
            if(service){
                for(bool right:{true,false}){
                    app.HandleOrbitFaceSwipe(right);app.Drain();assert(app.orbit_view_==View::Service);
                }
                app.HandleOrbitMenuBlueOnMain();assert(app.orbit_view_==View::Service);
            }else{
                for(unsigned page=0;page<4;++page){
                    assert(app.orbit_menu_index_==page && painted.menu_page==static_cast<int>(page));
                    app.ConfirmOrbitMenu();app.Drain();assert(app.orbit_view_==targets[page]);
                    app.HandleOrbitMenuBlueOnMain();assert(app.IsOrbitMenuFace());
                    app.HandleOrbitMenuBlueOnMain();assert(app.orbit_menu_index_==(page+1)%4);
                }
                for(bool right:{true,false})for(unsigned n=0;n<4;++n){
                    const auto page=app.orbit_menu_index_;
                    app.HandleOrbitFaceSwipe(right);app.Drain();
                    assert(app.orbit_menu_index_==(page+(right?1:3))%4 && app.IsOrbitMenuFace());
                    assert(!app.IsOrbitService() && Settings::writes==0);
                }
            }
        }
        ''')

    def test_queued_entry_and_confirmation_preserve_work_guards(self):
        run_navigation(r'''
        for(bool service:{false,true})for(unsigned guard=0;guard<20;++guard){
            Application app;app.orbit_service_mode_=service;
            app.orbit_view_=service?View::Service:View::Menu;
            app.dictation_screen_=service;const auto before=app.orbit_view_.load();
            Settings::writes=0;const auto paints=paint_count,hidden=qr_hidden;
            app.ShowOrbitModeChoice();set_guard(app,guard);app.Drain();
            assert(app.orbit_view_==before && app.IsOrbitService()==service && Settings::writes==0);
            assert(app.protocol->closed==0 && app.fenced==0 && app.ended==0 && app.left==0);
            assert(paint_count==paints && qr_hidden==hidden);
        }
        for(unsigned guard=0;guard<20;++guard){
            Application app;app.ShowOrbitModeChoice();app.Drain();
            app.HandleOrbitMenuBlueOnMain();assert(app.orbit_mode_choice_service_);
            Settings::writes=0;assert(app.ConfirmOrbitMenu());set_guard(app,guard);app.Drain();
            assert(app.IsOrbitModeChoice() && !app.IsOrbitService() && Settings::writes==0);
            assert(app.protocol->closed==0 && app.fenced==0 && app.ended==0);
        }
        Application stale;bool valid=true;
        stale.ShowOrbitModeChoice([&]{return valid;});valid=false;stale.Drain();
        assert(stale.orbit_view_==View::Home && !stale.IsOrbitService());
        Application reviewed;reviewed.provisions_recorder_->record.state=provisions::dictation::State::Reviewed;
        reviewed.provisions_recorder_->record.count=2;reviewed.ShowOrbitModeChoice();reviewed.Drain();
        assert(reviewed.IsOrbitModeChoice());
        Application offline;offline.protocol->opened=false;offline.state=kDeviceStateStarting;
        offline.ShowOrbitModeChoice();offline.Drain();assert(offline.IsOrbitModeChoice());
        ''')

    def test_explicit_role_change_waits_for_authenticated_chef_ack(self):
        run_navigation(r'''
        Application app;Settings::writes=0;
        app.ShowOrbitModeChoice();app.Drain();app.HandleOrbitMenuBlueOnMain();
        assert(app.ConfirmOrbitMenu());assert(!app.IsOrbitService());app.Drain();
        assert(app.IsOrbitService() && app.orbit_view_==View::Service && app.dictation_screen_);
        assert(Settings::writes==1 && Settings::saved_role==1 && app.protocol->closed==1);
        assert(!app.orbit_service_recording_ && app.provisions_recorder_->starts==0);
        app.protocol->opened=true;app.ShowOrbitModeChoice();app.Drain();
        assert(app.orbit_mode_choice_service_);app.HandleOrbitMenuBlueOnMain();
        assert(app.ConfirmOrbitMenu());app.Drain();
        assert(app.protocol->chef_requests==1 && app.IsOrbitService() && app.IsOrbitModeChoice());
        assert(Settings::writes==1 && app.protocol->closed==1);
        app.orbit_service_recording_=true;const auto paints=paint_count;
        app.OrbitServiceFrame("chef");app.Drain();
        assert(app.IsOrbitService() && app.IsOrbitModeChoice() && Settings::writes==1);
        assert(paint_count==paints && app.protocol->closed==1);
        app.orbit_service_recording_=false;app.OrbitServiceFrame("chef");
        assert(app.IsOrbitService());app.Drain();
        assert(!app.IsOrbitService() && app.IsOrbitMenuFace() && app.orbit_menu_index_==0);
        assert(Settings::writes==2 && Settings::saved_role==0 && app.protocol->closed==2);
        assert(painted.title=="Chef menu" && painted.menu_page==0);
        app.orbit_menu_index_=3;app.PaintOrbitView();const auto stale_paints=paint_count;
        app.OrbitServiceFrame("chef");app.Drain();
        assert(app.IsOrbitMenuFace() && app.orbit_menu_index_==3 && paint_count==stale_paints);
        assert(Settings::writes==2);
        // Offline confirmation retains the selected role and allows retry.
        Application offline;offline.orbit_service_mode_=true;offline.orbit_view_=View::Service;
        offline.protocol->opened=false;offline.ShowOrbitModeChoice();offline.Drain();
        offline.HandleOrbitMenuBlueOnMain();offline.ConfirmOrbitMenu();offline.Drain();
        assert(offline.IsOrbitService() && offline.IsOrbitModeChoice());
        assert(Settings::writes==2 && Board::GetInstance().display.notification=="Connect to switch to Chef");
        ''')

    def test_late_service_frames_and_callbacks_cannot_take_chooser(self):
        run_navigation(r'''
        Application app;app.orbit_service_mode_=true;app.orbit_view_=View::Service;
        app.dictation_screen_=true;app.ShowOrbitModeChoice();app.Drain();
        const auto paints=paint_count,shown=qr_shown,hidden=qr_hidden;
        app.OrbitServiceFrame("pairing","fresh-code");app.Drain();
        assert(app.IsOrbitModeChoice() && app.orbit_service_code_=="fresh-code");
        assert(qr_shown==shown && qr_hidden==hidden && paint_count==paints && app.fenced==0);
        app.OrbitServiceTap();app.OrbitServicePairing();app.Drain();
        assert(app.IsOrbitModeChoice() && !app.orbit_service_recording_);
        assert(app.provisions_recorder_->starts==0 && app.protocol->pair_requests==0);
        app.ConfirmOrbitMenu();app.Drain();
        assert(app.orbit_view_==View::Service && qr_shown==shown+1);
        assert(shown_qr=="dojo-orbit://pair?code=fresh-code" && app.protocol->closed==0);
        for(const std::string frame:{"ready","recovery","approved","expired"}){
            app.ShowOrbitModeChoice();app.Drain();
            const auto current_paints=paint_count,current_qr=qr_shown,current_hidden=qr_hidden;
            app.OrbitServiceFrame(frame,"9");app.Drain();
            assert(app.IsOrbitModeChoice() && paint_count==current_paints);
            assert(qr_shown==current_qr && qr_hidden==current_hidden && !app.dictation_screen_);
            if(frame=="ready" || frame=="recovery"){
                assert(app.orbit_service_ready_ && app.orbit_service_table_=="9");
                assert(app.orbit_service_recovery_==(frame=="recovery"));
            }
            app.ConfirmOrbitMenu();app.Drain();assert(app.orbit_view_==View::Service);
        }
        // Service blue fences a running recording once and retains the Service screen.
        app.orbit_service_code_="old-code";app.orbit_service_recording_=true;
        app.manual_listening_requested_=true;app.provisions_recorder_->continuous=true;
        app.provisions_recorder_->record.state=provisions::dictation::State::Open;
        app.HandleOrbitMenuBlueOnMain();assert(app.orbit_view_==View::Service);
        assert(!app.orbit_service_recording_ && !app.manual_listening_requested_);
        assert(!app.provisions_recorder_->continuous && app.fenced==1 && app.ended==0);
        app.Drain();assert(app.ended==1 && app.provisions_recorder_->stops==1);
        app.HandleOrbitMenuBlueOnMain();app.Drain();assert(app.orbit_view_==View::Service && app.fenced==1);
        ''')


if __name__ == "__main__":
    unittest.main()
