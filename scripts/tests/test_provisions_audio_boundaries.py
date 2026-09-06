"""Actual codec/input methods: errors, DMA tails, fresh presses and cancellation."""
import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def method(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    cursor = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[cursor] == "{") - (source[cursor] == "}")
        cursor += 1
    return source[start:cursor]


def run_cpp(program):
    with tempfile.TemporaryDirectory(prefix="orbit-audio-boundary-") as directory:
        path = Path(directory)
        (path / "test.cc").write_text(program)
        build = subprocess.run(["c++", "-std=c++17", "-pthread", "-fsanitize=address,undefined",
                                "-fno-omit-frame-pointer", str(path / "test.cc"), "-o", str(path / "test")],
                               capture_output=True, text=True, timeout=60)
        if build.returncode:
            raise AssertionError(build.stderr)
        result = subprocess.run([str(path / "test")], capture_output=True, text=True, timeout=15,
                                env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)


CODEC = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <vector>
constexpr int ESP_OK=0,ESP_CODEC_DEV_OK=0,ESP_ERR_INVALID_STATE=2,AUDIO_CODEC_DMA_DESC_NUM=6;
#define portENTER_CRITICAL(p) ((p)->lock())
#define portEXIT_CRITICAL(p) ((p)->unlock())
#define portENTER_CRITICAL_ISR(p) portENTER_CRITICAL(p)
#define portEXIT_CRITICAL_ISR(p) portEXIT_CRITICAL(p)
using i2s_chan_handle_t=void*;struct i2s_event_data_t{};
int read_error=0,write_error=0,disable_error=0,enable_error=0;
int disabled=0,enabled=0;std::vector<int16_t> rx_queue{111};
int esp_codec_dev_read(void*,void* out,int bytes){if(read_error)return read_error;std::fill_n(static_cast<int16_t*>(out),bytes/2,222);return 0;}
int esp_codec_dev_write(void*,void*,int){return write_error;}
int i2s_channel_disable(void*){++disabled;return disable_error;}
int i2s_channel_enable(void*){++enabled;if(!enable_error)rx_queue.clear();return enable_error;}
class AudioCodec {public:virtual int Read(int16_t*,int)=0;virtual int Write(const int16_t*,int)=0;
 bool InputData(std::vector<int16_t>&);bool OutputData(std::vector<int16_t>&);};
