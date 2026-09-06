"""Frozen F1 wire, real NVS adapter and alarm controller with real ES8311 DMA methods."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from test_provisions_audio_boundaries import CODEC, method, run_cpp

ROOT = Path(__file__).resolve().parents[2]
HEADERS = {
    "nvs.h": r'''
#pragma once
#include <cstddef>
using nvs_handle_t=unsigned;
constexpr int ESP_OK=0,NVS_READONLY=0,NVS_READWRITE=1,ESP_ERR_NVS_NOT_FOUND=9;
int nvs_open(const char*,int,nvs_handle_t*);void nvs_close(nvs_handle_t);
int nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);
int nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);
int nvs_commit(nvs_handle_t);int nvs_erase_key(nvs_handle_t,const char*);
''',
    "psa/crypto.h": r'''
#pragma once
#include <openssl/sha.h>
#include <cstddef>
#include <cstdint>
constexpr int PSA_SUCCESS=0,PSA_ALG_SHA_256=1;
struct psa_hash_operation_t {SHA256_CTX ctx;bool active=false;};
#define PSA_HASH_OPERATION_INIT {}
inline int psa_crypto_init(){return 0;}
inline int psa_hash_setup(psa_hash_operation_t* p,int){p->active=true;return SHA256_Init(&p->ctx)==1?0:-1;}
inline int psa_hash_abort(psa_hash_operation_t* p){p->active=false;return 0;}
inline int psa_hash_update(psa_hash_operation_t* p,const uint8_t* b,size_t n){return p->active&&SHA256_Update(&p->ctx,b,n)==1?0:-1;}
inline int psa_hash_finish(psa_hash_operation_t* p,uint8_t* b,size_t n,size_t* actual){if(!p->active||n<32)return -1;p->active=false;*actual=32;return SHA256_Final(b,&p->ctx)==1?0:-1;}
'''
}
AUDIO = r'''
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>
using namespace std::chrono_literals;
#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1
#define CONFIG_PROVISIONS_LOCAL_CAPTURE 1
#define CONFIG_USE_SERVER_AEC 0
#define ESP_LOGW(...) ((void)0)
constexpr int AUDIO_POWER_CHECK_INTERVAL_MS=1000,MAX_DECODE_PACKETS_IN_QUEUE=20;
void esp_timer_stop(int){}void esp_timer_start_periodic(int,int){}
void esp_opus_dec_reset(void*){}
std::function<void()> before_decode_queue_lock,before_decode_queue_wait;
struct AudioStreamPacket {uint32_t playback_id=0,media_position_ms=0;std::vector<uint8_t> payload;};
struct AudioTask {std::vector<int16_t> pcm=std::vector<int16_t>(1440);uint32_t playback_id=0,media_position_ms=0,timestamp=0,playback_generation=0;};
struct AudioService {
 std::mutex audio_queue_mutex_,decoder_mutex_;std::condition_variable audio_queue_cv_;
 std::deque<std::unique_ptr<AudioTask>> audio_playback_queue_;
 std::deque<std::unique_ptr<AudioStreamPacket>> audio_decode_queue_;
 std::deque<int> timestamp_queue_,audio_testing_queue_;
 std::atomic<bool> service_stopped_{false};
 std::atomic<uint32_t> timer_output_owner_{0},local_input_press_{0},local_recording_press_{0},local_physical_boundary_{0},local_output_boundary_{0};
 bool output_in_flight_=false,decode_in_flight_=false,playback_drained_notified_=false,local_feedback_active_=false;
 uint32_t playback_generation_=0;std::string_view local_feedback_;void* opus_decoder_=nullptr;
 Es8311AudioCodec codec;Es8311AudioCodec* codec_=&codec;int audio_power_timer_=0;
 struct{std::function<void()> on_playback_drained;std::function<void(uint32_t)> on_playback_error;std::function<void(uint32_t,uint32_t)> on_playback_progress;}callbacks_;
 struct{uint32_t playback_count=0;}debug_statistics_;std::chrono::steady_clock::time_point last_output_time_;
 void AudioOutputTask();bool IsPlaybackIdle();bool IsPlaybackDrainedLocked()const;bool MarkPlaybackDrainedLocked();void ResetDecoder();
 bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet,bool wait=false);
 __OWNER_METHODS__
 void DecodeOne(bool fail=false) {
  std::lock_guard<std::mutex> lock(audio_queue_mutex_);assert(!audio_decode_queue_.empty());
  auto packet=std::move(audio_decode_queue_.front());audio_decode_queue_.pop_front();
  if(!fail){auto task=std::make_unique<AudioTask>();task->playback_id=packet->playback_id;task->media_position_ms=packet->media_position_ms;task->playback_generation=playback_generation_;audio_playback_queue_.push_back(std::move(task));}
  audio_queue_cv_.notify_all();
 }
 void stop(){service_stopped_=true;audio_queue_cv_.notify_all();}
};
__AUDIO_METHODS__
'''

PROGRAM = r'''
#include "provisions_timer_player.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <map>
#include <iostream>
#include <nvs.h>
__CODEC__
__AUDIO__
using namespace provisions::timers;
std::string disk,pending;bool namespace_present=false,fail_open=false,fail_read=false,fail_set=false,fail_commit=false,fail_erase=false,corrupt_readback=false;
int commits=0,sets=0,erases=0;
int nvs_open(const char* ns,int mode,nvs_handle_t* handle){assert(std::string(ns)=="orbit_tmr_v1");if(fail_open)return -1;if(!namespace_present&&mode==NVS_READONLY)return ESP_ERR_NVS_NOT_FOUND;namespace_present=true;*handle=1;pending=disk;return 0;}
void nvs_close(nvs_handle_t){}
int nvs_get_blob(nvs_handle_t,const char* key,void* out,size_t* size){assert(std::string(key)=="attempt");if(fail_read)return -1;if(disk.empty())return ESP_ERR_NVS_NOT_FOUND;if(!out){*size=disk.size();return 0;}if(*size<disk.size())return -1;std::memcpy(out,disk.data(),disk.size());*size=disk.size();if(corrupt_readback)static_cast<char*>(out)[0]='!';return 0;}
int nvs_set_blob(nvs_handle_t,const char* key,const void* p,size_t n){assert(std::string(key)=="attempt");++sets;if(fail_set)return -1;pending.assign(static_cast<const char*>(p),n);return 0;}
int nvs_commit(nvs_handle_t){++commits;if(fail_commit)return -1;disk=pending;return 0;}
int nvs_erase_key(nvs_handle_t,const char* key){assert(std::string(key)=="attempt");++erases;if(fail_erase)return -1;pending.clear();return 0;}
void reset(){disk.clear();pending.clear();namespace_present=fail_open=fail_read=fail_set=fail_commit=fail_erase=corrupt_readback=false;commits=sets=erases=0;write_error=0;}
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
Json json(const std::string& text){Json result(cJSON_Parse(text.c_str()),cJSON_Delete);assert(result);return result;}
std::string uuid(unsigned n){char b[40];std::snprintf(b,sizeof(b),"00000000-0000-0000-0000-%012x",n);return b;}
void replace(cJSON* o,const char* key,const std::string& value){auto v=json(value);assert(cJSON_ReplaceItemInObjectCaseSensitive(o,key,v.release()));}
std::vector<uint8_t> packet{0x18,0x00,0x55};
std::string hash(unsigned count=1){SHA256_CTX ctx;SHA256_Init(&ctx);uint8_t length[2]={3,0};for(unsigned i=0;i<count;++i){SHA256_Update(&ctx,length,2);SHA256_Update(&ctx,packet.data(),packet.size());}unsigned char out[32];SHA256_Final(out,&ctx);char b[65];for(size_t i=0;i<32;++i)std::snprintf(b+2*i,3,"%02x",out[i]);return b;}
Alarm alarm(unsigned count=1){Alarm a;a.session_id=uuid(1);a.lease_id=uuid(2);a.playback_id=uuid(3);a.timer_id=uuid(4);a.timer_revision=1;a.attempt=1;a.spoken_number=7;a.label="Pasta";a.audio_sha256=hash(count);a.packet_count=count;return a;}
Json envelope(const Alarm& a){auto r=json(RecordJson({a,Outcome::Unknown}));return Json(cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(r.get(),"alarm"),true),cJSON_Delete);}
Json tts(const Alarm& a,const std::string& state){std::string text="{\"type\":\"tts\",\"state\":\""+state+"\",\"session_id\":\""+a.session_id+"\",\"playback_id\":\""+a.playback_id+"\",\"timer_id\":\""+a.timer_id+"\",\"timer_revision\":"+std::to_string(a.timer_revision)+",\"attempt\":"+std::to_string(a.attempt);if(state=="sentence_start")text+=",\"text\":\"Timer 7, pasta is ready.\"";return json(text+"}");}
Json ack(const Record& r,const std::string& session){auto j=json(ReceiptJson(r,session));replace(j.get(),"action","\"drain_ack\"");cJSON_DeleteItemFromObjectCaseSensitive(j.get(),"outcome");cJSON_DeleteItemFromObjectCaseSensitive(j.get(),"output_drained");cJSON_AddStringToObject(j.get(),"status","accepted");return j;}
Record stored(){Record r;assert(ParseRecord(disk,r));return r;}
struct Harness {
 NvsStore store;Player player{store};Es8311AudioCodec codec;
 uint32_t owner=0,press=0;bool ready=true,negotiated=true,accept_queue=true;int began=0,cancels=0,ended=0;
 int64_t time=1000000;std::string session=uuid(1);std::vector<std::string> sent;
 std::deque<std::pair<uint32_t,uint32_t>> queued;
 Harness(){Player::Hooks h;
  h.claim=[&](uint32_t id){if(owner)return false;owner=id;return true;};
  h.release=[&](uint32_t id){if(owner!=id)return false;owner=0;return true;};
  h.cancel=[&]{++cancels;queued.clear();};
  h.drained=[&]{return queued.empty()&&codec.IsOutputDrained();};
  h.queue=[&](uint32_t id,uint32_t n,const std::vector<uint8_t>& p){assert(!disk.empty()&&stored().outcome==Outcome::Unknown&&owner==id&&p==packet);if(!accept_queue)return false;queued.emplace_back(id,n);return true;};
  h.send=[&](const std::string& s){assert(stored().outcome!=Outcome::Unknown);sent.push_back(s);return true;};
  h.wake=[]{};h.began=[&]{++began;};h.ended=[&]{++ended;};player.Initialize(h);
 }
 bool frame(const Json& j){return player.OnJson(j.get(),session,negotiated,press);}
 void service(){player.Service(session,negotiated,ready,press,time);time+=1000;}
 void start(const Alarm& a=alarm()){assert(frame(envelope(a)));service();assert(frame(tts(a,"start")));assert(frame(tts(a,"sentence_start")));}
 void stop(const Alarm& a=alarm()){assert(frame(tts(a,"stop")));}
 void audio(){assert(player.OnAudio(packet,uuid(1)));service();}
 void play(bool decoded=true){assert(!queued.empty());auto p=queued.front();queued.pop_front();if(!decoded)return;std::vector<int16_t> pcm(1440);if(codec.OutputData(pcm))player.OnProgress(p.first,p.second);else player.OnError(p.first);}
 void dma(unsigned n=6){for(unsigned i=0;i<n;++i)codec.OnOutputSent(nullptr,nullptr,&codec);}
 void completed(){start();audio();stop();play();service();assert(sent.empty()&&stored().outcome==Outcome::Unknown);dma();service();assert(stored().outcome==Outcome::Completed&&sent.size()==1&&owner!=0);}
};
void check_receipt(const std::string& s,const char* outcome,const char* action="drain") {auto j=json(s);assert(cJSON_GetArraySize(j.get())==10);assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j.get(),"output_drained")));assert(std::string(cJSON_GetObjectItemCaseSensitive(j.get(),"outcome")->valuestring)==outcome);assert(std::string(cJSON_GetObjectItemCaseSensitive(j.get(),"action")->valuestring)==action);}
void completed_dma(){Harness h;h.start();h.audio();h.stop();h.play();for(unsigned i=0;i<6;++i){h.service();assert(h.sent.empty());h.dma(1);}h.service();assert(h.sent.size()==1);check_receipt(h.sent.back(),"completed");assert(h.owner&&h.player.Fenced());}
void no_provider_stop(){Harness h;h.start();h.audio();h.play();h.dma();h.service();assert(h.sent.empty());h.stop();h.service();check_receipt(h.sent.back(),"completed");}
void interrupted_dma(){Harness h;h.start();h.audio();h.play();++h.press;h.service();assert(h.sent.empty());h.dma(5);h.service();assert(h.sent.empty());h.dma(1);h.service();check_receipt(h.sent.back(),"interrupted");assert(stored().outcome==Outcome::Interrupted);}
void decoder_failure(){Harness h;h.start();h.audio();h.stop();h.play(false);h.service();check_receipt(h.sent.back(),"failed");}
void write_failure(){Harness h;h.start();h.audio();h.stop();write_error=-1;h.play();h.service();assert(h.sent.empty());h.dma();h.service();check_receipt(h.sent.back(),"failed");}
void digest_failure(){Harness h;auto a=alarm();a.audio_sha256=std::string(64,'0');h.start(a);h.audio();h.play();h.stop(a);h.service();assert(h.sent.empty());h.dma();h.service();check_receipt(h.sent.back(),"failed");}
void packet_order(){for(int mode=0;mode<4;++mode){reset();Harness h;auto a=alarm();assert(h.frame(envelope(a)));h.service();if(mode!=0)assert(h.frame(tts(a,"start")));if(mode>1)assert(h.frame(tts(a,"sentence_start")));if(mode==3){h.audio();h.stop();}h.player.OnAudio(packet,uuid(1));if(mode==2)h.player.OnAudio(packet,uuid(1));h.service();assert(stored().outcome==Outcome::Failed&&h.began==1);}}
void packet_bounds(){for(auto p:std::vector<std::vector<uint8_t>>{{},{0x18},{0xf8,0},{0xfb},{0xfb,0},std::vector<uint8_t>(2049,0)}){if(p.size()==1&&p[0]==0x18)continue;assert(!IsSixtyMsOpus(p));}assert(IsSixtyMsOpus(packet));assert(IsSixtyMsOpus({0xfb,3,0}));Harness h;auto a=alarm(22);h.start(a);h.accept_queue=false;for(int i=0;i<21;++i)h.player.OnAudio(packet,uuid(1));h.service();check_receipt(h.sent.back(),"failed");}
void reconnect_terminal(){Harness h;h.completed();const auto record=stored();h.player.OnDisconnected();h.session=uuid(10);h.negotiated=false;h.service();assert(h.sent.size()==1);h.negotiated=true;h.service();check_receipt(h.sent.back(),"completed","reconcile_drain");assert(RecordJson(record)==disk);assert(h.frame(ack(record,h.session)));h.service();assert(disk.empty()&&!h.player.Fenced()&&h.owner==0);}
void reboot_unknown(){NvsStore store;assert(store.Save({alarm(),Outcome::Unknown}));Harness h;h.session=uuid(10);h.codec.output_dma_remaining_=6;h.service();assert(h.sent.empty()&&h.began==0);h.dma();h.service();check_receipt(h.sent.back(),"interrupted","reconcile_drain");assert(h.player.Fenced());}
void reboot_terminal(){NvsStore store;assert(store.Save({alarm(),Outcome::Completed}));Harness h;h.session=uuid(10);h.service();check_receipt(h.sent.back(),"completed","reconcile_drain");assert(h.began==0&&h.queued.empty());}
void stale_ack(){Harness h;h.completed();const auto original=stored();for(auto key:{"session_id","lease_id","playback_id","timer_id","timer_revision","attempt"}){auto a=ack(original,h.session);replace(a.get(),key,std::string(key)=="timer_revision"||std::string(key)=="attempt"?"2":"\""+uuid(88)+"\"");assert(!h.frame(a));h.service();assert(h.owner&&h.player.Fenced()&&!disk.empty());}assert(h.frame(ack(original,h.session)));h.service();assert(!h.owner);auto a=alarm();a.lease_id=uuid(22);a.attempt=2;h.start(a);assert(!h.frame(ack(original,h.session)));h.service();assert(h.owner&&stored().alarm.attempt==2);}
void receipt_retry(){Harness h;h.completed();auto first=h.sent.back();h.time+=1000001;h.service();assert(h.sent.size()==2&&h.sent.back()==first);h.session=uuid(10);h.service();auto second=h.sent.back();h.time+=1000001;h.service();assert(second==h.sent.back()&&disk==RecordJson(stored()));}
void disk_faults(){for(int mode=0;mode<5;++mode){reset();Harness h;if(mode==0)fail_open=true;if(mode==1)fail_set=true;if(mode==2)fail_commit=true;if(mode==3){namespace_present=true;disk="broken";}if(mode==4)corrupt_readback=true;assert(h.frame(envelope(alarm())));h.service();assert(h.player.Fenced()&&h.owner==0&&h.began==0&&h.sent.empty());}reset();disk="broken";namespace_present=true;Harness corrupt;corrupt.service();assert(corrupt.player.Fenced()&&corrupt.owner&&corrupt.sent.empty());}
void terminal_write_fault(){Harness h;h.start();h.audio();h.stop();h.play();h.dma();fail_commit=true;h.service();assert(h.sent.empty()&&h.owner&&h.ended==0&&stored().outcome==Outcome::Unknown);}
void erase_fault(){Harness h;h.completed();assert(h.frame(ack(stored(),h.session)));fail_erase=true;h.service();assert(!disk.empty()&&h.owner&&h.player.Fenced());}
void store_immutable(){NvsStore s;auto a=alarm();assert(s.Save({a,Outcome::Unknown}));auto other=a;other.lease_id=uuid(100);assert(!s.Save({other,Outcome::Unknown}));assert(s.Save({a,Outcome::Failed}));assert(!s.Save({a,Outcome::Completed}));assert(!s.Save({a,Outcome::Unknown}));assert(!s.Erase({a,Outcome::Completed}));assert(s.Erase({a,Outcome::Failed}));Record r;assert(s.Load(r)==Store::LoadResult::Empty);}
void busy_or_press(){for(int mode=0;mode<3;++mode){reset();Harness h;assert(h.frame(envelope(alarm())));if(mode==0)h.ready=false;if(mode==1)h.press+=1;if(mode==2)h.press+=2;h.service();assert(h.began==0&&h.queued.empty()&&h.sent.size()==1);check_receipt(h.sent.back(),mode==0?"failed":"interrupted");}}
void identity_schema(){auto a=alarm();Alarm result;assert(ParseAlarm(envelope(a).get(),result));for(auto key:{"session_id","lease_id","playback_id","timer_id"}){for(auto invalid:{"\"00000000-0000-0000-0000-000000000000\"","\"ABCDEF00-0000-0000-0000-000000000001\"","\"garbage\"","null","4"}){auto j=envelope(a);replace(j.get(),key,invalid);assert(!ParseAlarm(j.get(),result));}}for(auto key:{"packet_count","timer_revision","attempt","spoken_number"}){for(auto invalid:{"0","-1","1.5","2147483648","null","\"1\""}){auto j=envelope(a);replace(j.get(),key,invalid);assert(!ParseAlarm(j.get(),result));}}for(auto invalid:{"501","999"}){auto j=envelope(a);replace(j.get(),"packet_count",invalid);assert(!ParseAlarm(j.get(),result));}for(auto key:{"lease_id","timer_revision","label","packet_count"}){auto j=envelope(a);cJSON_AddNullToObject(j.get(),key);assert(!ParseAlarm(j.get(),result));}auto j=envelope(a);cJSON_AddNullToObject(j.get(),"turn_id");assert(!ParseAlarm(j.get(),result));}
Json snapshot(const std::string& deadline="2026-09-06T12:00:00Z",unsigned count=1){std::string s="{\"type\":\"timer\",\"action\":\"snapshot\",\"session_id\":\""+uuid(1)+"\",\"request_id\":\""+uuid(10)+"\",\"timers\":[";for(unsigned i=0;i<count;++i){if(i)s+=",";s+="{\"id\":\""+uuid(i+20)+"\",\"spoken_number\":"+std::to_string(i+1)+",\"label\":\"Pasta\",\"deadline_at\":\""+deadline+"\",\"revision\":1,\"state\":\"active\"}";}return json(s+"]}");}
void snapshot_bounds(){Snapshot out;assert(ParseSnapshot(snapshot("2026-09-06T12:00:00Z",32).get(),out)&&out.timers.size()==32);assert(!ParseSnapshot(snapshot("2026-09-06T12:00:00Z",33).get(),out)&&out.timers.size()==32);for(auto key:{"id","spoken_number"}){auto j=snapshot("2026-09-06T12:00:00Z",2);auto list=cJSON_GetObjectItemCaseSensitive(j.get(),"timers");auto first=cJSON_GetArrayItem(list,0);auto second=cJSON_GetArrayItem(list,1);auto copy=cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(first,key),true);cJSON_ReplaceItemInObjectCaseSensitive(second,key,copy);assert(!ParseSnapshot(j.get(),out));}for(auto state:{"cancelled","acknowledged","paused"}){auto j=snapshot();replace(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j.get(),"timers"),0),"state","\""+std::string(state)+"\"");assert(!ParseSnapshot(j.get(),out));}}
void snapshot_authority(){Harness h;auto j=snapshot();assert(h.frame(j));h.service();auto state=h.player.GetSnapshot();assert(DisplayText(state,state.timers[0].deadline_ms+100000).find("0:00")!=std::string::npos);assert(!h.owner&&h.sent.empty()&&h.began==0);h.player.OnDisconnected();assert(h.player.GetSnapshot().timers.empty());h.negotiated=false;assert(!h.frame(j));}
void deadline_validation(){Snapshot a,b;assert(ParseSnapshot(snapshot().get(),a));for(auto value:{"2026-09-06T14:00:00+02:00","2026-09-06T07:00:00-05:00","2026-09-06t12:00:00z"}){assert(ParseSnapshot(snapshot(value).get(),b));assert(a.timers[0].deadline_ms==b.timers[0].deadline_ms);}for(auto value:{"2026-02-29T12:00:00Z","2026-09-31T12:00:00Z","2026-09-06T24:00:00Z","2026-09-06T12:00:00+24:00","2026-09-06T12:00:00+01:60","2026-09-06T12:00:00.Z","2026-09-06T12:00:00Zjunk"})assert(!ParseSnapshot(snapshot(value).get(),b));assert(ParseSnapshot(snapshot("2024-02-29T12:00:00.123456Z").get(),b));}
void late_audio_and_json_budget(){
 Harness h;h.start();assert(h.player.OnAudio(packet,uuid(99)));h.service();assert(h.queued.empty());
 h.audio();h.stop();h.play();h.dma();h.service();check_receipt(h.sent.back(),"completed");
 assert(WithinJsonBudget("{\"text\":\"[[[[\"}"));
 assert(WithinJsonBudget(std::string(16,'[')+std::string(16,']')));
 assert(!WithinJsonBudget(std::string(17,'[')+std::string(17,']')));
 assert(!WithinJsonBudget(std::string(32769,' ')));
 assert(!WithinJsonBudget("{\"broken\""));
 Record r;assert(!ParseRecord(std::string(1000,'[')+std::string(1000,']'),r));
}
void tts_schema(){auto a=alarm();std::string state;for(auto phase:{"start","sentence_start","stop"}){auto j=tts(a,phase);assert(MatchesTts(j.get(),a,state));cJSON_AddNumberToObject(j.get(),"turn_id",1);assert(!MatchesTts(j.get(),a,state));}auto j=tts(a,"stop");replace(j.get(),"attempt","2");assert(!MatchesTts(j.get(),a,state));assert(ReceiptJson({a,Outcome::Unknown},a.session_id).empty());Record r;assert(!ParseRecord(RecordJson({a,Outcome::Completed})+"{}",r));}

void actual_output_bridge(){
 for(int scenario=0;scenario<4;++scenario){reset();
 AudioService audio;NvsStore store;Player player(store);std::vector<std::string> sent;
 std::atomic<unsigned> progress{0},errors{0};uint32_t press=0;int64_t time=1000000;
 Player::Hooks hooks;
 hooks.claim=[&](uint32_t id){return audio.ClaimTimerOutput(id);};
 hooks.release=[&](uint32_t id){return audio.ReleaseTimerOutput(id);};
 hooks.cancel=[&]{audio.ResetDecoder();};hooks.drained=[&]{return audio.IsPlaybackIdle();};
 hooks.queue=[&](uint32_t id,uint32_t ordinal,const std::vector<uint8_t>& payload){
  auto p=std::make_unique<AudioStreamPacket>();p->payload=payload;p->playback_id=id;p->media_position_ms=ordinal;
  return audio.PushPacketToDecodeQueue(std::move(p),false);
 };
 hooks.send=[&](const std::string& text){sent.push_back(text);return true;};hooks.wake=[]{};hooks.began=[]{};hooks.ended=[]{};
 player.Initialize(hooks);
 audio.callbacks_.on_playback_progress=[&](uint32_t id,uint32_t ordinal){player.OnProgress(id,ordinal);++progress;};
 audio.callbacks_.on_playback_error=[&](uint32_t id){player.OnError(id);++errors;};
 auto service=[&]{player.Service(uuid(1),true,true,press,time);time+=1000;};
 auto frame=[&](const Json& j){assert(player.OnJson(j.get(),uuid(1),true,press));};
 frame(envelope(alarm()));service();frame(tts(alarm(),"start"));frame(tts(alarm(),"sentence_start"));
 // Real enqueue rejects ordinary/old ownership, and a stale release cannot open it.
 assert(!audio.PushPacketToDecodeQueue(std::make_unique<AudioStreamPacket>(),false));
 assert(!audio.ReleaseTimerOutput(7)&&audio.timer_output_owner_!=0);
 player.OnAudio(packet,uuid(1));service();frame(tts(alarm(),"stop"));
 if(scenario==1)write_error=-1;
 audio.DecodeOne(scenario==2);
 std::thread output([&]{audio.AudioOutputTask();});
 const auto until=std::chrono::steady_clock::now()+2s;
 if(scenario!=2){while(progress+errors==0&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(100us);assert(progress+errors==1);}
 if(scenario==3){++press;audio.local_physical_boundary_=press;}
 service();
 if(scenario!=2){
  assert(sent.empty()&&!audio.IsPlaybackIdle());
  for(int i=0;i<5;++i)audio.codec.OnOutputSent(nullptr,nullptr,&audio.codec);
  service();assert(sent.empty());audio.codec.OnOutputSent(nullptr,nullptr,&audio.codec);
 }
 while(!audio.IsPlaybackIdle()&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(100us);
 assert(audio.IsPlaybackIdle());service();assert(sent.size()==1);
 check_receipt(sent.back(),scenario==0?"completed":scenario==3?"interrupted":"failed");
 frame(ack(stored(),uuid(1)));service();assert(!player.Fenced()&&audio.timer_output_owner_==0&&disk.empty());
 audio.stop();output.join();
 }
}
void actual_busy_ownership(){
 for(int mode=0;mode<4;++mode){reset();AudioService audio;NvsStore store;Player player(store);
 std::vector<std::string> sent;int claims=0,releases=0,cancels=0,began=0,ended=0;std::atomic<int> progress{0};
 Player::Hooks h;
 h.claim=[&](uint32_t id){++claims;
  // Readiness was observed before this physical press acquired input.
  if(mode==2){audio.local_physical_boundary_=9;audio.local_recording_press_=9;audio.local_input_press_=9;}
  return audio.ClaimTimerOutput(id);
 };
 h.release=[&](uint32_t id){++releases;return audio.ReleaseTimerOutput(id);};
 h.cancel=[&]{++cancels;audio.ResetDecoder();};h.drained=[&]{return audio.IsPlaybackIdle();};
 h.queue=[](uint32_t,uint32_t,const std::vector<uint8_t>&){assert(false);return false;};
 h.send=[&](const std::string& text){sent.push_back(text);return true;};h.wake=[]{};
 h.began=[&]{++began;};h.ended=[&]{++ended;};player.Initialize(h);
 if(mode<2){auto packet=std::make_unique<AudioStreamPacket>();packet->playback_id=7;packet->payload={0x18,0,0x55};assert(audio.PushPacketToDecodeQueue(std::move(packet),false));}
 if(mode==3){std::vector<int16_t> pcm(1440);assert(audio.codec.OutputData(pcm));}
 const auto generation=audio.playback_generation_;
 assert(player.OnJson(envelope(alarm()).get(),uuid(1),true,0));
 player.Service(uuid(1),true,mode!=0,0,1000000);
 assert(claims==(mode==0?0:1)&&cancels==0&&began==0&&ended==0&&releases==0);
 assert(audio.timer_output_owner_==0&&audio.playback_generation_==generation);
 if(mode==2){assert(audio.local_recording_press_==9&&audio.local_input_press_==9&&audio.local_physical_boundary_==9);assert(sent.size()==1);}
 else assert(sent.empty());
 std::thread output;
 if(mode<2){
  assert(audio.audio_decode_queue_.size()==1&&audio.audio_decode_queue_.front()->playback_id==7);
  // Disconnecting the declined timer also cannot reset the earlier reply.
  player.OnDisconnected();player.Service(uuid(1),true,false,0,1001000);
  assert(cancels==0&&audio.audio_decode_queue_.size()==1&&audio.playback_generation_==generation);
  audio.callbacks_.on_playback_progress=[&](uint32_t id,uint32_t){assert(id==7);++progress;};
  audio.DecodeOne();output=std::thread([&]{audio.AudioOutputTask();});
  auto deadline=std::chrono::steady_clock::now()+2s;
  while(progress==0&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(100us);
  assert(progress==1);
 }
 if(mode!=2){
  for(int i=0;i<5;++i)audio.codec.OnOutputSent(nullptr,nullptr,&audio.codec);
  player.Service(uuid(1),true,false,0,1002000);assert(sent.empty());
  audio.codec.OnOutputSent(nullptr,nullptr,&audio.codec);
  auto deadline=std::chrono::steady_clock::now()+2s;
  while(!audio.IsPlaybackIdle()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(100us);
  assert(audio.IsPlaybackIdle());player.Service(uuid(1),true,false,0,1003000);assert(sent.size()==1);
 }
 check_receipt(sent.back(),"failed");assert(player.Fenced()&&!player.OwnsOutput());
 assert(player.OnJson(ack(stored(),uuid(1)).get(),uuid(1),true,0));
 player.Service(uuid(1),true,false,0,1004000);
 assert(!player.Fenced()&&disk.empty()&&releases==0&&cancels==0&&began==0&&ended==0&&audio.playback_generation_==generation);
 if(mode==2)assert(audio.local_recording_press_==9&&audio.local_input_press_==9);
 audio.stop();if(output.joinable())output.join();
 }
}
void enqueue_claim_interleavings(){
 constexpr uint32_t timer_id=0x80000001u;
 {
  AudioService audio;const auto generation=audio.playback_generation_;
  std::mutex gate;std::condition_variable cv;bool at_lock=false,resume=false,pushed=false;
  before_decode_queue_lock=[&]{std::unique_lock<std::mutex> lock(gate);at_lock=true;cv.notify_all();cv.wait(lock,[&]{return resume;});};
  std::thread producer([&]{auto p=std::make_unique<AudioStreamPacket>();p->playback_id=7;p->payload=packet;pushed=audio.PushPacketToDecodeQueue(std::move(p),false);});
  {std::unique_lock<std::mutex> lock(gate);assert(cv.wait_for(lock,2s,[&]{return at_lock;}));}
  assert(audio.ClaimTimerOutput(timer_id));
  {std::lock_guard<std::mutex> lock(gate);resume=true;}cv.notify_all();producer.join();
  before_decode_queue_lock={};
  assert(!pushed&&audio.timer_output_owner_==timer_id&&audio.audio_decode_queue_.empty());
  assert(audio.playback_generation_==generation);
  auto matching=std::make_unique<AudioStreamPacket>();matching->playback_id=timer_id;
  assert(audio.PushPacketToDecodeQueue(std::move(matching),false));
  assert(audio.audio_decode_queue_.size()==1&&audio.audio_decode_queue_.front()->playback_id==timer_id);
 }
 {
  AudioService audio;const auto generation=audio.playback_generation_;
  auto ordinary=std::make_unique<AudioStreamPacket>();ordinary->playback_id=7;
  assert(audio.PushPacketToDecodeQueue(std::move(ordinary),false));
  assert(!audio.ClaimTimerOutput(timer_id));
  assert(audio.timer_output_owner_==0&&audio.audio_decode_queue_.size()==1);
  assert(audio.audio_decode_queue_.front()->playback_id==7&&audio.playback_generation_==generation);
 }
}
void enqueue_waiting_owner_change(){
 constexpr uint32_t timer_id=0x80000001u;
 AudioService audio;const auto generation=audio.playback_generation_;
 for(int i=0;i<MAX_DECODE_PACKETS_IN_QUEUE;++i){auto p=std::make_unique<AudioStreamPacket>();p->playback_id=7;assert(audio.PushPacketToDecodeQueue(std::move(p),false));}
 std::mutex gate;std::condition_variable cv;bool waiting=false,pushed=false;
 before_decode_queue_wait=[&]{{std::lock_guard<std::mutex> lock(gate);waiting=true;}cv.notify_all();};
 std::thread producer([&]{auto p=std::make_unique<AudioStreamPacket>();p->playback_id=8;pushed=audio.PushPacketToDecodeQueue(std::move(p),true);});
 {std::unique_lock<std::mutex> lock(gate);assert(cv.wait_for(lock,2s,[&]{return waiting;}));}
 {
  std::lock_guard<std::mutex> lock(audio.audio_queue_mutex_);
  audio.audio_decode_queue_.clear();
  assert(audio.IsPlaybackDrainedLocked());
  uint32_t empty=0;assert(audio.timer_output_owner_.compare_exchange_strong(empty,timer_id));
  audio.audio_queue_cv_.notify_all();
 }
 producer.join();before_decode_queue_wait={};
 assert(!pushed&&audio.timer_output_owner_==timer_id&&audio.audio_decode_queue_.empty());
 assert(audio.playback_generation_==generation);
}
void terminal_hook_paths(){
 {Harness h;h.completed();assert(h.began==1&&h.ended==1);}
 reset();
 {Harness h;h.start();h.audio();h.stop();h.play(false);h.service();assert(h.began==1&&h.ended==1);}
 reset();
 {Harness h;h.start();h.audio();h.stop();h.play();++h.press;h.service();h.dma();h.service();assert(h.began==1&&h.ended==1);}
 reset();
 {Harness h;assert(h.frame(envelope(alarm())));h.ready=false;h.service();assert(h.began==0&&h.ended==0);}
}

int main(int argc,char** argv){assert(argc==2);std::map<std::string,std::function<void()>> tests={
#define CASE(name) {#name,name}
 CASE(enqueue_claim_interleavings),CASE(enqueue_waiting_owner_change),CASE(terminal_hook_paths),CASE(actual_busy_ownership),CASE(late_audio_and_json_budget),CASE(actual_output_bridge),CASE(completed_dma),CASE(no_provider_stop),CASE(interrupted_dma),CASE(decoder_failure),CASE(write_failure),CASE(digest_failure),CASE(packet_order),CASE(packet_bounds),CASE(reconnect_terminal),CASE(reboot_unknown),CASE(reboot_terminal),CASE(stale_ack),CASE(receipt_retry),CASE(disk_faults),CASE(terminal_write_fault),CASE(erase_fault),CASE(store_immutable),CASE(busy_or_press),CASE(identity_schema),CASE(snapshot_bounds),CASE(snapshot_authority),CASE(deadline_validation),CASE(tts_schema)};
 reset();tests.at(argv[1])();std::cout<<argv[1]<<" passed\n";
}
'''
CASES = ('enqueue_claim_interleavings enqueue_waiting_owner_change terminal_hook_paths '
         'actual_busy_ownership late_audio_and_json_budget actual_output_bridge completed_dma no_provider_stop interrupted_dma decoder_failure write_failure digest_failure '
         'packet_order packet_bounds reconnect_terminal reboot_unknown reboot_terminal stale_ack receipt_retry '
         'disk_faults terminal_write_fault erase_fault store_immutable busy_or_press identity_schema '
         'snapshot_bounds snapshot_authority deadline_validation tts_schema').split()


class TimerDisplayIntegrationTests(unittest.TestCase):
    def test_actual_timer_label_updates_and_clears_on_main_surface(self):
        setter = method("main/boards/m5stack/stopwatch/crest_display.h", "void SetTimerText(")
        program = r"""
