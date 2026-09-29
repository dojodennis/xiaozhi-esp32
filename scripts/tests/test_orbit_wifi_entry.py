"""Execute the actual yellow callbacks, hold timer and application entry with fake hardware."""
import unittest
from test_provisions_audio_boundaries import ROOT, method, run_cpp

BOARD = 'main/boards/m5stack/stopwatch/m5stack_stopwatch.cc'

class OrbitWifiEntryTests(unittest.TestCase):
    def test_actual_buttons_and_hold_timer(self):
        source = (ROOT / BOARD).read_text()
        timer = source.split('esp_timer_create_args_t wifi_args = {', 1)[1]
        timer = timer.split('.callback =', 1)[1].split('.arg = this', 1)[0].strip().rstrip(',')
        callbacks = '\n'.join(method(BOARD, x) + ');' for x in
                              ['button1_.OnPressDown([this]()', 'button1_.OnPressUp([this]()'])
        blue = method(BOARD, 'void BluePressed()')
        chord = method(BOARD, 'void OnButtonChord()')
        program = r'''
#include <cassert>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>
#include "__ROOT__/main/boards/m5stack/stopwatch/button_chord.h"
#include "__ROOT__/main/boards/m5stack/stopwatch/menu_wifi_hold.h"
#define CONFIG_PROVISIONS_LOCAL_CAPTURE 1
#define ESP_LOGW(...) ((void)0)
constexpr int ESP_OK=0;int64_t now_us=0;
int64_t esp_timer_get_time(){return now_us;}
int esp_timer_stop(void*){return 0;}
int esp_timer_start_once(void*,int64_t){return 0;}
struct Application {
 bool menu=true,setup=false;int starts=0,stops=0,confirms=0,entries=0;
 std::vector<std::function<void()>> work;
 static Application& GetInstance(){static Application a;return a;}
 bool IsOrbitMenuFace(){return menu;}bool IsOrbitWifiSetup(){return setup;}
 void Schedule(std::function<void()> fn){work.push_back(fn);}
 void StartOrbitWifiSetup(std::function<bool()> allowed){Schedule([this,allowed](){if(allowed()&&menu){++entries;setup=true;}});}
 bool ConfirmOrbitMenu(){if(!menu)return false;++confirms;return false;}
 void StartListening(){++starts;}void StopListening(){++stops;}
 void Drain(){while(!work.empty()){auto batch=std::move(work);work.clear();for(auto& fn:batch)fn();}}
};
struct Button{std::function<void()> down,up;void OnPressDown(std::function<void()> fn){down=fn;}void OnPressUp(std::function<void()> fn){up=fn;}};
struct M5StackStopwatchBoard {
 Button button1_;std::atomic<bool> orbit_locked_{false};std::mutex chord_mutex_;
 struct Display{bool alarm=false;bool HasTimerAlarm(){return alarm;}} display;Display* display_=&display;
 ProvisionsStopWatch::MenuWifiHold wifi_hold_;ProvisionsStopWatch::ButtonChord chord_;
 void* wifi_hold_timer_=this;void* chord_timer_=this;void* lock_timer_=this;
 static constexpr int64_t kLockHoldUs=1000000;std::function<void()> talk;
 void ResetDisplayIdleTimer(){}void ToggleOrbitLock(){orbit_locked_=!orbit_locked_;}
 void ArmTalkStart(std::function<void()> fn){talk=fn;if(chord_.TalkDown(now_us)==ProvisionsStopWatch::ButtonChord::Edge::kChord)OnButtonChord();}
 void Window(){if(chord_.TalkWindowElapsed())talk();}
 bool TalkReleased(){return chord_.TalkUp();}bool TalkClicked(){return chord_.TakeTalkClick();}
 __BLUE__
 __CHORD__
 void Init(){__CALLBACKS__}
 void Fire(){auto fn=__TIMER__;fn(this);}
};
int main(){
 auto& a=Application::GetInstance();
 // Short taps and ordinary holds select on release, never opening audio.
 for(int64_t duration:{100000LL,200000LL,4999999LL}){
  a=Application{};M5StackStopwatchBoard b;b.Init();now_us=0;b.button1_.down();
  if(duration>150000){now_us=150000;b.Window();}
  assert(a.starts==0&&a.confirms==0);now_us=duration;b.button1_.up();a.Drain();
  assert(a.confirms==1&&a.entries==0&&a.starts==0&&a.stops==0);
 }
 // Exact five-second threshold fires once; releasing never selects a menu page.
 a=Application{};{M5StackStopwatchBoard b;b.Init();now_us=0;b.button1_.down();now_us=150000;b.Window();
 now_us=4999999;b.Fire();a.Drain();assert(a.entries==0);
 now_us=5000000;b.Fire();b.Fire();a.Drain();assert(a.entries==1);
 b.button1_.up();a.Drain();assert(a.confirms==0&&a.starts==0&&a.stops==0);}
 // Release, lock, leaving menu and another press revoke a queued timer action.
 for(int revoke=0;revoke<4;++revoke){a=Application{};M5StackStopwatchBoard b;b.Init();now_us=0;b.button1_.down();
 now_us=5000000;b.Fire();
 if(revoke==0)b.button1_.up();if(revoke==1)b.orbit_locked_=true;if(revoke==2)a.menu=false;
 if(revoke==3){b.button1_.up();++now_us;b.button1_.down();}
 a.Drain();assert(a.entries==0);}
 // Both chord orders, and late blue, suppress Wi-Fi and menu confirmation.
 for(int order=0;order<3;++order){a=Application{};M5StackStopwatchBoard b;b.Init();now_us=0;
 if(order==0){b.BluePressed();now_us=100000;b.button1_.down();}
 else {b.button1_.down();now_us=order==1?100000:200000;if(order==2)b.Window();b.BluePressed();}
 now_us=6000000;b.Fire();b.button1_.up();a.Drain();assert(a.entries==0&&a.confirms==0&&a.starts==0);}
 // A ringing overlay must keep its existing Talk path, not reserve a Wi-Fi hold.
 a=Application{};{M5StackStopwatchBoard b;b.Init();b.display.alarm=true;now_us=0;b.button1_.down();
 now_us=150000;b.Window();now_us=5000000;b.Fire();a.Drain();b.button1_.up();assert(a.entries==0&&a.starts==1&&a.stops==1);}
 // Locked presses do not wake Wi-Fi; off-menu Talk still starts and stops.
 a=Application{};{M5StackStopwatchBoard b;b.Init();b.orbit_locked_=true;now_us=0;b.button1_.down();
 now_us=5000000;b.Window();b.Fire();b.button1_.up();a.Drain();assert(a.entries==0&&a.starts==0&&a.confirms==0);}
 a=Application{};a.menu=false;{M5StackStopwatchBoard b;b.Init();now_us=0;b.button1_.down();now_us=150000;b.Window();
 b.button1_.up();assert(a.starts==1&&a.stops==1);}
}
'''
        run_cpp(program.replace('__ROOT__', str(ROOT)).replace('__BLUE__', blue)
                .replace('__CHORD__', chord).replace('__CALLBACKS__', callbacks).replace('__TIMER__', timer))

    def test_actual_application_entry_rechecks_and_finishes(self):
        entry = method('main/provisions_dictation_application.cc', 'void Application::StartOrbitWifiSetup(')
        program = r'''
#include <atomic>
#include <cassert>
#include <functional>
#include <string>
#include <vector>
constexpr int kDeviceStateIdle=1,kDeviceStateWifiConfiguring=2,kDeviceStateStarting=3;
enum class OrbitView { Menu,Other };
int shown=0,hidden=0;
void ProvisionsShowOrbitWifiSetup(const std::string&){++shown;}
void ProvisionsHideOrbitWifiSetup(){++hidden;}
struct Display{void ShowNotification(const char*,int=3000){}};
struct Board{
 int starts=0;bool accepts=true;Display display;
 std::function<void(std::string)> ready;std::function<void(bool)> finished;
 static Board& GetInstance(){static Board b;return b;}Display* GetDisplay(){return &display;}
 bool StartWifiSetup(std::function<void(std::string)> r,std::function<void(bool)> f){++starts;ready=r;finished=f;return accepts;}
};
struct Application{
 std::atomic<bool> orbit_wifi_setup_{false},manual_listening_requested_{false},provisions_network_busy_{false},provisions_response_pending_{false},provisions_recording_saving_{false};
 std::atomic<OrbitView> orbit_view_{OrbitView::Menu};bool provisions_timer_ringing_=false;int state=kDeviceStateIdle,painted=0;
 struct Audio{bool idle=true;bool IsPlaybackIdle(){return idle;}} audio_service_;
 struct Timer{bool fenced=false;bool Fenced(){return fenced;}} timer_player_;
 std::vector<std::function<void()>> work;
 bool IsOrbitWifiSetup(){return orbit_wifi_setup_;}bool IsOrbitMenuFace(){return orbit_view_==OrbitView::Menu;}
 int GetDeviceState(){return state;}void SetDeviceState(int s){state=s;}
 void LeaveDictationScreenOnMain(){}void PaintOrbitView(){++painted;}
 void Schedule(std::function<void()> f){work.push_back(f);}void Drain(){while(!work.empty()){auto batch=std::move(work);work.clear();for(auto& f:batch)f();}}
 void StartOrbitWifiSetup(std::function<bool()>);
};
__ENTRY__
int main(){auto& b=Board::GetInstance();
 for(int initial:{kDeviceStateIdle,kDeviceStateStarting})
 for(int guard=0;guard<10;++guard){Application a;a.state=initial;b.starts=0;bool physical=true;a.StartOrbitWifiSetup([&](){return physical;});
 switch(guard){case 0:physical=false;break;case 1:a.orbit_view_=OrbitView::Other;break;case 2:a.provisions_timer_ringing_=true;break;
 case 3:a.state=99;break;case 4:a.manual_listening_requested_=true;break;case 5:a.provisions_network_busy_=true;break;
 case 6:a.provisions_response_pending_=true;break;case 7:a.provisions_recording_saving_=true;break;case 8:a.audio_service_.idle=false;break;case 9:a.timer_player_.fenced=true;break;}
 a.Drain();assert(b.starts==0&&!a.IsOrbitWifiSetup());}
 for(int state:{kDeviceStateIdle,kDeviceStateStarting})
 for(bool accepts:{true,false}){Application a;a.state=state;b.starts=0;b.accepts=accepts;a.StartOrbitWifiSetup([](){return true;});a.Drain();assert(b.starts==1);
 if(accepts){assert(a.IsOrbitWifiSetup()&&a.state==kDeviceStateWifiConfiguring);b.ready("WIFI:example");a.Drain();b.finished(true);a.Drain();}
 assert(!a.IsOrbitWifiSetup()&&a.state==kDeviceStateIdle&&a.painted==1);
 int before=shown;b.ready("late");a.Drain();assert(shown==before);
 }
}
'''
        run_cpp(program.replace('__ENTRY__', entry))