class Es8311AudioCodec:public AudioCodec {public:
 bool input_enabled_=true,output_enabled_=true;void* dev_=this;void* rx_handle_=this;
 std::mutex data_if_mutex_;mutable std::mutex output_dma_mutex_;bool output_write_active_=false;uint32_t output_dma_remaining_=0;
 int Read(int16_t*,int) override;int Write(const int16_t*,int) override;
 void EnableInput(bool value){input_enabled_=value;}
 bool PrepareInputCapture();bool IsOutputDrained() const;
 static bool OnOutputSent(i2s_chan_handle_t,i2s_event_data_t*,void*);
};
__METHODS__
int main(){
 Es8311AudioCodec codec;std::vector<int16_t> pcm(240,12345);
 read_error=-1;assert(!codec.InputData(pcm));assert(pcm.front()==12345);
 read_error=0;assert(codec.InputData(pcm));assert(pcm.front()==222);
 codec.input_enabled_=false;assert(!codec.InputData(pcm));
 assert(codec.PrepareInputCapture());assert(disabled==1&&enabled==1&&rx_queue.empty());
 disable_error=-1;assert(!codec.PrepareInputCapture());assert(enabled==1);
 disable_error=ESP_ERR_INVALID_STATE;assert(codec.PrepareInputCapture());
 enable_error=-1;assert(!codec.PrepareInputCapture());
 write_error=-1;assert(!codec.OutputData(pcm));assert(!codec.IsOutputDrained());
 for(int i=0;i<5;++i){codec.OnOutputSent(nullptr,nullptr,&codec);assert(!codec.IsOutputDrained());}
 codec.OnOutputSent(nullptr,nullptr,&codec);assert(codec.IsOutputDrained());
 write_error=0;assert(codec.OutputData(pcm));assert(!codec.IsOutputDrained());
 for(int i=0;i<6;++i)codec.OnOutputSent(nullptr,nullptr,&codec);
 assert(codec.IsOutputDrained());codec.output_write_active_=true;assert(!codec.IsOutputDrained());
}
'''

INPUT = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1
#define CONFIG_PROVISIONS_LOCAL_CAPTURE 1
#define ESP_LOGW(...) ((void)0)
constexpr int AS_EVENT_AUDIO_TESTING_RUNNING=1,AS_EVENT_WAKE_WORD_RUNNING=2,AS_EVENT_AUDIO_PROCESSOR_RUNNING=4,AS_EVENT_LOCAL_RECORDING_RUNNING=8,AS_EVENT_AUDIO_INPUT_STOP_REQUEST=16;
constexpr int pdFALSE=0,pdTRUE=1,portMAX_DELAY=-1,AUDIO_TESTING_MAX_DURATION_MS=1000,OPUS_FRAME_DURATION_MS=60,ESP_AE_ERR_OK=0,kAudioTaskTypeEncodeToTestingQueue=1;
#define pdMS_TO_TICKS(v) (v)
using EventBits_t=int;
struct Event{std::mutex mutex;std::condition_variable cv;int bits=0;};
void xEventGroupSetBits(Event* e,int b){std::lock_guard<std::mutex> l(e->mutex);e->bits|=b;e->cv.notify_all();}
void xEventGroupClearBits(Event* e,int b){std::lock_guard<std::mutex> l(e->mutex);e->bits&=~b;}
int xEventGroupGetBits(Event* e){std::lock_guard<std::mutex> l(e->mutex);return e->bits;}
int xEventGroupWaitBits(Event* e,int b,int,int,int){std::unique_lock<std::mutex> l(e->mutex);e->cv.wait(l,[&]{return e->bits&b;});return e->bits;}
void vTaskDelay(int ms){std::this_thread::sleep_for(std::chrono::milliseconds(ms));}
std::atomic<int> resets=0;int esp_ae_rate_cvt_reset(void*){++resets;return ESP_AE_ERR_OK;}
struct Codec {
 std::atomic<bool> drained{true},hold{false},entered{false},release{false},fail{false};std::atomic<int> prepares{0};
 bool input_enabled(){return true;}void EnableInput(bool){}size_t input_channels(){return 1;}
 bool IsOutputDrained(){return drained;}
 bool PrepareInputCapture(){++prepares;entered=true;while(hold&&!release)std::this_thread::sleep_for(100us);return !fail;}
};
struct Engine{void Feed(std::vector<int16_t>) {}};
struct AudioService {
 std::atomic<bool> service_stopped_{false},audio_input_need_warmup_{false};
 std::atomic<uint32_t> local_recording_press_{0},timer_output_owner_{0},local_input_press_{0},local_prepared_press_{0},local_physical_boundary_{0},local_output_boundary_{0};
 std::mutex local_recording_mutex_,audio_queue_mutex_,input_resampler_mutex_;std::condition_variable audio_queue_cv_;
 std::deque<int> audio_decode_queue_{1},audio_playback_queue_{2,3},audio_testing_queue_;
 std::string_view local_feedback_;bool local_feedback_active_=false,output_in_flight_=false;uint32_t playback_generation_=0;
 Event event;Event* event_group_=&event;Codec codec;Codec* codec_=&codec;Engine engine;Engine* audio_engine_=&engine;void* input_resampler_=this;
 struct {std::function<void(uint32_t,const int16_t*,size_t,size_t)> on_recording_audio;std::function<void(uint32_t)> on_recording_error,on_recording_ready;}callbacks_;
 std::atomic<bool> hold_read{false},read_entered{false},release_read{false};
 bool ReadAudioData(std::vector<int16_t>& data,int,int){read_entered=true;while(hold_read&&!release_read)std::this_thread::sleep_for(100us);data.assign(160,codec.prepares.load());std::this_thread::sleep_for(1ms);return true;}
 void EnableAudioTesting(bool){}void PushTaskToEncodeQueue(int,std::vector<int16_t>){}
 void AudioInputTask();void StartLocalRecording(uint32_t);void FenceLocalRecording(uint32_t);void ReleaseLocalRecordingFence(uint32_t);void ReconcileLocalRecording(uint32_t);void StopLocalRecording(uint32_t expected_press=0);bool IsLocalRecordingClosed(uint32_t) const;bool IsLocalInputIdle() const;bool IsLocalRecordingReady(uint32_t) const;
 void stop(){service_stopped_=true;xEventGroupSetBits(event_group_,AS_EVENT_AUDIO_INPUT_STOP_REQUEST);}
};
__METHODS__
template<class F>void wait_for(F f){auto end=std::chrono::steady_clock::now()+2s;while(!f()&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(100us);assert(f());}
int main(){
 {AudioService a;std::atomic<int> seen=0;a.callbacks_.on_recording_audio=[&](uint32_t p,const int16_t* pcm,size_t,size_t){assert(p==1&&pcm[0]==1&&resets>=1);++seen;};
  a.codec.drained=false;std::thread input([&]{a.AudioInputTask();});a.FenceLocalRecording(1);a.StartLocalRecording(1);
  assert(a.audio_decode_queue_.empty()&&a.audio_playback_queue_.empty());std::this_thread::sleep_for(10ms);assert(a.codec.prepares==0&&seen==0&&!a.IsLocalRecordingReady(1));
  a.codec.drained=true;wait_for([&]{return seen>0;});assert(a.IsLocalRecordingReady(1));a.StopLocalRecording(1);assert(!a.IsLocalRecordingReady(1));wait_for([&]{return a.IsLocalRecordingClosed(1);});a.stop();input.join();}
 {AudioService a;std::atomic<int> wrong=0,seen=0;a.codec.hold=true;
  a.callbacks_.on_recording_audio=[&](uint32_t p,const int16_t* pcm,size_t,size_t){if(p!=2)++wrong;assert(pcm[0]==2);++seen;};
  std::thread input([&]{a.AudioInputTask();});a.FenceLocalRecording(1);a.StartLocalRecording(1);wait_for([&]{return a.codec.entered.load();});
  a.StopLocalRecording(1);assert(!a.IsLocalRecordingClosed(1));a.FenceLocalRecording(2);a.StartLocalRecording(2);a.codec.release=true;
  wait_for([&]{return seen>0;});assert(wrong==0&&a.codec.prepares==2&&a.IsLocalRecordingClosed(1)&&!a.IsLocalInputIdle());a.StopLocalRecording(2);a.stop();input.join();}
 {AudioService a;std::atomic<int> seen=0;a.hold_read=true;a.callbacks_.on_recording_audio=[&](uint32_t,const int16_t*,size_t,size_t){++seen;};
  std::thread input([&]{a.AudioInputTask();});a.FenceLocalRecording(1);a.StartLocalRecording(1);wait_for([&]{return a.read_entered.load();});a.ReleaseLocalRecordingFence(1);
  // Only the production timer fence has run; main StopLocalRecording is delayed.
  assert(!a.IsLocalRecordingClosed(1)&&!a.IsLocalInputIdle());a.release_read=true;wait_for([&]{return a.IsLocalRecordingClosed(1);});assert(seen==0&&a.IsLocalInputIdle());a.StopLocalRecording();a.stop();input.join();}
 {AudioService a;std::atomic<int> errors=0,seen=0;a.codec.fail=true;a.callbacks_.on_recording_error=[&](uint32_t p){assert(p==1);++errors;};
  a.callbacks_.on_recording_audio=[&](uint32_t,const int16_t*,size_t,size_t){++seen;};std::thread input([&]{a.AudioInputTask();});a.FenceLocalRecording(1);a.StartLocalRecording(1);
  wait_for([&]{return errors==1;});assert(seen==0);a.stop();input.join();}
 {AudioService a;std::atomic<int> errors=0;a.codec.drained=false;
  a.callbacks_.on_recording_error=[&](uint32_t p){assert(p==1);++errors;};
  std::thread input([&]{a.AudioInputTask();});a.FenceLocalRecording(1);a.StartLocalRecording(1);wait_for([&]{return errors==1;});
  wait_for([&]{return a.IsLocalInputIdle();});assert(a.codec.prepares==0&&!a.read_entered);a.stop();input.join();}
}
'''