#include <cassert>
#include <string>
struct Label {std::string text;};int writes=0,locks=0;
void lv_label_set_text(Label* label,const char* text){assert(locks==1);++writes;label->text=text;}
struct Base {virtual void SetTimerText(const std::string&) {}};
struct DisplayLockGuard {DisplayLockGuard(void*){++locks;}~DisplayLockGuard(){--locks;}};
struct OrbitCrestDisplay:Base {Label label;Label* timers_=&label;std::string timer_text_;
__SETTER__
};
int main(){OrbitCrestDisplay display;display.SetTimerText("7 • 0:10 Pasta");assert(writes==1&&locks==0);display.SetTimerText("7 • 0:10 Pasta");assert(writes==1);display.SetTimerText("");assert(writes==2&&display.label.text.empty());display.timers_=nullptr;display.SetTimerText("ignored");assert(writes==2);}
"""
        run_cpp(program.replace("__SETTER__", setter))

    def test_actual_application_routes_timer_before_voice_turn_and_preserves_physical_edges(self):
        source = (ROOT / "main/application.cc").read_text()
        incoming = source[source.index("protocol->OnIncomingJson("):]
        self.assertLess(incoming.index("timer_player_.OnJson("), incoming.index("uint32_t gateway_turn"))
        init = method("main/application.cc", "void Application::InitializeTimers()")
        self.assertIn("audio_service_.IsPlaybackIdle()", init)
        self.assertIn("audio_service_.ClaimTimerOutput(id)", init)
        self.assertIn("audio_service_.ReleaseTimerOutput(id)", init)
        self.assertIn("audio_service_.PushPacketToDecodeQueue(std::move(packet), false)", init)
        self.assertIn("hooks.ended = [this]() { HandleTimerOutputEnded(); };", init)
        for edge in ("void Application::StartListening()", "void Application::StopListening()"):
            callback = method("main/application.cc", edge)
            self.assertNotIn("timer_player_", callback)
            self.assertNotIn("Nvs", callback)
        self.assertIn("packet->source_session_id", source)
        self.assertIn("source_session_id = packet_session", (ROOT / "main/protocols/websocket_protocol.cc").read_text())

    def test_timer_terminal_power_hook_preserves_foreground_owners(self):
        handler = method("main/application.cc", "void Application::HandleTimerOutputEnded()")
        program = r"""
