"""Runtime activation faults and capture UI states, executing actual methods."""
import unittest
from test_provisions_audio_boundaries import method, run_cpp


class ActivationAndStatus(unittest.TestCase):
    def test_codec_activation_failures_are_recoverable_and_disable_io(self):
        program = r'''
#include <cassert>
#include <cstdint>
#include <mutex>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
constexpr int ESP_CODEC_DEV_TYPE_IN_OUT=3,GPIO_NUM_NC=-1,ESP_CODEC_DEV_OK=0,ESP_ERR_NO_MEM=257;
int fault=0,created=0,deleted=0,closed=0,pa=0;
struct esp_codec_dev_cfg_t{int dev_type;void* codec_if;void* data_if;};
struct esp_codec_dev_sample_info_t {int bits_per_sample,channel,channel_mask;unsigned sample_rate;int mclk_multiple;};
void* esp_codec_dev_new(void*){if(fault==1)return nullptr;++created;return reinterpret_cast<void*>(2);}
int esp_codec_dev_open(void*,void*){return fault==2?-42:0;}
int esp_codec_dev_set_in_gain(void*,float){return fault==3?-43:0;}
int esp_codec_dev_set_out_vol(void*,int){return fault==4?-44:0;}
int esp_codec_dev_close(void*){++closed;return fault==5?-45:0;}
void esp_codec_dev_delete(void*){++deleted;}
void gpio_set_level(int,int level){pa=level;}
struct AudioCodec{bool input_enabled_=false,output_enabled_=false;void EnableInput(bool);void EnableOutput(bool);};
struct Es8311AudioCodec:AudioCodec {
 void* codec_if_=this;void* data_if_=this;void* dev_=nullptr;std::mutex data_if_mutex_;
 int input_sample_rate_=24000,pa_pin_=1,output_volume_=75;float input_gain_=30;bool pa_inverted_=false;
 void EnableInput(bool);void EnableOutput(bool);void UpdateDeviceState();
};
__METHODS__
int main(){
 for(int stage=0;stage<=4;++stage)for(bool output:{false,true}) {
  Es8311AudioCodec codec;fault=stage;
  if(output)codec.EnableOutput(true);else codec.EnableInput(true);
  if(stage){assert(!codec.dev_&&!codec.input_enabled_&&!codec.output_enabled_&&pa==0);}
  else{assert(codec.dev_&&(output?codec.output_enabled_:codec.input_enabled_));}
  fault=0;codec.EnableInput(true);assert(codec.dev_&&codec.input_enabled_);
  codec.EnableOutput(true);assert(pa==1);
  codec.EnableInput(false);assert(codec.dev_&&!codec.input_enabled_&&codec.output_enabled_);
  fault=5;codec.EnableOutput(false);assert(!codec.dev_&&!codec.output_enabled_&&pa==0);
 }
 assert(created==deleted&&closed>=deleted);
 Es8311AudioCodec missing;missing.codec_if_=nullptr;missing.EnableInput(true);assert(!missing.input_enabled_&&!missing.dev_);
}
'''
        specs = [("main/audio/audio_codec.cc", f"void AudioCodec::{name}(")
                 for name in ("EnableInput", "EnableOutput")]
        specs += [("main/audio/codecs/es8311_audio_codec.cc", f"void Es8311AudioCodec::{name}(")
                  for name in ("EnableInput", "EnableOutput", "UpdateDeviceState")]
        run_cpp(program.replace("__METHODS__", "\n".join(method(*spec) for spec in specs)))

    def test_capture_statuses_never_impersonate_boot(self):
        program = r'''
#include <cassert>
#include <cstring>
#include <initializer_list>
namespace Lang::Strings {
constexpr auto LISTENING="Listening",SPEAKING="Speaking",CONNECTING="Connecting",REGISTERING_NETWORK="Registering",LOADING_PROTOCOL="Loading",ERROR="Error",SERVER_ERROR="Server error",SERVER_NOT_CONNECTED="Disconnected",SERVER_TIMEOUT="Timeout",SERVER_NOT_FOUND="Not found";
}
enum class State{Boot,Idle,Thinking,Listening,Speaking,Connecting,Error};
__METHOD__
int main(){
 for(auto status:{"Saving","Retry queued","Preparing microphone"})assert(StateForStatus(status)==State::Thinking);
 for(auto status:{"Couldn't save","Capture unavailable","Hold blue to retry","Recording kept"})assert(StateForStatus(status)==State::Error);
 assert(StateForStatus("Saved on Orbit")==State::Idle);
 assert(StateForStatus("Listening")==State::Listening);
 assert(StateForStatus("Ready")==State::Idle);
 assert(StateForStatus(nullptr)==State::Boot);
 assert(StateForStatus("Starting")==State::Boot);
 assert(StateForStatus("untrusted status")==State::Boot);
}
'''
        run_cpp(program.replace("__METHOD__", method(
            "main/boards/m5stack/stopwatch/crest_display.h", "static State StateForStatus(")))

    def test_dimmed_talk_wakes_on_main_even_if_capture_cannot_start(self):
        program = r''' 
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <vector>
#define CONFIG_PROVISIONS_LOCAL_CAPTURE 1
#define ESP_LOGE(...) ((void)0)
constexpr int ESP_OK=0,ESP_ERR_INVALID_STATE=2,kAbortReasonNone=0,kDeviceStateNotifying=1,kListeningModeManualStop=2,kDeviceStateListening=3;
using esp_err_t=int;
enum class PowerSaveLevel{PERFORMANCE};
int64_t clock_us=45000000;int restarted=0;
int64_t esp_timer_get_time(){return clock_us;}
int esp_timer_stop(void*){return 0;}
int esp_timer_start_once(void*,int64_t){++restarted;return 0;}
struct Display{bool saving=true;void SetPowerSaveMode(bool value){saving=value;}};
struct Backlight{int restores=0;void RestoreBrightness(){++restores;}};
struct WifiBoard{void SetPowerSaveLevel(PowerSaveLevel){}};
struct Board:WifiBoard{
 void* display_idle_timer_=this;std::atomic<int64_t> display_idle_deadline_us_{0};
 static constexpr int64_t kDisplayIdleTimeoutUs=45000000;
 bool display_dimmed_=true;Display display;Backlight light;
 Display* GetDisplay(){return &display;}Backlight* GetBacklight(){return &light;}
 static Board& GetInstance(){static Board b;return b;}
 void ResetDisplayIdleTimer();void SetPowerSaveLevel(PowerSaveLevel);
};
struct AudioService{void EnableVoiceProcessing(bool){}void EnableWakeWordDetection(bool){}};
struct Application{
 static Application& GetInstance(){static Application a;return a;}
 std::atomic<bool> manual_listening_requested_{true};int listening_mode_=0;AudioService audio_service_;
 std::vector<std::function<void()>> scheduled;bool begin_allowed=false;
 void Schedule(std::function<void()> fn){scheduled.push_back(fn);}
 void AbortSpeaking(int){}bool BeginLocalRecordingOnMain(){return begin_allowed;}
 int GetDeviceState(){return 0;}void StopNotification(){}void SetDeviceState(int){}
 void HandleStartListeningEvent();
 void Drain(){auto work=std::move(scheduled);scheduled.clear();for(auto& fn:work)fn();}
};
__METHODS__
int main(){
 auto& app=Application::GetInstance();auto& board=Board::GetInstance();
 for(bool held:{true,false})for(bool succeeds:{true,false}) {
  board.display_dimmed_=true;board.display.saving=true;
  app.manual_listening_requested_=held;app.begin_allowed=succeeds;
  app.HandleStartListeningEvent();assert(board.display_dimmed_); // queued main work
  app.Drain();assert(!board.display_dimmed_&&!board.display.saving);
  assert(board.display_idle_deadline_us_==clock_us+45000000);
 }
 assert(board.light.restores==4&&restarted==4);
}
'''
        reset = method("main/boards/m5stack/stopwatch/m5stack_stopwatch.cc", "void ResetDisplayIdleTimer()")
        reset = reset.replace("void ResetDisplayIdleTimer()", "void Board::ResetDisplayIdleTimer()")
        power = method("main/boards/m5stack/stopwatch/m5stack_stopwatch.cc", "void SetPowerSaveLevel(PowerSaveLevel level)")
        power = power.replace("void SetPowerSaveLevel(PowerSaveLevel level) override", "void Board::SetPowerSaveLevel(PowerSaveLevel level)")
        main = method("main/application.cc", "void Application::HandleStartListeningEvent()")
        main = main.split("#endif", 1)[0] + "#endif\n}"  # selected local-capture branch
        run_cpp(program.replace("__METHODS__", reset + "\n" + power + "\n" + main))