class AudioBoundaries(unittest.TestCase):
    def test_actual_notification_failure_cannot_become_success_at_drain(self):
        program = r'''
#include <cassert>
#include <cstdint>
#include <functional>
#include <mutex>
#define ESP_LOGW(...) ((void)0)
struct NotifyPlayer {
 using FinishedCallback=std::function<void(uint32_t,bool)>;
 std::mutex mutex_;bool active_=true,cancelled_=false,completion_reported_=false,
 playback_drained_=false,http_finished_=true,stream_started_=true;
 uint32_t playback_id_=7,underrun_count_=0,last_playback_position_ms_=0;
 FinishedCallback finished_callback_;
 FinishedCallback CompleteLocked(uint32_t&);void OnPlaybackDrained();void OnPlaybackError(uint32_t);
};
__METHODS__
int main(){
 {NotifyPlayer p;int calls=0;p.finished_callback_=[&](uint32_t id,bool ok){assert(id==7&&!ok);++calls;};
  p.OnPlaybackError(7);p.OnPlaybackDrained();p.OnPlaybackError(7);assert(calls==1&&p.cancelled_);}
 {NotifyPlayer p;int calls=0;p.finished_callback_=[&](uint32_t id,bool ok){assert(id==7&&ok);++calls;};
  p.OnPlaybackError(6);p.OnPlaybackError(0);assert(calls==0&&!p.cancelled_);
  p.OnPlaybackDrained();assert(calls==1);}
 {NotifyPlayer p;int calls=0;p.http_finished_=false;
  p.finished_callback_=[&](uint32_t,bool ok){assert(!ok);++calls;};
  p.OnPlaybackError(7);p.http_finished_=true;p.OnPlaybackDrained();assert(calls==1);}
}
'''
        run_cpp(program.replace("__METHODS__", "\n".join(method("main/notify/notify_player.cc", signature)
            for signature in ("NotifyPlayer::FinishedCallback NotifyPlayer::CompleteLocked(",
                              "void NotifyPlayer::OnPlaybackError(", "void NotifyPlayer::OnPlaybackDrained("))))

    def test_actual_read_rejects_failed_or_missing_resampler(self):
        program = r'''
#include <cassert>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>
#define CONFIG_USE_AUDIO_DEBUGGER 0
constexpr int ESP_AE_ERR_OK=0,AUDIO_POWER_CHECK_INTERVAL_MS=1000;
using esp_ae_sample_t=void*;
void esp_timer_stop(int){}void esp_timer_start_periodic(int,int){}
int size_error=0,process_error=0;uint32_t capacity=160,returned=160;
int esp_ae_rate_cvt_get_max_out_sample_num(void*,uint32_t,uint32_t* n){*n=capacity;return size_error;}
int esp_ae_rate_cvt_process(void*,void*,uint32_t,void*,uint32_t* n){*n=returned;return process_error;}
struct Codec {
 bool read_ok=true;int rate=24000;
 bool input_enabled(){return true;}void EnableInput(bool){}int input_channels(){return 1;}
 int input_sample_rate(){return rate;}bool InputData(std::vector<int16_t>&){return read_ok;}
};
struct AudioService {
 Codec codec;Codec* codec_=&codec;int audio_power_timer_=0;void* input_resampler_=this;
 std::mutex input_resampler_mutex_;std::chrono::steady_clock::time_point last_input_time_;
 struct {unsigned input_count=0;}debug_statistics_;
 bool ReadAudioData(std::vector<int16_t>&,int,int);
};
__METHOD__
int main(){
 AudioService a;std::vector<int16_t> data;
 a.input_resampler_=nullptr;assert(!a.ReadAudioData(data,16000,160));a.input_resampler_=&a;
 size_error=-1;assert(!a.ReadAudioData(data,16000,160));size_error=0;
 capacity=0;assert(!a.ReadAudioData(data,16000,160));capacity=160;
 process_error=-1;assert(!a.ReadAudioData(data,16000,160));process_error=0;
 returned=0;assert(!a.ReadAudioData(data,16000,160));returned=161;assert(!a.ReadAudioData(data,16000,160));
 returned=160;assert(a.ReadAudioData(data,16000,160)&&data.size()==160&&a.debug_statistics_.input_count==1);
 a.codec.read_ok=false;assert(!a.ReadAudioData(data,16000,160));
 a.codec.rate=16000;assert(!a.ReadAudioData(data,16000,160));
}
'''
        run_cpp(program.replace("__METHOD__", method("main/audio/audio_service.cc", "bool AudioService::ReadAudioData(")))

    def test_actual_recorder_flushes_without_extending_capture_or_journal(self):
        program = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