#include <atomic>
#include <cassert>
#include <functional>
enum DeviceState {kDeviceStateIdle,kDeviceStateListening,kDeviceStateSpeaking,kDeviceStateNotifying};
enum class PowerSaveLevel {LOW_POWER,PERFORMANCE};
struct Board {int low_power=0,performance=0;PowerSaveLevel level=PowerSaveLevel::PERFORMANCE;std::function<void()> before_low;static Board& GetInstance(){static Board b;return b;}void SetPowerSaveLevel(PowerSaveLevel next){if(next==PowerSaveLevel::LOW_POWER){if(before_low)before_low();++low_power;}else{++performance;}level=next;}};
struct AudioService {bool playback_idle=true,input_idle=true;bool IsPlaybackIdle(){return playback_idle;}bool IsLocalInputIdle(){return input_idle;}};
struct Application {DeviceState state=kDeviceStateNotifying;bool transition=true,talk_on_transition=false,output_on_transition=false,reply_on_transition=false;int transitions=0;std::atomic<bool> manual_listening_requested_{false};AudioService audio_service_;
 DeviceState GetDeviceState()const{return state;}bool SetDeviceState(DeviceState next){++transitions;if(!transition)return false;state=next;if(talk_on_transition)manual_listening_requested_=true;if(output_on_transition)audio_service_.playback_idle=false;if(reply_on_transition)state=kDeviceStateSpeaking;return true;}
 void HandleTimerOutputEnded();
};
__HANDLER__
void reset_board(){auto& board=Board::GetInstance();board.low_power=board.performance=0;board.level=PowerSaveLevel::PERFORMANCE;board.before_low={};}
int main(){
 {reset_board();Application app;app.HandleTimerOutputEnded();assert(app.state==kDeviceStateIdle&&app.transitions==1&&Board::GetInstance().low_power==1&&Board::GetInstance().performance==0&&Board::GetInstance().level==PowerSaveLevel::LOW_POWER);}
 {reset_board();Application app;app.manual_listening_requested_=true;app.HandleTimerOutputEnded();assert(app.state==kDeviceStateNotifying&&app.transitions==0&&Board::GetInstance().low_power==0);}
 {reset_board();Application app;app.audio_service_.input_idle=false;app.HandleTimerOutputEnded();assert(app.state==kDeviceStateNotifying&&app.transitions==0&&Board::GetInstance().low_power==0);}
 {reset_board();Application app;app.audio_service_.playback_idle=false;app.HandleTimerOutputEnded();assert(app.state==kDeviceStateNotifying&&app.transitions==0&&Board::GetInstance().low_power==0);}
 {reset_board();Application app;app.state=kDeviceStateSpeaking;app.HandleTimerOutputEnded();assert(app.state==kDeviceStateSpeaking&&app.transitions==0&&Board::GetInstance().low_power==0);}
 {reset_board();Application app;app.transition=false;app.HandleTimerOutputEnded();assert(app.state==kDeviceStateNotifying&&app.transitions==1&&Board::GetInstance().low_power==0);}
 {reset_board();Application app;app.talk_on_transition=true;app.HandleTimerOutputEnded();assert(app.state==kDeviceStateIdle&&app.transitions==1&&Board::GetInstance().low_power==0);}
 {reset_board();Application app;app.output_on_transition=true;app.HandleTimerOutputEnded();assert(app.state==kDeviceStateIdle&&app.transitions==1&&Board::GetInstance().low_power==0);}
 {reset_board();Application app;app.reply_on_transition=true;app.HandleTimerOutputEnded();assert(app.state==kDeviceStateSpeaking&&app.transitions==1&&Board::GetInstance().low_power==0);}
 {reset_board();Application app;Board::GetInstance().before_low=[&]{app.manual_listening_requested_=true;};app.HandleTimerOutputEnded();assert(app.state==kDeviceStateIdle&&app.transitions==1&&Board::GetInstance().low_power==1&&Board::GetInstance().performance==1&&Board::GetInstance().level==PowerSaveLevel::PERFORMANCE);}
 {reset_board();Application app;Board::GetInstance().before_low=[&]{app.audio_service_.playback_idle=false;};app.HandleTimerOutputEnded();assert(app.state==kDeviceStateIdle&&app.transitions==1&&Board::GetInstance().low_power==1&&Board::GetInstance().performance==1&&Board::GetInstance().level==PowerSaveLevel::PERFORMANCE);}
}
"""
        run_cpp(program.replace("__HANDLER__", handler))


class TimerFirmwareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory(prefix="orbit-timers-review-")
        cls.addClassCleanup(cls.folder.cleanup)
        path = Path(cls.folder.name)
        cjson = ROOT / "managed_components/espressif__cjson/cJSON"
        if not (cjson / "cJSON.c").exists():
            raise unittest.SkipTest("Prepare the canonical firmware dependencies first")
        openssl = Path("/opt/homebrew/opt/openssl@3")
        flags = ["-I", str(openssl / "include"), "-L", str(openssl / "lib")] if openssl.exists() else []
        for name, source in HEADERS.items():
            header = path / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text(source)
        codec = CODEC[:CODEC.index("int main(){")].replace("constexpr int ESP_OK=0,", "constexpr int ")
        specs = [("main/audio/audio_codec.cc", "bool AudioCodec::InputData("),
                 ("main/audio/audio_codec.cc", "bool AudioCodec::OutputData(")]
        specs += [("main/audio/codecs/es8311_audio_codec.cc", sig) for sig in (
            "int Es8311AudioCodec::Read(", "int Es8311AudioCodec::Write(",
            "bool Es8311AudioCodec::PrepareInputCapture(", "bool Es8311AudioCodec::IsOutputDrained(",
            "bool Es8311AudioCodec::OnOutputSent(")]
        codec = codec.replace(" bool input_enabled_=true", " bool output_enabled() const{return output_enabled_;} void EnableOutput(bool on){output_enabled_=on;}\n bool input_enabled_=true")
        codec = codec.replace("__METHODS__", "\n".join(method(*spec) for spec in specs))
        audio = AUDIO.replace("__OWNER_METHODS__", "\n".join(method("main/audio/audio_service.h", sig) for sig in (
            "bool ClaimTimerOutput(", "bool ReleaseTimerOutput(")))
        push = method("main/audio/audio_service.cc", "bool AudioService::PushPacketToDecodeQueue(")
        push = push.replace(
            "std::unique_lock<std::mutex> lock(audio_queue_mutex_);",
            "if (before_decode_queue_lock) before_decode_queue_lock();\n    "
            "std::unique_lock<std::mutex> lock(audio_queue_mutex_);",
        )
        push = push.replace(
            "audio_queue_cv_.wait(lock, [this, generation]() {",
            "if (before_decode_queue_wait) before_decode_queue_wait();\n            "
            "audio_queue_cv_.wait(lock, [this, generation]() {",
        )
        audio_methods = [method("main/audio/audio_service.cc", sig) for sig in (
            "void AudioService::AudioOutputTask()", "bool AudioService::IsPlaybackIdle()",
            "bool AudioService::IsPlaybackDrainedLocked() const", "bool AudioService::MarkPlaybackDrainedLocked()",
            "void AudioService::ResetDecoder()")]
        audio_methods.append(push)
        audio = audio.replace("__AUDIO_METHODS__", "\n".join(audio_methods))
        (path / "review.cc").write_text(PROGRAM.replace("__CODEC__", codec).replace("__AUDIO__", audio))
        sanitize = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(["cc", *sanitize, "-I", str(cjson), "-c", str(cjson / "cJSON.c"),
                        "-o", str(path / "cjson.o")], check=True, capture_output=True, text=True)
        cls.binary = path / "review"
        result = subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-Wno-deprecated-declarations", *sanitize, *flags,
                        "-I", str(path), "-I", str(ROOT / "main"), "-I", str(cjson),
                        str(path / "review.cc"), str(ROOT / "main/provisions_timers.cc"),
                        str(ROOT / "main/provisions_timer_player.cc"), str(ROOT / "main/provisions_timer_store.cc"),
                        str(path / "cjson.o"), "-lcrypto", "-o", str(cls.binary)],
                        capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr)


def make_test(case):
    def test(self):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True,
                                timeout=15, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0" if sys.platform == "darwin" else "detect_leaks=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
    return test


for case in CASES:
    setattr(TimerFirmwareTests, "test_" + case, make_test(case))

if __name__ == "__main__":
    unittest.main()
