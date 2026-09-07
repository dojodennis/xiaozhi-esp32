"""Actual AudioService/Codec methods with admission and retained worker boundaries.

The hardware driver is controlled by deterministic barriers. This is host evidence,
not an assertion of acoustic delivery or installed-device acceptance.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_provisions_audio_boundaries import method

ROOT = Path(__file__).resolve().parents[2]

PRELUDE = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <stdexcept>
using namespace std::chrono_literals;
#define CONFIG_PROVISIONS_OUTPUT_FENCE_V1 1
#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1
#define CONFIG_PROVISIONS_LOCAL_CAPTURE 1
#define ESP_IDF_VERSION 60000
#define ESP_IDF_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(v) (v)
constexpr int pdFALSE=0,pdTRUE=1,portMAX_DELAY=-1,ESP_AE_ERR_OK=0;
using EventBits_t=int;
struct Event {std::mutex mutex;std::condition_variable cv;int bits=0;};
using EventGroupHandle_t=Event*;
using TaskHandle_t=void*;thread_local int task_identity;
TaskHandle_t xTaskGetCurrentTaskHandle(){return &task_identity;}
using i2s_chan_handle_t=void*;using esp_timer_handle_t=void*;
using esp_ae_rate_cvt_handle_t=void*;using esp_ae_sample_t=void*;
void xEventGroupSetBits(Event* e,int bits){std::lock_guard<std::mutex> l(e->mutex);e->bits|=bits;e->cv.notify_all();}
void xEventGroupClearBits(Event* e,int bits){std::lock_guard<std::mutex> l(e->mutex);e->bits&=~bits;}
int xEventGroupGetBits(Event* e){std::lock_guard<std::mutex> l(e->mutex);return e->bits;}
int xEventGroupWaitBits(Event* e,int bits,int,int,int){std::unique_lock<std::mutex> l(e->mutex);e->cv.wait(l,[&]{return (e->bits&bits)!=0;});return e->bits;}
void vTaskDelay(int ms){std::this_thread::sleep_for(std::chrono::milliseconds(ms));}
void esp_timer_stop(void*){} void esp_timer_start_periodic(void*,int){}
int esp_ae_rate_cvt_reset(void*){return 0;}
int esp_ae_rate_cvt_get_max_out_sample_num(void*,uint32_t n,uint32_t* out){*out=n;return 0;}
int esp_ae_rate_cvt_process(void*,void*,uint32_t,void*,uint32_t*){return 0;}
struct srmodel_list_t{};
struct AudioStreamPacket {int sample_rate=16000,frame_duration=60;uint32_t timestamp=0,
 playback_id=0,media_position_ms=0,voice_upload_generation=0;std::vector<uint8_t> payload{1};};
struct AudioDebugger{};struct AudioEngine{};struct OggDemuxer{};
struct VoiceUploadGate {void Close(){}bool Allows(uint32_t)const{return true;}};
constexpr int ESP_AUDIO_ERR_OK=0,ESP_AUDIO_DEC_RECOVERY_NONE=0;
struct esp_audio_dec_in_raw_t{uint8_t* buffer;uint32_t len,consumed;int frame_recover;};
struct esp_audio_dec_out_frame_t{uint8_t* buffer;uint32_t len,decoded_size;};struct esp_audio_dec_info_t{};
struct esp_audio_enc_in_frame_t{uint8_t* buffer;uint32_t len;};
struct esp_audio_enc_out_frame_t{uint8_t* buffer;uint32_t len,encoded_bytes;};
#include "audio/provisions_audio_admission.h"
using namespace provisions::audio_admission;
struct Barrier {
 std::mutex mutex;std::condition_variable cv;bool entered=false,released=false;
 void enter(){std::unique_lock<std::mutex> lock(mutex);entered=true;cv.notify_all();cv.wait(lock,[&]{return released;});}
 void wait(){std::unique_lock<std::mutex> lock(mutex);assert(cv.wait_for(lock,2s,[&]{return entered;}));}
 void release(){std::lock_guard<std::mutex> lock(mutex);released=true;cv.notify_all();}
};
template<class F>void until(F predicate){const auto end=std::chrono::steady_clock::now()+2s;
 while(!predicate()&&std::chrono::steady_clock::now()<end)std::this_thread::yield();assert(predicate());}
'''

OPUS = r'''
Barrier *decoding=nullptr,*encoding=nullptr;int decoder_result=0;
int esp_opus_dec_decode(void*,esp_audio_dec_in_raw_t* raw,esp_audio_dec_out_frame_t* out,esp_audio_dec_info_t*){
 if(decoding)decoding->enter();raw->consumed=raw->len;out->decoded_size=out->len;return decoder_result;}
int esp_opus_enc_process(void*,esp_audio_enc_in_frame_t*,esp_audio_enc_out_frame_t* out){
 if(encoding)encoding->enter();out->encoded_bytes=1;out->buffer[0]=1;return 0;}
'''

FAKE_CODEC = r'''
struct FakeCodec:AudioCodec {
 std::atomic<bool> rx_closed{true},tx_closed{true},drained{true},fail_close{false},supported{true};
 std::atomic<int> reads{0},writes{0},preparations{0},rx_stops{0},tx_stops{0},meters{0};
 Barrier *reading=nullptr,*preparing=nullptr,*writing=nullptr,*stopping=nullptr,*meter=nullptr;
 const Reservation* recursive_token=nullptr;bool throw_read=false;
 FakeCodec(){input_sample_rate_=16000;output_sample_rate_=16000;}
 bool SupportsOutputFence()const override{return supported;}
 bool IsInputClosedForFence()const override{return rx_closed;}
 bool IsOutputClosedForFence()const override{return tx_closed;}
 bool IsOutputDrained()const override{return drained;}
 bool CloseInputForFence()override{++rx_stops;if(stopping)stopping->enter();if(fail_close)return false;rx_closed=true;input_enabled_=false;return true;}
 bool CloseOutputForFence()override{++tx_stops;if(!drained||fail_close)return false;tx_closed=true;output_enabled_=false;return true;}
 bool EnableOutputAdmitted(const Reservation& token)override{if(!admission_.load()->AllowsPublication(token,Producer::Output))return false;tx_closed=false;output_enabled_=true;return true;}
 bool PrepareInputCapture()override{++preparations;if(preparing)preparing->enter();
  if(!InputContextAllows(Producer::InputPreparation))return false;rx_closed=false;tx_closed=false;input_enabled_=true;return true;}
 bool InputData(std::vector<int16_t>& data)override{const bool ok=AudioCodec::InputData(data);if(ok){++meters;if(meter)meter->enter();}return ok;}
 int Read(int16_t* data,int samples)override{++reads;if(reading)reading->enter();
  if(recursive_token){std::vector<int16_t> retry(1);assert(!InputDataAdmitted(retry,*recursive_token));}
  if(throw_read)throw std::runtime_error("read");std::fill_n(data,samples,123);return samples;}
 int Write(const int16_t*,int samples)override{++writes;if(writing)writing->enter();drained=false;return samples;}
};
'''

ES_DRIVER = r'''
constexpr int ESP_OK=0,ESP_ERR_INVALID_STATE=2,ESP_CODEC_DEV_OK=0,I2S_NUM_0=0,I2S_ROLE_MASTER=0,
 I2S_CLK_SRC_DEFAULT=0,I2S_MCLK_MULTIPLE_256=256,I2S_DATA_BIT_WIDTH_16BIT=16,
 I2S_SLOT_BIT_WIDTH_AUTO=0,I2S_SLOT_MODE_STEREO=2,I2S_STD_SLOT_BOTH=3;
#define ESP_ERROR_CHECK(value) assert((value)==ESP_OK)
#define portENTER_CRITICAL(p) ((p)->lock())
#define portEXIT_CRITICAL(p) ((p)->unlock())
#define portENTER_CRITICAL_ISR(p) portENTER_CRITICAL(p)
#define portEXIT_CRITICAL_ISR(p) portEXIT_CRITICAL(p)
using gpio_num_t=int;struct i2s_event_data_t{};
struct i2s_chan_config_t{int id,role,dma_desc_num,dma_frame_num;bool auto_clear_after_cb,auto_clear_before_cb;int intr_priority;};
struct i2s_std_config_t{struct{uint32_t sample_rate_hz;int clk_src,mclk_multiple;}clk_cfg;
 struct{int data_bit_width,slot_bit_width,slot_mode,slot_mask,ws_width;bool ws_pol,bit_shift;}slot_cfg;
 struct{int mclk,bclk,ws,dout,din;struct{bool mclk_inv,bclk_inv,ws_inv;}invert_flags;}gpio_cfg;};
struct i2s_event_callbacks_t{bool(*on_sent)(i2s_chan_handle_t,i2s_event_data_t*,void*)=nullptr;};
int enabled=0,disabled=0,driver_failure=0,driver_writes=0;void* callback_context=nullptr;i2s_event_callbacks_t driver_callbacks;
int i2s_new_channel(i2s_chan_config_t* cfg,void** tx,void** rx){assert(cfg->dma_desc_num==6);*tx=&enabled;*rx=&disabled;return 0;}
int i2s_channel_init_std_mode(void*,i2s_std_config_t*){return 0;}
int i2s_channel_register_event_callback(void*,i2s_event_callbacks_t* cb,void* ctx){driver_callbacks=*cb;callback_context=ctx;return 0;}
int i2s_channel_enable(void*){++enabled;return driver_failure;}
int i2s_channel_disable(void*){++disabled;return driver_failure;}
int esp_codec_dev_read(void*,void* data,int count){if(driver_failure)return driver_failure;std::fill_n(static_cast<int16_t*>(data),count/2,7);return 0;}
int esp_codec_dev_write(void*,void*,int){++driver_writes;return driver_failure;}
enum esp_codec_dev_type_t{ESP_CODEC_DEV_TYPE_IN=1,ESP_CODEC_DEV_TYPE_OUT=2,ESP_CODEC_DEV_TYPE_IN_OUT=3};
constexpr int GPIO_NUM_NC=-1,ESP_ERR_NO_MEM=-5;
struct esp_codec_dev_cfg_t{esp_codec_dev_type_t dev_type;void* codec_if;void* data_if;};
struct esp_codec_dev_sample_info_t{int bits_per_sample,channel,channel_mask;uint32_t sample_rate;int mclk_multiple;};
int vendor_caps=0,rx_enables=0,vendor_opens=0;bool vendor_input=false,vendor_output=false;
void* esp_codec_dev_new(esp_codec_dev_cfg_t* cfg){vendor_caps=cfg->dev_type;return &vendor_caps;}
int esp_codec_dev_open(void*,esp_codec_dev_sample_info_t*){++vendor_opens;
 if(vendor_caps&ESP_CODEC_DEV_TYPE_IN){++rx_enables;vendor_input=true;}
 if(vendor_caps&ESP_CODEC_DEV_TYPE_OUT)vendor_output=true;
 return driver_failure;}
int esp_codec_dev_close(void*){vendor_input=vendor_output=false;return driver_failure;}
void esp_codec_dev_delete(void*){}
int esp_codec_dev_set_in_gain(void*,float){assert(vendor_caps&ESP_CODEC_DEV_TYPE_IN);return 0;}
int esp_codec_dev_set_out_vol(void*,int){assert(vendor_caps&ESP_CODEC_DEV_TYPE_OUT);return 0;}
void gpio_set_level(int,int){}
struct Es8311AudioCodec:AudioCodec{
 void* codec_if_=this;void* data_if_=this;void* dev_=nullptr;int pa_pin_=-1;bool pa_inverted_=false;std::mutex data_if_mutex_;mutable std::mutex output_dma_mutex_;
 bool output_write_active_=false;uint32_t output_dma_remaining_=0;std::atomic<bool> fence_rx_closed_{true},fence_tx_closed_{true};
 Es8311AudioCodec(){input_sample_rate_=output_sample_rate_=16000;}
 void CreateDuplexChannels(gpio_num_t,gpio_num_t,gpio_num_t,gpio_num_t,gpio_num_t);
 bool SupportsOutputFence()const override{return codec_if_!=nullptr;}
 bool IsInputClosedForFence()const override{return fence_rx_closed_;}bool IsOutputClosedForFence()const override{return fence_tx_closed_;}
 bool IsOutputDrained()const override;bool CloseInputForFence()override;bool CloseOutputForFence()override;
 bool EnableOutputAdmitted(const Reservation&)override;bool PrepareInputCapture()override;
 void EnableInput(bool)override;void EnableOutput(bool)override;void UpdateDeviceState();
 int Read(int16_t*,int)override;int Write(const int16_t*,int)override;
 static bool OnOutputSent(i2s_chan_handle_t,i2s_event_data_t*,void*);
};
'''

SUPPORT = r'''
AudioCodec::AudioCodec(){} AudioCodec::~AudioCodec(){}
void AudioCodec::Start(){} void AudioCodec::SetOutputVolume(int){} void AudioCodec::SetInputGain(float){}
void AudioCodec::EnableInput(bool value){input_enabled_=value;}
void AudioCodec::EnableOutput(bool value){output_enabled_=value;}
AudioService::AudioService(){event_group_=new Event;service_stopped_=false;}
AudioService::~AudioService(){delete event_group_;}
void AudioService::PushTaskToEncodeQueue(AudioTaskType,std::vector<int16_t>&&){}
void AudioService::SetDecodeSampleRate(int rate,int duration){decoder_sample_rate_=rate;decoder_duration_ms_=duration;decoder_frame_size_=8;opus_decoder_=this;}
void AudioService::FillLocalFeedbackLocked(){}
FenceIdentity owner(){FenceIdentity x;for(auto* id:{&x.device_id,&x.lease_id,&x.playback_id,&x.request_id,&x.route_epoch,&x.device_connection_id})(*id)[15]=1;
 x.fence_epoch=1;x.sequence=1;x.response_revision=1;x.checkpoint_sha256[0]=1;return x;}
void init(AudioService& a,FakeCodec& codec){a.codec_=&codec;codec.BindAudioAdmission(a.audio_admission_);}
void open(AudioService& a){auto id=owner();auto g=a.BeginAudioFenceClose();assert(a.HoldAudioFence(id,g));
 a.ServiceInputFence();a.ServiceOutputFence();for(auto ack:{Acknowledgement::Boot,Acknowledgement::Main,Acknowledgement::Notification,Acknowledgement::Recorder})assert(a.audio_admission_.Acknowledge(ack,g));
 assert(a.GetAudioFenceSnapshot(id).metadata.metadata_closed);assert(a.OpenAudioFenceAfterTerminal(id,g));}
void stop_input(AudioService& a){a.service_stopped_=true;xEventGroupSetBits(a.event_group_,AS_EVENT_AUDIO_INPUT_STOP_REQUEST);}
void stop_output(AudioService& a){std::lock_guard<std::mutex> l(a.audio_queue_mutex_);a.service_stopped_=true;a.audio_queue_cv_.notify_all();}
std::unique_ptr<AudioStreamPacket> packet(uint32_t owner=0){auto p=std::make_unique<AudioStreamPacket>();p->playback_id=owner;return p;}
void queue_pcm(AudioService& a,uint64_t ordinary=0,uint32_t timer=0){std::lock_guard<std::mutex> l(a.audio_queue_mutex_);auto p=std::make_unique<AudioTask>();p->pcm={1};p->ordinary_owner=ordinary;p->playback_id=timer;p->playback_generation=a.playback_generation_;a.audio_playback_queue_.push_back(std::move(p));a.audio_queue_cv_.notify_all();}
'''

CASES = r'''
int main(int argc,char** argv){assert(argc==2);std::string name=argv[1];
 if(name=="boot"){
  AudioService a;FakeCodec codec;init(a,codec);assert(!a.BeginOrdinaryOutput(1));a.FenceLocalRecording(1);assert(!a.ReserveCaptureParent(1));
  auto id=owner();auto g=a.BeginAudioFenceClose();assert(a.HoldAudioFence(id,g));
  for(auto ack:{Acknowledgement::Boot,Acknowledgement::Main,Acknowledgement::Notification,Acknowledgement::Recorder,Acknowledgement::Engine})a.audio_admission_.Acknowledge(ack,g);
  assert(!a.OpenAudioFenceAfterTerminal(id,g));codec.fail_close=true;a.ServiceInputFence();a.ServiceOutputFence();assert(!a.GetAudioFenceSnapshot(id).input_closed);
  codec.fail_close=false;a.ServiceInputFence();a.ServiceOutputFence();assert(a.OpenAudioFenceAfterTerminal(id,g));
  int rx=codec.rx_stops,tx=codec.tx_stops;a.ServiceInputFence();a.ServiceOutputFence();assert(codec.rx_stops==rx&&codec.tx_stops==tx);
  assert(!a.ReserveCaptureParent(1));a.FenceLocalRecording(2);assert(a.ReserveCaptureParent(2));
 }else if(name=="capture_worker"){
  AudioService a;FakeCodec codec;init(a,codec);open(a);a.FenceLocalRecording(1);auto p=a.ReserveCaptureParent(1);assert(p&&p->InputGeneration()==1);
  assert(!a.BeginOrdinaryOutput(1));TimerIdentity timer;timer.lease_id[0]=1;assert(!a.ReserveTimerPreparation(timer));
  Reservation worker,encode,upload;assert(a.ReserveCaptureWork(p,Producer::CaptureWork,worker));assert(a.ReserveCaptureWork(p,Producer::Encode,encode));
  assert(!a.ReserveCaptureWork(p,Producer::CaptureUpload,upload));
  std::thread input([&]{a.AudioInputTask();});assert(a.SealCaptureInput(p));
  until([&]{return a.capture_closed_input_generation_==p->InputGeneration();});
  assert(!a.GetCaptureClosureSnapshot(p).workers_closed&&!a.ReleaseCaptureParent(p));
  assert(a.CompleteCaptureWork(p,encode));assert(!a.GetCaptureClosureSnapshot(p).workers_closed);
  assert(a.CompleteCaptureWork(p,worker));assert(a.GetCaptureClosureSnapshot(p).workers_closed);
  assert(a.ReserveCaptureWork(p,Producer::CaptureUpload,upload));assert(a.GetCaptureClosureSnapshot(p).upload==1&&!a.ReleaseCaptureParent(p));
  auto generation=a.BeginAudioFenceClose();assert(!a.CanPublishCaptureWork(p,upload));assert(a.CompleteCaptureWork(p,upload));assert(a.ReleaseCaptureParent(p));
  assert(!a.ReleaseCaptureParent(p));assert(a.audio_admission_.Snapshot().blocked&&a.audio_admission_.Snapshot().generation==generation);
  stop_input(a);input.join();
 }else if(name=="replay"){
  AudioService a;FakeCodec codec;init(a,codec);open(a);
  assert(!a.ReserveSealedReplayParent(0,std::nullopt,false));assert(!a.ReserveSealedReplayParent(3,std::nullopt,false));assert(!a.ReserveSealedReplayParent(3,4,true));
  codec.rx_closed=false;assert(!a.ReserveSealedReplayParent(0,std::nullopt,true));codec.rx_closed=true;
  auto p=a.ReserveSealedReplayParent(0,std::nullopt,true);assert(p&&p->IsSealedReplay()&&!p->SourceInputGeneration());
  assert(p->Press()==0&&p->InputGeneration()==1&&a.GetCaptureClosureSnapshot(p).workers_closed);
  assert(!a.StartLocalRecording(0,p));a.FenceLocalRecording(7);assert(!a.ReserveCaptureParent(7));assert(a.ReleaseCaptureParent(p));assert(!a.ReserveCaptureParent(7));
  a.ReleaseLocalRecordingFence(7);a.ReconcileLocalRecording(7);auto next=a.ReserveSealedReplayParent(6,4,false);assert(next&&next->InputGeneration()==2&&next->SourceInputGeneration()==4);
  assert(!a.GetCaptureClosureSnapshot(p).exact_parent&&!a.ReleaseCaptureParent(p));assert(a.ReleaseCaptureParent(next));
 }else if(name=="read_close"||name=="prepare_close"||name=="append_close"){
  AudioService a;FakeCodec codec;init(a,codec);open(a);Barrier held;std::atomic<int> appended=0;
  if(name=="read_close")codec.reading=&held;if(name=="prepare_close")codec.preparing=&held;
  a.callbacks_.on_recording_audio=[&](uint32_t,const int16_t*,size_t,size_t){++appended;if(name=="append_close")held.enter();};
  a.FenceLocalRecording(1);auto p=a.ReserveCaptureParent(1);assert(p);assert(a.StartLocalRecording(1,p));std::thread input([&]{a.AudioInputTask();});held.wait();
  auto g=a.BeginAudioFenceClose();assert(a.audio_admission_.Snapshot().generation==g);assert(!a.GetCaptureClosureSnapshot(p).workers_closed);
  assert(!a.ReleaseCaptureParent(p));assert(a.input_closed_generation_!=g);held.release();
  until([&]{return a.GetCaptureClosureSnapshot(p).workers_closed;});assert(a.ReleaseCaptureParent(p));
  if(name!="append_close")assert(appended==0);stop_input(a);input.join();
 }else if(name=="release_only"){
  AudioService a;FakeCodec codec;init(a,codec);open(a);Barrier read;codec.reading=&read;std::atomic<int> appended=0;
  a.callbacks_.on_recording_audio=[&](uint32_t,const int16_t*,size_t,size_t){++appended;};
  a.FenceLocalRecording(1);auto p=a.ReserveCaptureParent(1);assert(p&&a.StartLocalRecording(1,p));std::thread input([&]{a.AudioInputTask();});read.wait();
  a.ReleaseLocalRecordingFence(1);read.release();until([&]{return a.IsLocalInputIdle();});assert(appended==0&&!a.GetCaptureClosureSnapshot(p).workers_closed);
  assert(a.SealCaptureInput(p));until([&]{return a.GetCaptureClosureSnapshot(p).workers_closed;});assert(a.ReleaseCaptureParent(p));stop_input(a);input.join();
 }else if(name=="ordinary"){
  AudioService a;FakeCodec codec;init(a,codec);open(a);assert(a.BeginOrdinaryOutput(10));assert(!a.BeginOrdinaryOutput(11));
  a.FenceLocalRecording(1);assert(!a.ReserveCaptureParent(1));a.ReleaseLocalRecordingFence(1);a.ReconcileLocalRecording(1);
  assert(!a.PushPacketToDecodeQueue(packet()));assert(!a.PushTimerPacket(7,packet(7)));assert(!a.PushOrdinaryPacket(9,packet()));
  assert(a.PushOrdinaryPacket(10,packet()));assert(a.SealOrdinaryOutput(10));assert(!a.PushOrdinaryPacket(10,packet()));assert(!a.IsOrdinaryOutputClosed(10));
  a.audio_decode_queue_.clear();codec.drained=false;assert(!a.IsOrdinaryOutputClosed(10));codec.drained=true;
  assert(a.IsOrdinaryOutputClosed(10));assert(!a.RetireOrdinaryOutput(9));assert(a.RetireOrdinaryOutput(10));assert(!a.BeginOrdinaryOutput(10));
  assert(a.BeginOrdinaryOutput(11));assert(!a.RetireOrdinaryOutput(10));
 }else if(name=="output_close"){
  AudioService a;FakeCodec codec;init(a,codec);open(a);Barrier write;codec.writing=&write;assert(a.BeginOrdinaryOutput(1));queue_pcm(a,1);
  std::thread output([&]{a.AudioOutputTask();});write.wait();auto g=a.BeginAudioFenceClose();a.ServiceInputFence();assert(!a.IsOrdinaryOutputClosed(1));
  assert(a.output_closed_generation_!=g);write.release();until([&]{return codec.writes==1&&!a.output_in_flight_;});assert(!a.IsOrdinaryOutputClosed(1));
  codec.drained=true;until([&]{return a.output_closed_generation_==g;});assert(a.IsOrdinaryOutputClosed(1));assert(a.RetireOrdinaryOutput(1));stop_output(a);output.join();
 }else if(name=="timer"){
  AudioService a;FakeCodec codec;init(a,codec);open(a);TimerIdentity timer;timer.lease_id[0]=1;
  assert(a.ReserveTimerPreparation(timer));assert(!a.BeginOrdinaryOutput(1));a.FenceLocalRecording(1);assert(!a.ReserveCaptureParent(1));
  a.ReleaseLocalRecordingFence(1);a.ReconcileLocalRecording(1);assert(a.ClaimTimerOutput(8));assert(!a.PushPacketToDecodeQueue(packet(8)));
  assert(!a.PushTimerPacket(9,packet(9)));assert(a.PushTimerPacket(8,packet(8)));assert(!a.ReleaseTimerOutput(8));
  a.audio_decode_queue_.clear();assert(a.ReleaseTimerOutput(8));assert(a.ReleaseTimerPreparation(timer));
  auto g=a.BeginAudioFenceClose();TimerIdentity retained;retained.kind=TimerKind::Alarm;retained.lease_id[0]=1;retained.playback_id[0]=2;retained.timer_id[0]=3;retained.timer_revision=1;retained.attempt=1;
  assert(a.audio_admission_.RegisterRetainedTimer(retained,g));assert(a.ClaimRetainedTimerOutput(9,retained));assert(!a.PushTimerPacket(9,packet(9)));
  assert(a.ReleaseTimerOutput(9));assert(a.audio_admission_.Snapshot().timer_recovery_retained);
 }else if(name=="codec_context"){
  Gate gate;auto id=owner();auto g=gate.BeginClose();assert(gate.Hold(id,g));for(unsigned i=0;i<5;++i)gate.Acknowledge(static_cast<Acknowledgement>(i),g);assert(gate.OpenAfterTerminal(id,g));
  Reservation parent,read,prep;assert(gate.Reserve(Producer::Capture,parent));assert(gate.ReserveCaptureMedia(parent,Producer::InputRead,read));assert(gate.ReserveCaptureMedia(parent,Producer::InputPreparation,prep));
  FakeCodec codec;codec.BindAudioAdmission(gate);std::vector<int16_t> pcm(8);assert(!codec.InputData(pcm));assert(!codec.InputDataAdmitted(pcm,prep));
  assert(codec.PrepareInputCaptureAdmitted(prep));codec.recursive_token=&read;assert(codec.InputDataAdmitted(pcm,read));assert(codec.meters==1);codec.recursive_token=nullptr;
  Barrier held;codec.reading=&held;std::thread task([&]{assert(codec.InputDataAdmitted(pcm,read));});held.wait();std::vector<int16_t> other(8);
  assert(!codec.InputData(other)&&!codec.InputDataAdmitted(other,read));held.release();task.join();codec.reading=nullptr;
  assert(!codec.InputData(other));codec.throw_read=true;try{codec.InputDataAdmitted(other,read);assert(false);}catch(const std::runtime_error&){}
  codec.throw_read=false;assert(!codec.InputData(other));assert(codec.InputDataAdmitted(other,read));
  gate.SealCaptureInput(parent);assert(!codec.InputDataAdmitted(other,read)&&!codec.PrepareInputCaptureAdmitted(prep));
 }else if(name=="queue_wait"){
  AudioService a;FakeCodec codec;init(a,codec);open(a);assert(a.BeginOrdinaryOutput(1));for(int i=0;i<MAX_DECODE_PACKETS_IN_QUEUE;++i)assert(a.PushOrdinaryPacket(1,packet()));
  bool accepted=true;std::thread writer([&]{accepted=a.PushFencedPacket(packet(),true,1);});until([&]{return a.audio_admission_.Snapshot().active==2;});
  assert(a.SealOrdinaryOutput(1,true));writer.join();assert(!accepted&&a.audio_decode_queue_.empty());assert(a.RetireOrdinaryOutput(1));assert(a.BeginOrdinaryOutput(2));
 }else if(name=="decode_close"||name=="decode_failure"){
  AudioService a;FakeCodec codec;init(a,codec);open(a);Barrier held;decoding=&held;assert(a.BeginOrdinaryOutput(1));assert(a.PushOrdinaryPacket(1,packet()));
  std::atomic<int> errors=0;a.callbacks_.on_playback_error=[&](uint32_t){assert(a.decode_in_flight_);assert(!a.IsOrdinaryOutputClosed(1));++errors;};
  if(name=="decode_failure")decoder_result=-1;std::thread opus([&]{a.OpusCodecTask();});held.wait();
  if(name=="decode_close")a.BeginAudioFenceClose();else assert(a.SealOrdinaryOutput(1));
  assert(!a.IsOrdinaryOutputClosed(1));held.release();until([&]{std::lock_guard<std::mutex> l(a.audio_queue_mutex_);return !a.decode_in_flight_;});
  assert(a.audio_playback_queue_.empty());assert(errors==(name=="decode_failure"?1:0));assert(a.IsOrdinaryOutputClosed(1));assert(a.RetireOrdinaryOutput(1));stop_output(a);opus.join();
 }else if(name=="encode_close"){
  AudioService a;FakeCodec codec;init(a,codec);open(a);Barrier held;encoding=&held;
  auto task=std::make_unique<AudioTask>();task->type=kAudioTaskTypeEncodeToSendQueue;task->pcm={1};a.audio_encode_queue_.push_back(std::move(task));
  a.opus_encoder_=&a;a.encoder_frame_size_=1;a.encoder_outbuf_size_=8;
  std::thread opus([&]{a.OpusCodecTask();});held.wait();a.FenceLocalRecording(1);assert(!a.ReserveCaptureParent(1));auto g=a.BeginAudioFenceClose();
  assert(a.audio_admission_.Snapshot().active_by_producer[static_cast<size_t>(Producer::Encode)]==1);held.release();
  until([&]{return a.audio_admission_.Snapshot().active==0;});assert(a.audio_send_queue_.empty());assert(a.audio_admission_.Snapshot().generation==g);stop_output(a);opus.join();
 }else if(name=="es_dma"){
  Es8311AudioCodec codec;Gate gate;codec.BindAudioAdmission(gate);codec.CreateDuplexChannels(0,1,2,3,4);assert(enabled==0&&driver_callbacks.on_sent);
  auto id=owner();auto g=gate.BeginClose();assert(gate.Hold(id,g));for(unsigned i=0;i<5;++i)gate.Acknowledge(static_cast<Acknowledgement>(i),g);assert(gate.OpenAfterTerminal(id,g));
  std::vector<int16_t> pcm(240,7);codec.EnableInput(true);codec.EnableOutput(true);assert(enabled==0&&!codec.PrepareInputCapture()&&!codec.InputData(pcm)&&!codec.OutputData(pcm));
  Reservation capture,prep,read;assert(gate.Reserve(Producer::Capture,capture));assert(gate.ReserveCaptureMedia(capture,Producer::InputPreparation,prep));assert(gate.ReserveCaptureMedia(capture,Producer::InputRead,read));
  assert(codec.PrepareInputCaptureAdmitted(prep)&&enabled==3&&!codec.IsInputClosedForFence());assert(vendor_caps==ESP_CODEC_DEV_TYPE_IN&&rx_enables==1);assert(codec.InputDataAdmitted(pcm,read));
  gate.SealCaptureInput(capture);assert(!codec.PrepareInputCaptureAdmitted(prep));assert(gate.Complete(prep)&&gate.Complete(read)&&gate.Complete(capture));assert(codec.CloseInputForFence());
  Reservation ordinary,output;assert(gate.Reserve(Producer::OrdinaryOutput,ordinary));assert(gate.ReserveMedia(ordinary,Producer::Output,output));
  assert(codec.EnableOutputAdmitted(output)&&codec.OutputDataAdmitted(pcm,output)&&driver_writes==1);assert(vendor_caps==ESP_CODEC_DEV_TYPE_OUT&&rx_enables==1&&!vendor_input);assert(!codec.IsOutputDrained());gate.BeginClose();
  for(int i=0;i<5;++i){assert(!codec.CloseOutputForFence());driver_callbacks.on_sent(nullptr,nullptr,callback_context);assert(!codec.IsOutputDrained());}
  driver_callbacks.on_sent(nullptr,nullptr,callback_context);assert(codec.IsOutputDrained());driver_failure=-1;assert(!codec.CloseOutputForFence()&&!codec.IsOutputClosedForFence());
  driver_failure=0;assert(codec.CloseOutputForFence()&&codec.IsOutputClosedForFence());assert(!codec.OutputDataAdmitted(pcm,output));assert(driver_writes==1);
 }else if(name=="unsupported_testing"){
  AudioService a;FakeCodec codec;init(a,codec);open(a);a.audio_testing_queue_.push_back(packet());a.EnableAudioTesting(false);assert(a.audio_testing_queue_.empty()&&a.audio_decode_queue_.empty());
  a.EnableAudioTesting(true);assert(!(xEventGroupGetBits(a.event_group_)&AS_EVENT_AUDIO_TESTING_RUNNING));
  a.audio_engine_initialized_=true;a.BeginAudioFenceClose();a.ServiceInputFence();a.ServiceOutputFence();assert(!a.GetAudioFenceSnapshot(owner()).supported);
 }else assert(false);
}
'''


def header(path):
    return re.sub(r'^#include[^\n]*\n', '', (ROOT / path).read_text(), flags=re.M)


def program():
    codec = header('main/audio/audio_codec.h')
    service = 'public:'.join(header('main/audio/audio_service.h').rsplit('private:', 1))
    # CapturePermit retains its actual private token. Only the containing service
    # is made test-visible; a copied caller DTO can never become a permit.
    service = service.replace('friend class AudioService;', 'friend class AudioService;')
    methods = [method('main/audio/audio_codec.cc', sig) for sig in (
        'bool AudioCodec::InputData(', 'bool AudioCodec::OutputData(',
        'bool AudioCodec::InputContextAllows(', 'bool AudioCodec::InputDataAdmitted(',
        'bool AudioCodec::PrepareInputCaptureAdmitted(', 'bool AudioCodec::OutputContextAllows(',
        'bool AudioCodec::OutputDataAdmitted(')]
    signatures = (
        'uint64_t AudioService::BeginAudioFenceClose(', 'bool AudioService::HoldAudioFence(',
        'void AudioService::ServiceInputFence(', 'void AudioService::ServiceOutputFence(',
        'AudioService::FenceSnapshot AudioService::GetAudioFenceSnapshot(',
        'bool AudioService::OpenAudioFenceAfterTerminal(', 'bool AudioService::ReserveTimerPreparation(',
        'bool AudioService::ReleaseTimerPreparation(', 'bool AudioService::ClaimRetainedTimerOutput(',
        'const provisions::audio_admission::Reservation* AudioService::TimerParentLocked(',
        'const provisions::audio_admission::Reservation* AudioService::OutputParentLocked(',
        'void AudioService::DiscardAudioTesting(', 'bool AudioService::BeginOrdinaryOutput(',
        'bool AudioService::PushOrdinaryPacket(', 'bool AudioService::SealOrdinaryOutput(',
        'bool AudioService::OrdinaryClosedLocked(', 'bool AudioService::IsOrdinaryOutputClosed(',
        'bool AudioService::RetireOrdinaryOutput(', 'bool AudioService::PushFencedPacket(',
        'bool AudioService::PushTimerPacket(', 'AudioService::CapturePermitPtr AudioService::ReserveCaptureParent(',
        'AudioService::CapturePermitPtr AudioService::ReserveSealedReplayParent(',
        'bool AudioService::StartLocalRecording(', 'bool AudioService::SealCaptureInput(',
        'bool AudioService::ReserveCaptureWork(', 'bool AudioService::CanPublishCaptureWork(',
        'bool AudioService::CompleteCaptureWork(', 'bool AudioService::ReleaseCaptureParent(',
        'AudioService::CaptureClosureSnapshot AudioService::CaptureClosedLocked(',
        'AudioService::CaptureClosureSnapshot AudioService::GetCaptureClosureSnapshot(',
        'void AudioService::FenceLocalRecording(', 'void AudioService::ReleaseLocalRecordingFence(',
        'void AudioService::ReconcileLocalRecording(', 'void AudioService::StopLocalRecording(',
        'bool AudioService::IsLocalInputIdle(', 'bool AudioService::IsLocalRecordingClosed(',
        'bool AudioService::IsLocalRecordingReady(', 'bool AudioService::IsPlaybackDrainedLocked(',
        'bool AudioService::MarkPlaybackDrainedLocked(', 'void AudioService::AudioInputTask(',
        'void AudioService::AudioOutputTask(', 'bool AudioService::ReadAudioData(',
        'bool AudioService::PushPacketToDecodeQueue(', 'void AudioService::OpusCodecTask(')
    methods += [method('main/audio/audio_service.cc', sig) for sig in signatures]
    # The unsupported feature path returns before the unrelated legacy replay
    # implementation; preprocess that real branch without stubbing its engine.
    testing = method('main/audio/audio_service.cc', 'void AudioService::EnableAudioTesting(')
    methods.append(testing[:testing.index('#endif')] + '#endif\n}')
    methods += [method('main/audio/codecs/es8311_audio_codec.cc', sig) for sig in (
        'void Es8311AudioCodec::CreateDuplexChannels(', 'void Es8311AudioCodec::EnableInput(',
        'void Es8311AudioCodec::EnableOutput(', 'int Es8311AudioCodec::Read(',
        'int Es8311AudioCodec::Write(', 'bool Es8311AudioCodec::PrepareInputCapture(',
        'bool Es8311AudioCodec::IsOutputDrained(', 'bool Es8311AudioCodec::OnOutputSent(',
        'bool Es8311AudioCodec::CloseInputForFence(', 'bool Es8311AudioCodec::CloseOutputForFence(',
        'bool Es8311AudioCodec::EnableOutputAdmitted(', 'void Es8311AudioCodec::UpdateDeviceState(')]
    return PRELUDE + OPUS + codec + FAKE_CODEC + ES_DRIVER + service + SUPPORT + '\n'.join(methods) + CASES


class AudioFenceIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='orbit-audio-fence-actual-')
        cls.path = Path(cls.directory.name)
        (cls.path / 'test.cc').write_text(program())
        result = subprocess.run(['c++', '-std=c++17', '-pthread', '-fsanitize=address,undefined',
                                 '-fno-omit-frame-pointer', '-I', str(ROOT / 'main'),
                                 str(cls.path / 'test.cc'),
                                 str(ROOT / 'main/audio/provisions_audio_admission.cc'),
                                 '-o', str(cls.path / 'test')], capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def run_case(self, name):
        result = subprocess.run([str(self.path / 'test'), name], capture_output=True, text=True,
                                timeout=15, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_boot_needs_actual_worker_closure_and_rejected_press_never_retries(self): self.run_case('boot')
    def test_capture_retains_recorder_encode_and_upload_until_actual_completion(self): self.run_case('capture_worker')
    def test_replay_allocates_current_parent_and_preserves_optional_historical_lineage(self): self.run_case('replay')
    def test_input_read_in_flight_prevents_closure(self): self.run_case('read_close')
    def test_input_preparation_in_flight_prevents_closure(self): self.run_case('prepare_close')
    def test_append_callback_in_flight_prevents_closure(self): self.run_case('append_close')
    def test_only_actual_release_fence_discards_read_before_main_reconciliation(self): self.run_case('release_only')
    def test_ordinary_exact_parent_sealing_and_retirement(self): self.run_case('ordinary')
    def test_output_write_and_physical_tail_prevent_closure(self): self.run_case('output_close')
    def test_timer_preparation_and_cleanup_cannot_lend_media_authority(self): self.run_case('timer')
    def test_codec_context_denies_other_task_reentrancy_late_and_failed_calls(self): self.run_case('codec_context')
    def test_queue_wait_revalidates_exact_owner_and_seal(self): self.run_case('queue_wait')
    def test_actual_decoder_completion_cannot_publish_after_close(self): self.run_case('decode_close')
    def test_actual_decode_failure_reports_error_before_drain(self): self.run_case('decode_failure')
    def test_actual_encoder_completion_cannot_publish_after_close(self): self.run_case('encode_close')
    def test_actual_es8311_boot_stays_disabled_and_requires_all_six_dma_completions(self): self.run_case('es_dma')
    def test_unsupported_testing_discards_and_engine_prevents_readiness(self): self.run_case('unsupported_testing')