constexpr int ESP_AUDIO_SAMPLE_RATE_16K=16000,ESP_AUDIO_MONO=1,ESP_AUDIO_BIT16=16,
ESP_OPUS_BITRATE_AUTO=-1000,ESP_OPUS_ENC_FRAME_DURATION_60_MS=5,
ESP_OPUS_ENC_APPLICATION_VOIP=0,ESP_AUDIO_ERR_OK=0;
struct esp_opus_enc_config_t {int sample_rate,channel,bits_per_sample,bitrate,frame_duration,application_mode,complexity;bool enable_fec,enable_dtx,enable_vbr;};
struct esp_audio_enc_in_frame_t {uint8_t* buffer;uint32_t len;};
struct esp_audio_enc_out_frame_t {uint8_t* buffer;uint32_t len,encoded_bytes;uint64_t pts;};
int frames=0,closed=0,info_error=0;size_t captured=0;
int esp_opus_enc_open(void*,size_t,void** out){*out=&frames;return 0;}
int esp_opus_enc_get_frame_size(void*,int* in,int* out){*in=1920;*out=227;return info_error;}
int esp_opus_enc_process(void*,esp_audio_enc_in_frame_t* in,esp_audio_enc_out_frame_t* out){
 assert(in->len==1920);auto* pcm=reinterpret_cast<int16_t*>(in->buffer);
 for(size_t i=0;i<960;i++)assert(pcm[i]==(size_t(frames)*960+i<captured ? 777:0));
 ++frames;out->encoded_bytes=2;out->buffer[0]=frames;out->buffer[1]=1;return 0;
}
void esp_opus_enc_close(void*){++closed;}
void mbedtls_platform_zeroize(void* p,size_t n){memset(p,0,n);}
struct VoiceCapture {unsigned packet_count=0,sample_count=0;bool IsDictation()const{return false;}};
struct VoiceOutbox {static constexpr size_t kMaxPackets=167,kMaxFrameBytes=262478;};
struct VoiceRecording {static constexpr size_t kMaxSamples=160000;struct Work{bool failed=false;size_t samples;const int16_t* pcm;VoiceCapture capture;};};
__CONSTANT__
struct VoiceRecorder{uint8_t* frames_;bool Encode(const VoiceRecording::Work&,VoiceCapture&,size_t&);};
__METHOD__
int main(){
 std::vector<uint8_t> storage(VoiceOutbox::kMaxFrameBytes);VoiceRecorder recorder{storage.data()};
 for(size_t count:{size_t(1),size_t(640),size_t(641),size_t(960),size_t(961),size_t(160000)}) {
  // Exact-sized allocation lets ASan detect any attempt to read padding from
  // beyond the released microphone buffer.
  std::vector<int16_t> pcm(count,777);captured=count;frames=0;closed=0;
  VoiceRecording::Work work{false,count,pcm.data(),{}};VoiceCapture capture;size_t bytes=0;
  assert(recorder.Encode(work,capture,bytes));assert(closed==1);
  assert(size_t(frames)==(count+320+959)/960);assert(capture.packet_count==unsigned(frames));
  assert(bytes==size_t(frames)*4&&capture.packet_count<=167);
 }
 std::vector<int16_t> pcm(960,777);VoiceRecording::Work work{false,960,pcm.data(),{}};
 VoiceCapture capture;size_t bytes=0;info_error=-1;closed=0;frames=0;
 assert(!recorder.Encode(work,capture,bytes));assert(closed==1&&frames==0);
 info_error=0;work.samples=0;assert(!recorder.Encode(work,capture,bytes));
 work.samples=160001;assert(!recorder.Encode(work,capture,bytes));
 work.samples=960;work.failed=true;assert(!recorder.Encode(work,capture,bytes));
}
'''
        source = (ROOT / "main/provisions_voice_recorder.cc").read_text()
        constant = re.search(r"constexpr size_t kCaptureTailSamples = [^;]+;", source)[0]
        run_cpp(program.replace("__CONSTANT__", constant).replace("__METHOD__", method(
            "main/provisions_voice_recorder.cc", "bool VoiceRecorder::Encode(")))

    def test_actual_es8311_error_fresh_rx_and_dma_drain(self):
        specs = [("main/audio/audio_codec.cc", "bool AudioCodec::InputData("),
                 ("main/audio/audio_codec.cc", "bool AudioCodec::OutputData(")]
        specs += [("main/audio/codecs/es8311_audio_codec.cc", signature) for signature in (
            "int Es8311AudioCodec::Read(", "int Es8311AudioCodec::Write(",
            "bool Es8311AudioCodec::PrepareInputCapture(", "bool Es8311AudioCodec::IsOutputDrained(",
            "bool Es8311AudioCodec::OnOutputSent(")]
        run_cpp(CODEC.replace("__METHODS__", "\n".join(method(*spec) for spec in specs)))

    def test_actual_input_task_fences_preparation_reads_and_output_drain(self):
        run_cpp(INPUT.replace("__METHODS__", "\n".join(method("main/audio/audio_service.cc", signature)
                for signature in ("void AudioService::AudioInputTask()", "void AudioService::StartLocalRecording(",
                                  "void AudioService::FenceLocalRecording(", "void AudioService::ReleaseLocalRecordingFence(",
                                  "void AudioService::ReconcileLocalRecording(",
                                  "void AudioService::StopLocalRecording(", "bool AudioService::IsLocalRecordingClosed(",
                                  "bool AudioService::IsLocalInputIdle(", "bool AudioService::IsLocalRecordingReady("))))

    def output_task_program(self):
        program = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1
#define CONFIG_PROVISIONS_LOCAL_CAPTURE 1
#define CONFIG_USE_SERVER_AEC 0
#define ESP_LOGW(...) ((void)0)
constexpr int AUDIO_POWER_CHECK_INTERVAL_MS=1000,AS_EVENT_LOCAL_RECORDING_RUNNING=1;
void esp_timer_stop(int){}void esp_timer_start_periodic(int,int){}void xEventGroupSetBits(int,int){}
struct AudioTask{std::vector<int16_t> pcm{7};uint32_t playback_id=0,media_position_ms=0,timestamp=0,playback_generation=5;};
struct Codec{
 std::atomic<bool> hold_enable{false},enable_entered{false},enable_release{false},hold_write{false},write_entered{false},write_release{false},fail{false},drained{true};
 std::atomic<int> writes{0};
 bool output_enabled(){return !hold_enable;}
 void EnableOutput(bool){enable_entered=true;while(hold_enable&&!enable_release)std::this_thread::sleep_for(100us);}
 bool OutputData(std::vector<int16_t>&){++writes;write_entered=true;while(hold_write&&!write_release)std::this_thread::sleep_for(100us);return !fail;}
 bool IsOutputDrained()const{return drained;}
};
struct AudioService{
 std::mutex audio_queue_mutex_,local_recording_mutex_;std::condition_variable audio_queue_cv_;
 std::deque<std::unique_ptr<AudioTask>> audio_playback_queue_;std::deque<int> audio_decode_queue_;
 std::atomic<bool> service_stopped_{false};std::atomic<uint32_t> local_recording_press_{0},timer_output_owner_{0},local_prepared_press_{0},local_physical_boundary_{0},local_output_boundary_{0};
 bool output_in_flight_=false,decode_in_flight_=false,playback_drained_notified_=false,local_feedback_active_=false;
 uint32_t playback_generation_=5;std::string_view local_feedback_;
 Codec codec;Codec* codec_=&codec;int audio_power_timer_=0,event_group_=0;
 struct{std::function<void()> on_playback_drained;std::function<void(uint32_t)> on_playback_error;std::function<void(uint32_t,uint32_t)> on_playback_progress;}callbacks_;
 struct{uint32_t playback_count=0;}debug_statistics_;std::chrono::steady_clock::time_point last_output_time_;
 void AudioOutputTask();void StartLocalRecording(uint32_t);void FenceLocalRecording(uint32_t);void ReleaseLocalRecordingFence(uint32_t);void ReconcileLocalRecording(uint32_t);bool IsPlaybackDrainedLocked()const;bool MarkPlaybackDrainedLocked();
 void stop(){std::lock_guard<std::mutex> lock(audio_queue_mutex_);service_stopped_=true;audio_queue_cv_.notify_all();}
 void enqueue(){audio_playback_queue_.push_back(std::make_unique<AudioTask>());}
};
__METHODS__
template<class F>void wait_for(F f){auto end=std::chrono::steady_clock::now()+2s;while(!f()&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(100us);assert(f());}
int main(){
 {AudioService a;a.enqueue();a.enqueue();a.codec.hold_enable=true;std::atomic<int> drained=0;a.callbacks_.on_playback_drained=[&]{++drained;};
  std::thread output([&]{a.AudioOutputTask();});wait_for([&]{return a.codec.enable_entered.load();});
  a.FenceLocalRecording(1);a.StartLocalRecording(1);a.codec.enable_release=true;wait_for([&]{return drained==1;});assert(a.codec.writes==0);
  a.stop();output.join();}
 {AudioService a;a.enqueue();a.codec.hold_enable=true;std::atomic<int> drained=0;a.callbacks_.on_playback_drained=[&]{++drained;};
  std::thread output([&]{a.AudioOutputTask();});wait_for([&]{return a.codec.enable_entered.load();});
  a.FenceLocalRecording(1);a.ReleaseLocalRecordingFence(1); // Complete tap before any main event.
  assert(a.local_recording_press_==0&&a.local_physical_boundary_!=a.local_output_boundary_);
  a.codec.enable_release=true;wait_for([&]{return drained==1;});assert(a.codec.writes==0);
  a.FenceLocalRecording(2);a.ReconcileLocalRecording(1); // Old reconciliation cannot allow a newer hold.
  assert(a.local_physical_boundary_!=a.local_output_boundary_);
  a.ReleaseLocalRecordingFence(1);assert(a.local_physical_boundary_==2);
  a.ReleaseLocalRecordingFence(2);a.ReconcileLocalRecording(2);
  assert(a.local_physical_boundary_==a.local_output_boundary_);
  a.stop();output.join();}
 {AudioService a;a.enqueue();a.enqueue();a.codec.hold_write=true;a.codec.drained=false;std::atomic<int> drained=0;a.callbacks_.on_playback_drained=[&]{++drained;};
  std::thread output([&]{a.AudioOutputTask();});wait_for([&]{return a.codec.write_entered.load();});a.FenceLocalRecording(1);a.StartLocalRecording(1);a.codec.write_release=true;
  std::this_thread::sleep_for(20ms);assert(drained==0);{std::lock_guard<std::mutex> l(a.audio_queue_mutex_);assert(!a.IsPlaybackDrainedLocked());}
  a.codec.drained=true;wait_for([&]{return drained==1;});assert(a.codec.writes==1);a.stop();output.join();}
 {AudioService a;a.enqueue();a.enqueue();a.codec.fail=true;std::atomic<int> errors=0,drained=0;
  a.callbacks_.on_playback_error=[&](uint32_t id){assert(id==0);std::lock_guard<std::mutex> l(a.audio_queue_mutex_);assert(!a.IsPlaybackDrainedLocked());++errors;};a.callbacks_.on_playback_drained=[&]{++drained;};
  std::thread output([&]{a.AudioOutputTask();});wait_for([&]{return errors==1&&drained==1;});
  a.stop();output.join();assert(a.codec.writes==1&&a.debug_statistics_.playback_count==0&&a.audio_playback_queue_.empty());}
}
'''
        return program.replace("__METHODS__", "\n".join(method("main/audio/audio_service.cc", signature)
                for signature in ("void AudioService::AudioOutputTask()", "void AudioService::StartLocalRecording(",
                                  "void AudioService::FenceLocalRecording(", "void AudioService::ReleaseLocalRecordingFence(",
                                  "void AudioService::ReconcileLocalRecording(",
                                  "bool AudioService::IsPlaybackDrainedLocked() const",
                                  "bool AudioService::MarkPlaybackDrainedLocked()")))

    def test_actual_output_task_checks_final_ownership_and_physical_drain(self):
        run_cpp(self.output_task_program())

    def test_output_harness_stop_serializes_notification_with_actual_task_wait(self):
        # Stop must serialize its predicate and notification with the queue mutex,
        # just as production AudioService::Stop does. Hold the actual output task
        # immediately before wait to prove the old helper loses its notification.
        boundary = r'''
std::atomic<bool> wait_entered{false},release_wait{false},notified{false},timed_out{false};
struct ControlledConditionVariable {
 std::condition_variable actual;
 void wait(std::unique_lock<std::mutex>& lock) {
  wait_entered=true;
  while(!release_wait)std::this_thread::sleep_for(100us);
  // Only this detector is bounded; production wait and test process limits stay unchanged.
  timed_out=actual.wait_for(lock,200ms)==std::cv_status::timeout;
 }
 template<class Rep,class Period>auto wait_for(std::unique_lock<std::mutex>& lock,const std::chrono::duration<Rep,Period>& duration){return actual.wait_for(lock,duration);}
 void notify_all(){notified=true;actual.notify_all();}
};
'''
        case = r'''
int main(){
 AudioService a;a.playback_drained_notified_=true;
 std::thread output([&]{a.AudioOutputTask();});
 wait_for([&]{return wait_entered.load();});
 std::atomic<bool> stop_started{false};
 std::thread stopping([&]{stop_started=true;a.stop();});
 wait_for([&]{return stop_started.load();});
 if(__EXPECT_LOST_WAKE__)wait_for([&]{return notified.load();});
 release_wait=true;stopping.join();output.join();
 assert(timed_out.load()==__EXPECT_LOST_WAKE__);
}
'''
        fixed = ('void stop(){std::lock_guard<std::mutex> lock(audio_queue_mutex_);'
                 'service_stopped_=true;audio_queue_cv_.notify_all();}')
        original = 'void stop(){service_stopped_=true;audio_queue_cv_.notify_all();}'
        program = self.output_task_program()
        self.assertIn(fixed, program)
        program = program.replace('struct AudioService{', boundary + '\nstruct AudioService{')
        program = program.replace('std::condition_variable audio_queue_cv_;',
                                  'ControlledConditionVariable audio_queue_cv_;')
        program = program[:program.index('int main(){')]
        for locked in (False, True):
            with self.subTest(queue_mutex_held=locked):
                variant = program if locked else program.replace(fixed, original)
                run_cpp(variant + case.replace('__EXPECT_LOST_WAKE__', 'false' if locked else 'true'))


if __name__ == "__main__":
    unittest.main()
