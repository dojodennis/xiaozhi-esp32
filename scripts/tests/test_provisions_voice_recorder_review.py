"""Run the actual recorder worker with deterministic task/codec shims and real GCM.

The scheduler parks only at FreeRTOS notification waits; tests never call private
worker methods. Flash/NVS/crypto are the existing ESP adapter fault harness.
"""
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from test_provisions_audio_boundaries import method

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "orbit_outbox_worker_fixtures", Path(__file__).with_name("test_provisions_voice_outbox_esp_review.py"))
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)

HEADERS = {
    **fixture.HEADERS,
    "freertos/FreeRTOS.h": r'''
#pragma once
#include <cstdint>
constexpr int pdTRUE=1,pdPASS=1;
#define pdMS_TO_TICKS(x) (x)
''',
    "freertos/task.h": r'''
#pragma once
#include <cstdint>
struct TestTask;
using TaskHandle_t=TestTask*;
int xTaskCreate(void(*)(void*),const char*,unsigned,void*,unsigned,TaskHandle_t*);
void xTaskNotifyGive(TaskHandle_t);
unsigned ulTaskNotifyTake(int,unsigned);
void vTaskDelay(unsigned);
void vTaskDelete(void*);
''',
    "esp_timer.h": "#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n",
    "esp_audio_enc.h": r'''
#pragma once
#include <cstdint>
constexpr int ESP_AUDIO_ERR_OK=0,ESP_AUDIO_SAMPLE_RATE_16K=16000,ESP_AUDIO_MONO=1,ESP_AUDIO_BIT16=16;
struct esp_audio_enc_in_frame_t {uint8_t* buffer;uint32_t len;};
struct esp_audio_enc_out_frame_t {uint8_t* buffer;uint32_t len;uint32_t encoded_bytes;uint64_t pts;};
''',
    "esp_opus_enc.h": r'''
#pragma once
#include "esp_audio_enc.h"
#include <cstddef>
constexpr int ESP_OPUS_BITRATE_AUTO=-1,ESP_OPUS_ENC_FRAME_DURATION_60_MS=60,ESP_OPUS_ENC_APPLICATION_VOIP=1;
struct esp_opus_enc_config_t {
    int sample_rate,channel,bits_per_sample,bitrate,frame_duration,application_mode,complexity;
    bool enable_fec,enable_dtx,enable_vbr;
};
int esp_opus_enc_open(const esp_opus_enc_config_t*,size_t,void**);
int esp_opus_enc_get_frame_size(void*,int*,int*);
int esp_opus_enc_process(void*,esp_audio_enc_in_frame_t*,esp_audio_enc_out_frame_t*);
void esp_opus_enc_close(void*);
''',
}
HEADERS["esp_heap_caps.h"] += "\nvoid* heap_caps_calloc(size_t,size_t,unsigned);\n"
HEADERS["psa/crypto.h"] += """
constexpr unsigned PSA_ALG_SHA_256=30;
int psa_hash_compute(unsigned,const uint8_t*,size_t,uint8_t*,size_t,size_t*);
"""

PRELUDE = r'''
#include "provisions_voice_recorder.h"
#include "esp_opus_enc.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>
std::function<void()> read_hook;
'''
SUPPORT = fixture.PROGRAM.split("int main() {")[0].replace(
    "bytes==VoiceOutbox::kMaxFrameBytes && caps==",
    "(bytes==VoiceOutbox::kMaxFrameBytes || bytes==VoiceRecording::kMaxSamples*sizeof(int16_t)) && caps==").replace(
        "n==fail_n", "(fail_n==0 || n==fail_n)")

# Keep dictation's versioned NVS namespace distinct from the existing audio keys.
SUPPORT = SUPPORT.replace('std::string(space)=="orbit_audio" &&', '(std::string(space)=="orbit_audio" || std::string(space)=="orbit_dct_v1") &&')
SUPPORT = SUPPORT.replace('*handle=++state.handles;', '*handle=++state.handles;if(std::string(space)=="orbit_dct_v1")*handle|=0x80000000u;')
SUPPORT = SUPPORT.replace('nvs_get_blob(nvs_handle_t,const char* key,', 'nvs_get_blob(nvs_handle_t handle,const char* key,')
SUPPORT = SUPPORT.replace('nvs_set_blob(nvs_handle_t,const char* key,', 'nvs_set_blob(nvs_handle_t handle,const char* key,')
SUPPORT = SUPPORT.replace('auto requested=*bytes;*bytes=found->second.size();', 'auto requested=*bytes;*bytes=found->second.size();if(!out)return ESP_OK;')
SUPPORT = SUPPORT.replace('if(state.bad("get_blob"))return ESP_FAIL;', 'std::string scoped=(handle&0x80000000u)?std::string("dictation/")+key:key;key=scoped.c_str();if(state.bad("get_blob"))return ESP_FAIL;')
SUPPORT = SUPPORT.replace('if(state.bad("set_blob"))return ESP_FAIL;', 'std::string scoped=(handle&0x80000000u)?std::string("dictation/")+key:key;key=scoped.c_str();if(state.bad("set_blob"))return ESP_FAIL;')
SUPPORT = SUPPORT.replace('aad==108 || aad==16', 'aad==108 || aad==136 || aad==16')
SUPPORT = SUPPORT.replace('if(state.bad("read")) return ESP_FAIL;', 'if(read_hook){auto hook=std::move(read_hook);hook();}if(state.bad("read")) return ESP_FAIL;')
SUPPORT = SUPPORT.replace('struct State {', 'struct AdapterState {').replace('State state;', 'AdapterState state;').replace('state=State{};', 'state=AdapterState{};')

WORKER = r'''
struct TestTask {
    std::mutex mutex;std::condition_variable changed;unsigned notifications=0;bool waiting=false;
    std::thread thread;
};
TestTask* task=nullptr;
thread_local TestTask* current_task=nullptr;
std::atomic<int64_t> clock_us{0};
std::vector<std::pair<VoiceRecorder::Result,uint32_t>> notices;
struct Offered {VoiceCaptureReceipt receipt;uint32_t press;size_t slot;VoiceId retry_token;};
std::vector<Offered> offered;
std::shared_ptr<const VoiceReplay> held;
bool hold_replay=false;
bool fail_encoder=false;
bool pause_worker=false;
std::function<void(VoiceRecorder::Result)> notify_hook;
int xTaskCreate(void(*function)(void*),const char*,unsigned,void* argument,unsigned,TaskHandle_t* handle) {
    assert(!task);task=new TestTask;*handle=task;
    task->thread=std::thread([=]{current_task=task;function(argument);});return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t value) {
    std::lock_guard<std::mutex> lock(value->mutex);++value->notifications;value->changed.notify_all();
}
unsigned ulTaskNotifyTake(int clear,unsigned) {
    auto* value=current_task;assert(value);
    std::unique_lock<std::mutex> lock(value->mutex);value->waiting=true;value->changed.notify_all();
    value->changed.wait(lock,[&]{return value->notifications>0&&!pause_worker;});value->waiting=false;
    const auto count=value->notifications;if(clear)value->notifications=0;else --value->notifications;
    return count;
}
void vTaskDelay(unsigned) {std::this_thread::sleep_for(std::chrono::milliseconds(1));}
void vTaskDelete(void*) {}
void drain() {
    assert(task);std::unique_lock<std::mutex> lock(task->mutex);
    assert(task->changed.wait_for(lock,std::chrono::seconds(5),[]{return task->waiting&&task->notifications==0;}));
}
void join() {
    assert(task);task->thread.join();delete task;task=nullptr;
    assert(state.allocations.empty()&&state.keys.empty()&&state.active_ops==0);
}
int64_t esp_timer_get_time(){return clock_us.load();}
void* heap_caps_calloc(size_t n,size_t size,unsigned caps) {
    auto* value=heap_caps_malloc(n*size,caps);if(value)memset(value,0,n*size);return value;
}
int psa_hash_compute(unsigned alg,const uint8_t* in,size_t bytes,uint8_t* out,size_t capacity,size_t* written) {
    assert(alg==PSA_ALG_SHA_256&&capacity>=32);unsigned count=0;
    const bool ok=EVP_Digest(in,bytes,out,&count,EVP_sha256(),nullptr)==1;*written=count;return ok?0:-1;
}
int esp_opus_enc_open(const esp_opus_enc_config_t* config,size_t size,void** encoder) {
    assert(size==sizeof(*config)&&config->sample_rate==16000&&config->channel==1&&config->bits_per_sample==16);
    assert(config->frame_duration==60);if(fail_encoder)return -1;*encoder=reinterpret_cast<void*>(1);return 0;
}
int esp_opus_enc_get_frame_size(void* encoder,int* in,int* out) {assert(encoder);*in=1920;*out=2048;return 0;}
int esp_opus_enc_process(void* encoder,esp_audio_enc_in_frame_t* in,esp_audio_enc_out_frame_t* out) {
    assert(encoder&&in->len==1920&&out->len==2048);
    out->buffer[0]=0xab;out->buffer[1]=in->buffer[0];out->buffer[2]=0xcd;out->encoded_bytes=3;return 0;
}
void esp_opus_enc_close(void* encoder){assert(encoder);}
void initialize(VoiceRecorder& recorder) {
    assert(recorder.Start([](auto result,uint32_t press){notices.emplace_back(result,press);if(notify_hook)notify_hook(result);},
        [](auto replay){
            VoiceCaptureReceipt receipt;receipt.capture=replay->capture;receipt.bytes=replay->bytes;
            receipt.digest=replay->digest;offered.push_back({receipt,replay->press,replay->slot,replay->retry_token});
            if(hold_replay)held=replay;
        }));
    drain();
}
VoiceContext context(int conversation=42,int source=8,unsigned revision=2) {
    VoiceContext result;result.conversation_id[0]=conversation;
    result.source_request_id[0]=source;result.source_revision=revision;return result;
}
void authorize(VoiceRecorder& recorder,const VoiceContext& value=context()) {
    assert(recorder.UpdateContext(value));drain();assert(recorder.IsReady()&&recorder.HasContext());
}
void record(VoiceRecorder& recorder,uint32_t press,uint64_t captured=1788712345678) {
    assert(recorder.Begin(press,captured));int16_t samples[160];std::fill_n(samples,160,123);
    assert(recorder.Append(press,samples,160,1));recorder.Release(press);drain();
}
void replay(VoiceRecorder& recorder){recorder.RequestReplay();drain();}
void fresh() {
    assert(!held&&!task);assert(!read_hook);reset();clock_us=0;notices.clear();offered.clear();hold_replay=false;fail_encoder=false;
}
void acknowledge(VoiceRecorder& recorder,const VoiceCaptureReceipt& receipt) {
    assert(recorder.Acknowledge(receipt));drain();
}
void normal_cases() {
    fresh();
    VoiceCaptureReceipt original;
    {
        VoiceRecorder recorder;initialize(recorder);assert(!recorder.IsReady()&&!recorder.HasContext());
        assert(!recorder.Begin(1,0)&&state.nvs.empty());authorize(recorder);
        record(recorder,1);assert(recorder.PendingCount()==1);
        assert(notices.back()==std::make_pair(VoiceRecorder::Result::Saved,uint32_t(1)));
        replay(recorder);assert(offered.size()==1&&offered[0].press==1);original=offered[0].receipt;
        assert(original.capture.source_request_id==context().source_request_id&&original.capture.source_revision==2);
        assert(original.capture.packet_count==1&&original.bytes==5&&original.capture.request_id!=VoiceId{});
        replay(recorder);assert(offered.size()==1);clock_us=30000000;replay(recorder);
        assert(offered.size()==2&&offered.back().press==0); // Retry never narrates the foreground turn twice.
        assert(offered.back().receipt.capture.request_id==original.capture.request_id);
        assert(offered.back().receipt.digest==original.digest);
        auto receipt=original;acknowledge(recorder,receipt);assert(recorder.PendingCount()==1);
        // Every immutable metadata field and digest must match before deletion.
        for(int field=0;field<9;field++) {
            receipt=original;receipt.durable=true;
            if(field==0)receipt.capture.request_id[1]^=1;
            if(field==1)receipt.capture.conversation_id[1]^=1;
            if(field==2)receipt.capture.source_request_id[1]^=1;
            if(field==3)++receipt.capture.source_revision;
            if(field==4)++receipt.capture.captured_unix_ms;
            if(field==5)++receipt.capture.packet_count;
            if(field==6)++receipt.bytes;
            if(field==7)receipt.digest[0]^=1;
            if(field==8){receipt.capture.source_request_id={};receipt.capture.source_revision=0;}
            acknowledge(recorder,receipt);assert(recorder.PendingCount()==1);
        }
        receipt=original;receipt.needs_attention=true;acknowledge(recorder,receipt);
        assert(recorder.NeedsAttention()&&recorder.PendingCount()==1);
        clock_us=60000000;replay(recorder);assert(offered.size()==2);
    }
    join();
    {
        VoiceRecorder recorder;initialize(recorder);assert(recorder.IsReady()&&recorder.HasContext());
        replay(recorder);assert(offered.size()==3&&offered.back().press==0);
        assert(offered.back().receipt.capture.request_id==original.capture.request_id);
        auto receipt=original;receipt.durable=true;acknowledge(recorder,receipt);
        assert(recorder.PendingCount()==0&&!recorder.NeedsAttention());
        acknowledge(recorder,receipt);assert(recorder.PendingCount()==0); // Receipt replay is harmless.
    }
    join();
    fresh();
    {
        VoiceRecorder recorder;initialize(recorder);authorize(recorder);hold_replay=true;
        record(recorder,1);replay(recorder);assert(held&&offered.size()==1);
        const auto first=held->capture.request_id;const auto digest=held->digest;
        record(recorder,2);assert(recorder.PendingCount()==2);clock_us=30000000;replay(recorder);
        assert(offered.size()==1&&held->capture.request_id==first&&held->digest==digest);
        held.reset();hold_replay=false;replay(recorder);assert(offered.size()==2);
        // Reassignment must neither replay another conversation nor evict its audio.
        authorize(recorder,context(43));clock_us=60000000;replay(recorder);
        assert(offered.size()==2&&recorder.PendingCount()==2);
        authorize(recorder,context());
        record(recorder,3);record(recorder,4);assert(recorder.PendingCount()==4);
        record(recorder,5);assert(recorder.PendingCount()==4);
        assert(notices.back()==std::make_pair(VoiceRecorder::Result::Failed,uint32_t(5)));
        fail_encoder=true;record(recorder,6);assert(recorder.PendingCount()==4);
        assert(notices.back()==std::make_pair(VoiceRecorder::Result::Failed,uint32_t(6)));
    }
    join();
    std::cout<<"Recorder worker exact receipts, retry silence, restart, scope and full-store cases passed\n";
}
void context_cases() {
    // A failed preparation has not displayed the new answer or changed RAM.
    fresh();
    const auto newer=context(42,9,3);
    std::vector<uint8_t> committed;
    {
        VoiceRecorder recorder;initialize(recorder);authorize(recorder);committed=state.nvs.at("context_v1");
        state.fail="set_blob";state.fail_n=0;
        assert(!recorder.PrepareContext(newer));drain();assert(!recorder.ActivateContext(newer));
        assert(state.nvs.at("context_v1")==committed&&recorder.NeedsAttention());
        record(recorder,1);replay(recorder);assert(offered.back().receipt.capture.source_request_id==context().source_request_id);
        assert(offered.back().receipt.capture.source_revision==context().source_revision);
    }
    join();state.fail.clear();
    {
        VoiceRecorder recorder;initialize(recorder);assert(recorder.HasContext());
        assert(state.nvs.at("context_v1")==committed); // Last actually shown context remains eligible.
    }
    join();
    // Prepared but not yet displayed: old RAM remains current; reboot cannot
    // misinterpret either the earlier cache or the unpresented answer as current.
    fresh();
    {
        VoiceRecorder recorder;initialize(recorder);authorize(recorder);
        assert(recorder.PrepareContext(newer));drain();
        assert(memcmp(state.nvs.at("context_v1").data(),"ORP1",4)==0);
        assert(!recorder.ActivateContext(context(42,10,4)));
        record(recorder,1);replay(recorder);
        assert(offered.back().receipt.capture.source_request_id==context().source_request_id);
    }
    join();
    {
        VoiceRecorder recorder;initialize(recorder);assert(recorder.IsReady()&&!recorder.HasContext());
        assert(recorder.PendingCount()==1&&!recorder.Begin(1,0));
        const auto offered_before=offered.size();replay(recorder);assert(offered.size()==offered_before);
        authorize(recorder,newer);replay(recorder);assert(offered.size()==offered_before+1);
        assert(offered.back().press==0&&offered.back().receipt.capture.source_request_id==context().source_request_id);
    }
    join();
    // Display activation can precede the commit write. A failed commit keeps
    // its durable invalid marker, while the new capture preserves the new source.
    fresh();
    {
        VoiceRecorder recorder;initialize(recorder);authorize(recorder);
        assert(recorder.PrepareContext(newer));drain();
        state.fail="set_blob";state.fail_n=0;
        assert(recorder.ActivateContext(newer));drain();
        assert(recorder.NeedsAttention()&&memcmp(state.nvs.at("context_v1").data(),"ORP1",4)==0);
        record(recorder,1);replay(recorder);
        assert(offered.back().receipt.capture.source_request_id==newer.source_request_id);
        assert(offered.back().receipt.capture.source_revision==newer.source_revision);
        // Background cache repair is bounded to three attempts with 30-second spacing.
        const auto attempts=state.calls["set_blob"];
        for(int tick=1;tick<=5;tick++){clock_us=tick*30000000LL;replay(recorder);}
        assert(state.calls["set_blob"]==attempts+2);
        assert(memcmp(state.nvs.at("context_v1").data(),"ORP1",4)==0);
    }
    join();state.fail.clear();
    {
        VoiceRecorder recorder;initialize(recorder);assert(recorder.IsReady()&&!recorder.HasContext());
        assert(!recorder.Begin(1,0)&&recorder.PendingCount()==1);
        authorize(recorder,newer);assert(!recorder.NeedsAttention());
        assert(memcmp(state.nvs.at("context_v1").data(),"ORC1",4)==0);
        replay(recorder);assert(offered.back().press==0);
        assert(offered.back().receipt.capture.source_request_id==newer.source_request_id);
    }
    join();
    {
        VoiceRecorder recorder;initialize(recorder);assert(recorder.HasContext());
        record(recorder,1);clock_us+=30000000;replay(recorder);
        assert(offered.back().receipt.capture.source_request_id==newer.source_request_id);
    }
    join();
    std::cout<<"Two-phase context prepare/activate, failed writes, bounded repair and reboot cases passed\n";
}

VoiceCaptureReceipt challenge(VoiceCaptureReceipt receipt,uint8_t token=91,bool used=false,bool attention=true) {
    receipt.durable=false;receipt.needs_attention=attention;receipt.retry_token={};receipt.retry_token[0]=token;receipt.retry_used=used;return receipt;
}
void explicit_retry_cases() {
    fresh();VoiceCaptureReceipt original;
    {
        VoiceRecorder recorder;initialize(recorder);authorize(recorder);record(recorder,1);replay(recorder);
        original=offered.back().receipt;assert(offered.back().retry_token==VoiceId{});
        assert(!recorder.RequestRetry()&&!recorder.CanRetry()&&!recorder.RetryPending());
        auto forged=challenge(original);forged.digest[0]^=1;acknowledge(recorder,forged);
        assert(!recorder.CanRetry()&&!recorder.NeedsAttention());
        auto no_challenge=original;no_challenge.needs_attention=true;acknowledge(recorder,no_challenge);
        assert(recorder.NeedsAttention()&&!recorder.CanRetry()&&!recorder.RequestRetry());
        acknowledge(recorder,challenge(original));assert(recorder.CanRetry()&&!recorder.RetryPending());
        clock_us=30000000;replay(recorder);assert(offered.size()==1); // A received challenge is never authorization.
        assert(recorder.RequestRetry());drain();assert(recorder.RetryPending()&&!recorder.CanRetry());
        assert(offered.size()==2&&offered.back().press==0&&offered.back().retry_token[0]==91);
        assert(offered.back().receipt.capture.request_id==original.capture.request_id);
        replay(recorder);assert(offered.size()==2);clock_us=60000000;replay(recorder);
        assert(offered.size()==3&&offered.back().press==0&&offered.back().retry_token[0]==91); // Lost acknowledgement retries same nonce.
        acknowledge(recorder,challenge(original,92,true,false));assert(recorder.RetryPending());
        acknowledge(recorder,challenge(original,91,true,false));assert(!recorder.RetryPending()&&!recorder.CanRetry()&&!recorder.NeedsAttention());
        acknowledge(recorder,challenge(original));assert(!recorder.RetryPending()&&!recorder.NeedsAttention()); // Delayed unused challenge is fenced.
        clock_us=90000000;replay(recorder);assert(offered.size()==4&&offered.back().retry_token==VoiceId{}&&offered.back().press==0);
        acknowledge(recorder,challenge(original,91,true,true));assert(recorder.NeedsAttention()&&!recorder.CanRetry()&&!recorder.RequestRetry());
        clock_us=120000000;replay(recorder);assert(offered.size()==4); // No endless exhausted retry.
        auto durable=original;durable.durable=true;acknowledge(recorder,durable);assert(recorder.PendingCount()==0&&!recorder.RetryPending());
        record(recorder,2);replay(recorder);auto second=offered.back().receipt;assert(second.capture.request_id!=original.capture.request_id);
        acknowledge(recorder,challenge(original));assert(!recorder.CanRetry()); // Reused slot cannot inherit an older capture's challenge.
        acknowledge(recorder,challenge(second,92));assert(recorder.CanRetry());assert(recorder.RequestRetry());drain();
        assert(offered.back().retry_token[0]==92&&offered.back().press==0);
    }
    join();
    // A queued local gesture is not reconstructed on reboot. The first server
    // receipt either reports its spent nonce or offers a fresh explicit gesture.
    {
        VoiceRecorder recorder;initialize(recorder);assert(!recorder.CanRetry()&&!recorder.RetryPending());
        replay(recorder);assert(offered.back().retry_token==VoiceId{}&&offered.back().press==0);
        auto receipt=offered.back().receipt;acknowledge(recorder,challenge(receipt,92,true,true));
        assert(recorder.NeedsAttention()&&!recorder.CanRetry()&&!recorder.RequestRetry());
    }
    join();
    fresh();
    {
        VoiceRecorder recorder;initialize(recorder);authorize(recorder);
        record(recorder,1);replay(recorder);auto first=offered.back().receipt;
        acknowledge(recorder,challenge(first));
        record(recorder,2);replay(recorder);auto second=offered.back().receipt;
        assert(second.capture.request_id!=first.capture.request_id);acknowledge(recorder,challenge(second,92));
        assert(recorder.RequestRetry());drain();assert(offered.back().receipt.capture.request_id==first.capture.request_id&&offered.back().retry_token[0]==91);
        const auto count=offered.size();authorize(recorder,context(43));
        assert(!recorder.CanRetry()&&!recorder.RetryPending()&&!recorder.RequestRetry());
        clock_us=60000000;replay(recorder);assert(offered.size()==count&&recorder.PendingCount()==2);
    }
    join();
    std::cout<<"Explicit retry challenge, exact capture, same-nonce retransmission, stale receipt, slot reuse, restart and scope cases passed\n";
}


void queued_retry_scope_case() {
    fresh();
    {
        VoiceRecorder recorder;initialize(recorder);authorize(recorder);record(recorder,1);replay(recorder);
        auto first=offered.back().receipt;acknowledge(recorder,challenge(first));
        authorize(recorder,context(43));record(recorder,2);replay(recorder);
        auto second=offered.back().receipt;acknowledge(recorder,challenge(second,92));
        authorize(recorder,context());assert(recorder.CanRetry());
        assert(recorder.PrepareContext(context(43)));drain();
        const auto before=offered.size();
        {std::lock_guard<std::mutex> lock(task->mutex);pause_worker=true;}
        assert(recorder.RequestRetry()); // Captures assignment42 on the caller.
        assert(recorder.ActivateContext(context(43)));
        {std::lock_guard<std::mutex> lock(task->mutex);pause_worker=false;task->changed.notify_all();}
        drain();
        assert(offered.size()==before&&!recorder.RetryPending()&&recorder.CanRetry());
        assert(notices.back()==std::make_pair(VoiceRecorder::Result::RetryUnavailable,uint32_t(0)));
        assert(recorder.RequestRetry());drain();assert(offered.back().receipt.capture.request_id==second.capture.request_id&&offered.back().retry_token[0]==92);
    }
    join();
}


void dictation_ack(VoiceRecorder& recorder, dictation::State next) {
    const auto r=recorder.DictationRecord();dictation::Reply ack;
    ack.id=r.id;ack.action=r.pending;ack.acknowledged=true;ack.payload_revision=r.pending_revision;
    ack.payload_count=r.frozen_count;ack.state=next;ack.revision=r.revision+1;
    ack.control_revision=r.pending==dictation::Action::Start?1:r.control_revision+1;
    ack.expected_count=r.count;ack.expires_ms=1790812800000LL;
    assert(recorder.AcknowledgeDictation(ack));drain();
}
void begin_dictation(VoiceRecorder& recorder,uint32_t press) {
    assert(recorder.CanDictate(press,context().conversation_id,1788712345678LL));
    assert(!recorder.BeginDictation(press,1788712345678LL));drain();
    assert(recorder.BeginDictation(press,1788712345678LL));
}
void dictation_ack_stop_race() {
    fresh();
    {
        VoiceRecorder recorder;initialize(recorder);authorize(recorder);
        assert(recorder.RequestDictationControl(dictation::Action::Start));drain();
        // The production worker has committed its ACK but has not published
        // capture authority when the application asks to stop.
        bool requested=false;
        notify_hook=[&](auto result){
            if(result==VoiceRecorder::Result::DictationChanged&&!requested){
                requested=true;assert(recorder.RequestDictationControl(dictation::Action::Stop));
                assert(!recorder.CanDictate(1,context().conversation_id,1788712345678LL));
            }
        };
        dictation_ack(recorder,dictation::State::Open);notify_hook={};
        assert(requested&&recorder.DictationAuthorization()==0);
        assert(recorder.DictationRecord().pending==dictation::Action::Stop);
        assert(!recorder.CanDictate(1,context().conversation_id,1788712345678LL));
    }
    join();
}
void dictation_cases() {
    fresh();VoiceId session,request;
    {
        VoiceRecorder recorder;initialize(recorder);authorize(recorder);
        assert(recorder.RequestDictationControl(dictation::Action::Start));drain();
        session=recorder.DictationRecord().id;assert(recorder.DictationRecord().pending==dictation::Action::Start);
        assert(!recorder.CanDictate(1,context().conversation_id,1788712345678LL));
        dictation_ack(recorder,dictation::State::Open);
        assert(recorder.DictationAuthorization()==1);
        begin_dictation(recorder,1);assert(recorder.DictationRecord().count==1);
        request=recorder.DictationRecord().segments[0].request_id;
        int16_t pcm[160]{};assert(recorder.Append(1,pcm,160,1));
        {std::lock_guard<std::mutex> lock(task->mutex);pause_worker=true;}
        recorder.Release(1);assert(recorder.RequestDictationControl(dictation::Action::Stop));
        assert(!recorder.CanDictate(2,context().conversation_id,1788712345678LL));
        {std::lock_guard<std::mutex> lock(task->mutex);pause_worker=false;task->changed.notify_all();}
        drain();
        const auto stopped=recorder.DictationRecord();
        assert(stopped.count==1&&stopped.frozen_count==1&&stopped.pending_revision==1&&stopped.pending==dictation::Action::Stop);
        assert(stopped.segments[0].samples==160&&stopped.segments[0].request_id==request);
        assert(offered.back().press==0&&offered.back().receipt.capture.IsDictation());
        assert(offered.back().receipt.capture.sample_count==160&&offered.back().receipt.capture.packet_count==1);
        assert(recorder.PendingCount()==1&&!recorder.DictationBusy());
    }
    join();
    // The pending Stop and exact raw part survive restart before the ACK.
    {
        VoiceRecorder recorder;initialize(recorder);assert(recorder.DictationRecord().id==session);
        assert(recorder.DictationRecord().pending==dictation::Action::Stop&&recorder.DictationRecord().count==1);
        dictation_ack(recorder,dictation::State::Stopped);assert(recorder.DictationRecord().control_revision==2);
        replay(recorder);auto receipt=offered.back().receipt;receipt.durable=true;
        state.fail="set_blob";state.fail_n=0;acknowledge(recorder,receipt);
        assert(recorder.PendingCount()==1&&recorder.DictationFaulted()&&!recorder.DictationRecord().segments[0].terminal);
        state.fail.clear();acknowledge(recorder,receipt);assert(!recorder.DictationFaulted());
        assert(recorder.PendingCount()==0&&recorder.DictationRecord().count==1&&recorder.DictationRecord().segments[0].terminal);
        assert(recorder.RequestDictationControl(dictation::Action::Resume));drain();
        assert(!recorder.CanDictate(2,context().conversation_id,1788712345678LL));
        dictation_ack(recorder,dictation::State::Open);assert(recorder.DictationRecord().control_revision==3);
        begin_dictation(recorder,2);int16_t pcm[160]{};
        for(int i=0;i<1000;++i)assert(recorder.Append(2,pcm,160,1));
        assert(recorder.DictationCapped(2));drain();
        assert(!recorder.CanDictate(2,context().conversation_id,1788712345678LL));
        assert(offered.back().receipt.capture.sample_count==160000&&offered.back().receipt.capture.packet_count==167);
        assert(recorder.DictationRecord().count==2&&recorder.DictationRecord().segments[1].samples==160000);
        recorder.Release(2);drain();
    }
    join();
    // A failed flash write keeps Processing PCM and the same reserved request.
    fresh();
    {
        VoiceRecorder recorder;initialize(recorder);authorize(recorder);
        assert(recorder.RequestDictationControl(dictation::Action::Start));drain();dictation_ack(recorder,dictation::State::Open);
        begin_dictation(recorder,1);request=recorder.DictationRecord().segments[0].request_id;
        int16_t pcm[160]{};assert(recorder.Append(1,pcm,160,1));state.fail="write";state.fail_n=0;
        recorder.Release(1);assert(recorder.RequestDictationControl(dictation::Action::Stop));drain();
        assert(recorder.DictationFaulted()&&recorder.DictationBusy()&&recorder.DictationRecord().frozen_count==1);
        assert(recorder.DictationRecord().segments[0].request_id==request);
        state.fail.clear();clock_us+=31000000;replay(recorder);
        assert(!recorder.DictationFaulted()&&!recorder.DictationBusy()&&recorder.PendingCount()==1);
        assert(offered.back().receipt.capture.request_id==request&&offered.back().receipt.capture.sample_count==160);
    }
    join();
    // No microphone starts for a release while the reservation is still queued.
    fresh();
    {
        VoiceRecorder recorder;initialize(recorder);authorize(recorder);
        assert(recorder.RequestDictationControl(dictation::Action::Start));drain();dictation_ack(recorder,dictation::State::Open);
        {std::lock_guard<std::mutex> lock(task->mutex);pause_worker=true;}
        assert(!recorder.BeginDictation(1,1788712345678LL));recorder.Release(1);
        assert(recorder.RequestDictationControl(dictation::Action::Stop));
        {std::lock_guard<std::mutex> lock(task->mutex);pause_worker=false;task->changed.notify_all();}
        drain();assert(recorder.DictationRecord().count==0&&recorder.DictationRecord().frozen_count==0&&!recorder.DictationBusy());
    }
    join();
    std::cout<<"Dictation worker reservation, cap, Stop sealing, exact retry and restart cases passed\n";
}
int main(){normal_cases();context_cases();explicit_retry_cases();queued_retry_scope_case();dictation_ack_stop_race();dictation_cases();}

'''


PHYSICAL = r"""
#include "provisions_reply_turn.h"
#include "provisions_voice_wire.h"
#include <sys/time.h>
#include <string_view>
#define CONFIG_PROVISIONS_LOCAL_CAPTURE 1
#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1
constexpr int MAIN_EVENT_START_LISTENING=1,MAIN_EVENT_STOP_LISTENING=2,MAIN_EVENT_DICTATION_MODE=4,MAIN_EVENT_DICTATION_CONTROL=8,MAIN_EVENT_DICTATION_CAP=16,AS_EVENT_LOCAL_RECORDING_RUNNING=32;
constexpr int kDeviceStateIdle=0,kDeviceStateListening=1;
std::atomic<unsigned> app_events{0};
void xEventGroupSetBits(int,unsigned bits){app_events.fetch_or(bits);}void xEventGroupClearBits(int,unsigned bits){app_events.fetch_and(~bits);}
struct WebsocketProtocol {
 bool opened=true,negotiated=true;VoiceContext capture=context();std::vector<std::string> controls;
 void InterruptStoredRecording(){}bool IsAudioChannelOpened(){return opened;}bool DictationNegotiated(){return opened&&negotiated;}
 bool GetCaptureContext(VoiceContext& out){out=capture;return opened&&negotiated;}
 std::string session_id(){return "00000000-0000-0000-0000-000000000001";}
 bool SendDictationControl(const std::string& text){controls.push_back(text);return true;}
};
struct Display {bool visible=false;std::string status,action;void SetDictationScreen(bool v,const std::string& s,const std::string& a){visible=v;status=s;action=a;}};
struct Board {Display display;static Board& GetInstance(){static Board board;return board;}Display* GetDisplay(){return &display;}};
int64_t DictationNow(bool trusted){return trusted?1788712345678LL:0;}

struct AudioService {
 std::atomic<uint32_t> local_physical_boundary_{0},local_recording_press_{0},local_output_boundary_{0},local_prepared_press_{0},local_input_press_{0};
 std::atomic<bool> service_stopped_{false};std::mutex local_recording_mutex_,audio_queue_mutex_;std::condition_variable audio_queue_cv_;
 uint32_t playback_generation_=0;int event_group_=0;std::vector<int> audio_decode_queue_,audio_playback_queue_;std::string_view local_feedback_;bool local_feedback_active_=false;std::atomic<uint32_t> local_feedback_errors_{0};
 void FenceLocalRecording(uint32_t);void ReleaseLocalRecordingFence(uint32_t);void ReconcileLocalRecording(uint32_t);
 void StartLocalRecording(uint32_t);void StopLocalRecording(uint32_t expected=0);bool IsLocalInputIdle()const;
 void CancelLocalFeedback(){}void CloseVoiceUploadGate(){}void ResetDecoder(){}bool IsLocalRecordingReady(uint32_t press){return local_recording_press_==press;}
};
struct Application {
 std::mutex provisions_recording_control_mutex_;ProvisionsReplyTurn provisions_physical_press_;
 std::shared_ptr<VoiceRecorder> provisions_recorder_=std::make_shared<VoiceRecorder>();AudioService audio_service_;
 std::shared_ptr<WebsocketProtocol> protocol=std::make_shared<WebsocketProtocol>();int event_group_=0,state=kDeviceStateIdle;
 std::atomic<bool> manual_listening_requested_{false},has_server_time_{true},provisions_recording_failed_{false},provisions_recording_saving_{false},provisions_recording_local_{false},dictation_screen_{false};
 std::atomic<uint32_t> dictation_closed_press_{0};uint32_t provisions_recording_started_press_=0,dictation_authorization_seen_=0;
 bool provisions_recording_was_dictation_=false,dictation_has_assignment_proof_=true;VoiceId dictation_assignment_proof_=context().conversation_id;
 std::atomic<bool> provisions_network_busy_{false},provisions_response_pending_{false};
 int64_t dictation_next_receipt_us_=0,dictation_last_send_us_=0;std::string dictation_sent_control_;
 struct TimerPlayer{bool fenced=false;bool Fenced(){return fenced;}}timer_player_;
 std::shared_ptr<WebsocketProtocol> GetProtocol(){return protocol;}int GetDeviceState(){return state;}void SetDeviceState(int value){state=value;}
 void Schedule(std::function<void()> fn){fn();}void HandleVoiceRecordingResult(VoiceRecorder::Result,uint32_t){assert(false);}
 void StartListening();void StopListening();bool BeginLocalRecordingOnMain();void EndLocalRecordingOnMain();void ToggleDictationScreen();void DictationButton();void CloseDictationInputOnMain();void ServiceDictation();void HandleDictationControlOnMain();
 __FENCE__
 void Samples(uint32_t press,const int16_t* pcm,size_t frames,size_t channels) __SAMPLES__
};
__AUDIO_METHODS__
__APP_METHODS__
void dictation_main_consumer_cases(){
 fresh();
 {
  Application app;auto& recorder=*app.provisions_recorder_;initialize(recorder);authorize(recorder);
  app.dictation_has_assignment_proof_=false;app.ToggleDictationScreen();app.CloseDictationInputOnMain();
  app.protocol->negotiated=false;app.HandleDictationControlOnMain();drain();assert(recorder.DictationRecord().pending==dictation::Action::None);
  app.protocol->negotiated=true;app.ServiceDictation();app.HandleDictationControlOnMain();drain();
  assert(recorder.DictationRecord().pending==dictation::Action::Start);app.ServiceDictation();
  assert(app.protocol->controls.size()==1&&Board::GetInstance().display.action=="Stop");
  const auto pending=app.protocol->controls.back();clock_us+=1000001;app.ServiceDictation();assert(app.protocol->controls.size()==2&&app.protocol->controls.back()==pending);
  dictation_ack(recorder,dictation::State::Open);app.ServiceDictation();
  app.dictation_has_assignment_proof_=false;app.protocol->opened=false;
  app.StartListening();assert(!app.BeginLocalRecordingOnMain());
  // A current authenticated assignment cannot revive the hold begun offline
  // before this reboot's assignment proof existed.
  app.protocol->opened=true;app.ServiceDictation();assert(app.dictation_has_assignment_proof_&&!app.BeginLocalRecordingOnMain());
  app.StopListening();app.EndLocalRecordingOnMain();app.StartListening();assert(!app.BeginLocalRecordingOnMain());drain();assert(app.BeginLocalRecordingOnMain());
  int16_t samples[160]{};app.Samples(2,samples,160,1);app.state=kDeviceStateListening;
  app.DictationButton();assert(app.audio_service_.local_recording_press_==0);
  app.CloseDictationInputOnMain();app.HandleDictationControlOnMain();drain();
  assert(recorder.DictationRecord().pending==dictation::Action::Stop&&recorder.DictationRecord().frozen_count==1);
  app.StopListening();app.EndLocalRecordingOnMain();app.ServiceDictation();assert(app.protocol->controls.back().find("expected_segments")!=std::string::npos);
  dictation_ack(recorder,dictation::State::Stopped);app.ServiceDictation();drain();app.ServiceDictation();
  assert(Board::GetInstance().display.status.find("Stopped")!=std::string::npos||Board::GetInstance().display.status.find("Control pending")!=std::string::npos);
 }
 join();
}

void reassignment_start(Application& app) {
 auto& recorder=*app.provisions_recorder_;initialize(recorder);authorize(recorder);
 app.ToggleDictationScreen();app.CloseDictationInputOnMain();app.ServiceDictation();app.HandleDictationControlOnMain();drain();
 dictation_ack(recorder,dictation::State::Open);app.ServiceDictation();
}
void move_assignment(Application& app,int assignment=99){authorize(*app.provisions_recorder_,context(assignment));app.protocol->capture=context(assignment);app.ServiceDictation();}
void fresh_blue(Application& app){app.DictationButton();app.CloseDictationInputOnMain();app.HandleDictationControlOnMain();drain();app.ServiceDictation();}
void dictation_reassignment_cases(){
 fresh();VoiceId old_id,new_id;dictation::Reply old_ack;
 {
  Application app;reassignment_start(app);auto& recorder=*app.provisions_recorder_;const auto original=recorder.DictationRecord();old_id=original.id;
  old_ack.id=old_id;old_ack.action=dictation::Action::Start;old_ack.acknowledged=true;old_ack.state=dictation::State::Open;old_ack.revision=1;old_ack.control_revision=1;old_ack.expires_ms=original.expires_ms;
  move_assignment(app);assert(Board::GetInstance().display.action=="Start");
  const auto sent=app.protocol->controls.size();fresh_blue(app);
  const auto next=recorder.DictationRecord();new_id=next.id;
  assert(new_id!=old_id&&next.conversation_id==context(99).conversation_id&&next.pending==dictation::Action::Start&&next.count==0);
  assert(app.protocol->controls.size()==sent+1&&app.protocol->controls.back().find(VoiceIdText(new_id))!=std::string::npos);
  assert(recorder.AcknowledgeDictation(old_ack));drain();assert(recorder.DictationRecord().id==new_id&&recorder.DictationRecord().pending==dictation::Action::Start);
  assert(!recorder.CanDictate(1,context(99).conversation_id,1788712345678LL));
 }
 join();
 {
  Application app;auto& recorder=*app.provisions_recorder_;initialize(recorder);authorize(recorder,context(99));app.protocol->capture=context(99);
  app.ToggleDictationScreen();app.CloseDictationInputOnMain();app.ServiceDictation();
  assert(recorder.DictationRecord().id==new_id&&recorder.DictationRecord().pending==dictation::Action::Start);
  assert(app.protocol->controls.back().find(VoiceIdText(new_id))!=std::string::npos);
  assert(recorder.AcknowledgeDictation(old_ack));drain();
  app.StartListening();assert(!app.BeginLocalRecordingOnMain());dictation_ack(recorder,dictation::State::Open);app.ServiceDictation();assert(!app.BeginLocalRecordingOnMain());
  app.StopListening();app.EndLocalRecordingOnMain();app.StartListening();assert(!app.BeginLocalRecordingOnMain());drain();assert(app.BeginLocalRecordingOnMain());
  int16_t samples[160]{};app.Samples(2,samples,160,1);app.StopListening();app.EndLocalRecordingOnMain();drain();
  assert(recorder.DictationRecord().count==1&&recorder.DictationRecord().id==new_id);
 }
 join();
 // Every unresolved phase and any actual recording data preserves A.
 for(int mode=0;mode<6;++mode){fresh();VoiceId preserved;
  {
   Application app;reassignment_start(app);auto& recorder=*app.provisions_recorder_;
   if(mode==0){assert(recorder.RequestDictationControl(dictation::Action::Stop));drain();}
   if(mode==1){assert(recorder.RequestDictationControl(dictation::Action::Stop));drain();dictation_ack(recorder,dictation::State::Stopped);assert(recorder.RequestDictationControl(dictation::Action::Resume));drain();}
   if(mode==2){begin_dictation(recorder,1);}
   if(mode==3){record(recorder,1);}
   if(mode==4){assert(recorder.RequestDictationControl(dictation::Action::Receipt));drain();}
   if(mode==5){begin_dictation(recorder,1);int16_t samples[160]{};assert(recorder.Append(1,samples,160,1));state.fail="write";state.fail_n=0;recorder.Release(1);drain();state.fail.clear();}
   const auto before=recorder.DictationRecord();preserved=before.id;const auto manifest=state.nvs.at("dictation/journal");const auto raw=state.flash;
   move_assignment(app);const auto sent=app.protocol->controls.size();fresh_blue(app);
   assert(recorder.DictationRecord().id==before.id&&state.nvs.at("dictation/journal")==manifest&&state.flash==raw);
   assert(app.protocol->controls.size()==sent&&Board::GetInstance().display.action=="Recovery");
  }join();
  {
   Application app;auto& recorder=*app.provisions_recorder_;initialize(recorder);authorize(recorder,context(99));app.protocol->capture=context(99);app.ToggleDictationScreen();app.CloseDictationInputOnMain();app.ServiceDictation();fresh_blue(app);
   assert(recorder.DictationRecord().id==preserved&&app.protocol->controls.empty()&&Board::GetInstance().display.action=="Recovery");
  }join();
 }
 // An uncertain original Start is never an acknowledged empty journal.
 fresh();{
  Application app;auto& recorder=*app.provisions_recorder_;initialize(recorder);authorize(recorder);app.ToggleDictationScreen();app.CloseDictationInputOnMain();app.ServiceDictation();app.HandleDictationControlOnMain();drain();
  old_id=recorder.DictationRecord().id;move_assignment(app);fresh_blue(app);assert(recorder.DictationRecord().id==old_id&&recorder.DictationRecord().pending==dictation::Action::Start);
 }join();
 // The microphone can still own an in-flight read after a requested close.
 fresh();{
  Application app;reassignment_start(app);auto& recorder=*app.provisions_recorder_;old_id=recorder.DictationRecord().id;move_assignment(app);
  app.audio_service_.local_input_press_=44;fresh_blue(app);assert(recorder.DictationRecord().id==old_id);app.audio_service_.local_input_press_=0;
  fresh_blue(app);assert(recorder.DictationRecord().id!=old_id);
 }join();
 // A cached zero PendingCount does not substitute for the worker's slot scan.
 for(int fault=0;fault<3;++fault){fresh();
  {
   Application app;reassignment_start(app);auto& recorder=*app.provisions_recorder_;old_id=recorder.DictationRecord().id;move_assignment(app);const auto manifest=state.nvs.at("dictation/journal");
   if(fault==0)state.flash[tail]=0;
   if(fault==1){state.fail="read";state.fail_n=0;}
   if(fault==2)state.bad_readback=true;
   const auto raw=state.flash;fresh_blue(app);
   assert(recorder.DictationRecord().id==old_id&&recorder.DictationFaulted()&&state.flash==raw&&!recorder.CanDictate(1,context(99).conversation_id,1788712345678LL));
   if(fault!=2)assert(state.nvs.at("dictation/journal")==manifest);
   state.fail.clear();state.bad_readback=false;
  }join();
  if(fault==2){{Application app;auto& recorder=*app.provisions_recorder_;initialize(recorder);assert(recorder.DictationRecord().id!=old_id&&recorder.DictationRecord().pending==dictation::Action::Start&&!recorder.DictationRecord().authorized);}join();}
 }

 // An acknowledged empty A may also be retired by a fresh gesture after reboot.
 fresh();{
  Application app;reassignment_start(app);old_id=app.provisions_recorder_->DictationRecord().id;move_assignment(app);
 }join();{
  Application app;auto& recorder=*app.provisions_recorder_;initialize(recorder);authorize(recorder,context(99));app.protocol->capture=context(99);app.ToggleDictationScreen();app.CloseDictationInputOnMain();app.ServiceDictation();
  assert(Board::GetInstance().display.action=="Start");fresh_blue(app);assert(recorder.DictationRecord().id!=old_id&&recorder.DictationRecord().pending==dictation::Action::Start);
 }join();
 // A B-to-C request during the actual slot inspection is rechecked before the
 // new UUID commit. Activation waits until the short replacement gate ends.
 fresh();{
  Application app;reassignment_start(app);auto& recorder=*app.provisions_recorder_;old_id=recorder.DictationRecord().id;move_assignment(app);std::thread reassignment;
  bool prepared=false,activated=false;const auto sent=app.protocol->controls.size();
  {std::lock_guard<std::mutex> lock(task->mutex);pause_worker=true;}
  read_hook=[&]{
   reassignment=std::thread([&]{prepared=recorder.PrepareContext(context(100));if(prepared)activated=recorder.ActivateContext(context(100));});
   std::unique_lock<std::mutex> lock(task->mutex);assert(task->changed.wait_for(lock,std::chrono::seconds(2),[]{return task->notifications>0;}));
  };
  assert(recorder.RequestEmptyDictationReplacement(context(99).conversation_id));
  {std::lock_guard<std::mutex> lock(task->mutex);pause_worker=false;task->changed.notify_all();}
  drain();assert(reassignment.joinable());reassignment.join();drain();assert(prepared&&activated&&recorder.DictationRecord().id==old_id);
  app.protocol->capture=context(100);app.ServiceDictation();assert(app.protocol->controls.size()==sent);
  fresh_blue(app);assert(recorder.DictationRecord().conversation_id==context(100).conversation_id);
 }join();
 // A changed prepared context invalidates the queued B gesture before commit.
 fresh();{
  Application app;reassignment_start(app);auto& recorder=*app.provisions_recorder_;old_id=recorder.DictationRecord().id;move_assignment(app);
  assert(recorder.PrepareContext(context(100)));drain();
  {std::lock_guard<std::mutex> lock(task->mutex);pause_worker=true;}
  assert(recorder.RequestEmptyDictationReplacement(context(99).conversation_id));assert(!recorder.Begin(1,1788712345678LL));assert(!recorder.ActivateContext(context(100)));
  {std::lock_guard<std::mutex> lock(task->mutex);pause_worker=false;task->changed.notify_all();}
  drain();assert(recorder.DictationRecord().id==old_id);assert(recorder.ActivateContext(context(100)));drain();app.protocol->capture=context(100);app.ServiceDictation();
  fresh_blue(app);assert(recorder.DictationRecord().conversation_id==context(100).conversation_id&&recorder.DictationRecord().pending==dictation::Action::Start);
 }join();
 std::cout<<"Empty-journal reassignment: fresh Start, exact UUID/ACK, all uncertain phases, actual slots/input, readback faults, context changes and restart passed\n";
}

void dictation_physical_cases(){
 fresh();
 {
  Application app;auto& recorder=*app.provisions_recorder_;initialize(recorder);authorize(recorder);
  assert(recorder.RequestDictationControl(dictation::Action::Start));drain();dictation_ack(recorder,dictation::State::Open);
  app.dictation_authorization_seen_=recorder.DictationAuthorization();
  const auto writes=state.writes;
  app.ToggleDictationScreen();assert(app.dictation_screen_&&state.writes==writes);app.CloseDictationInputOnMain();
  app.StartListening();assert(app.audio_service_.local_recording_press_==0);
  assert(!app.BeginLocalRecordingOnMain());drain();assert(app.BeginLocalRecordingOnMain());app.state=kDeviceStateListening;
  assert(app.audio_service_.local_recording_press_==1);int16_t pcm[160]{};app.Samples(1,pcm,160,1);
  app.ToggleDictationScreen();assert(!app.dictation_screen_&&app.manual_listening_requested_&&app.audio_service_.local_recording_press_==0);
  app.CloseDictationInputOnMain();drain();assert(!app.BeginLocalRecordingOnMain()&&app.audio_service_.local_recording_press_==0);
  assert(recorder.DictationRecord().count==1&&offered.back().receipt.capture.IsDictation());
  app.StopListening();app.EndLocalRecordingOnMain();app.StartListening();assert(app.BeginLocalRecordingOnMain());
  assert(app.audio_service_.local_recording_press_==2);app.Samples(2,pcm,160,1);app.StopListening();app.EndLocalRecordingOnMain();drain();
  assert(recorder.RequestDictationControl(dictation::Action::Stop));drain();dictation_ack(recorder,dictation::State::Stopped);
  assert(recorder.RequestDictationControl(dictation::Action::Resume));drain();
  app.ToggleDictationScreen();app.CloseDictationInputOnMain();app.StartListening();assert(!app.BeginLocalRecordingOnMain());
  dictation_ack(recorder,dictation::State::Open);app.ServiceDictation();
  // A delayed ACK cannot activate the already-held third press.
  assert(!app.BeginLocalRecordingOnMain()&&app.dictation_closed_press_==3&&app.audio_service_.local_recording_press_==0);
  app.StopListening();app.EndLocalRecordingOnMain();app.StartListening();assert(!app.BeginLocalRecordingOnMain());drain();assert(app.BeginLocalRecordingOnMain());app.state=kDeviceStateListening;
  for(int i=0;i<1000;++i)app.Samples(4,pcm,160,1);
  assert(app.manual_listening_requested_&&app.audio_service_.local_recording_press_==0&&(app_events&MAIN_EVENT_DICTATION_CAP));
  app.CloseDictationInputOnMain();drain();assert(!app.BeginLocalRecordingOnMain()&&recorder.DictationRecord().segments[1].samples==160000);
  app.StopListening();app.EndLocalRecordingOnMain();
  {std::lock_guard<std::mutex> lock(task->mutex);pause_worker=true;}
  app.StartListening();assert(!app.BeginLocalRecordingOnMain());app.StopListening();app.EndLocalRecordingOnMain();
  {std::lock_guard<std::mutex> lock(task->mutex);pause_worker=false;task->changed.notify_all();}
  drain();assert(recorder.DictationRecord().count==2&&app.audio_service_.local_recording_press_==0);
  app.FenceDictationThrough(7);app.FenceDictationThrough(4);assert(app.dictation_closed_press_==7);
 }
 join();std::cout<<"Physical dictation entry/exit, queued release, ACK edge and cap fences passed\n";
}
"""
app_source = (ROOT / 'main/application.cc').read_text()
start = app_source.index('callbacks.on_recording_audio = ')
opening = app_source.index('{', start)
depth, end = 1, opening + 1
while depth:
    depth += (app_source[end] == '{') - (app_source[end] == '}')
    end += 1
PHYSICAL = PHYSICAL.replace('__SAMPLES__', app_source[opening:end])
PHYSICAL = PHYSICAL.replace('__FENCE__', method('main/application.h', 'void FenceDictationThrough('))
PHYSICAL = PHYSICAL.replace('__AUDIO_METHODS__', '\n'.join(method('main/audio/audio_service.cc', signature) for signature in (
    'void AudioService::FenceLocalRecording(', 'void AudioService::ReleaseLocalRecordingFence(',
    'void AudioService::ReconcileLocalRecording(', 'void AudioService::StartLocalRecording(', 'void AudioService::StopLocalRecording(', 'bool AudioService::IsLocalInputIdle()')))
PHYSICAL = PHYSICAL.replace('__APP_METHODS__', '\n'.join([
    *[method('main/application.cc', signature) for signature in ('void Application::StartListening()', 'void Application::StopListening()', 'bool Application::BeginLocalRecordingOnMain()', 'void Application::EndLocalRecordingOnMain()')],
    *[method('main/provisions_dictation_application.cc', signature) for signature in ('void Application::ToggleDictationScreen()', 'void Application::DictationButton()', 'void Application::CloseDictationInputOnMain()', 'void Application::HandleDictationControlOnMain()', 'void Application::ServiceDictation()')]]))
WORKER = WORKER.replace('int main(){', PHYSICAL + '\nint main(){').replace('dictation_cases();}', 'dictation_cases();dictation_physical_cases();dictation_main_consumer_cases();dictation_reassignment_cases();}')


class VoiceRecorderReviewTests(unittest.TestCase):
    def test_actual_worker_durability_replay_and_exact_receipts(self):
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler)
        crypto = Path("/opt/homebrew/opt/openssl@3")
        flags = ["-I", str(crypto / "include"), "-L", str(crypto / "lib")] if crypto.exists() else []
        with tempfile.TemporaryDirectory(prefix="orbit-recorder-review-") as folder:
            path = Path(folder)
            for name, source in HEADERS.items():
                header = path / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text(source)
            (path / "review.cc").write_text(PRELUDE + SUPPORT + WORKER)
            binary = path / "review"
            cjson = ROOT / "managed_components/espressif__cjson/cJSON"
            subprocess.run(["cc", "-fsanitize=address,undefined", "-I", str(cjson), "-c", str(cjson / "cJSON.c"), "-o", str(path / "json.o")], check=True)
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                            "-I", str(path), "-I", str(ROOT / "main"), "-I", str(cjson), *flags,
                            str(path / "review.cc"), str(ROOT / "main/provisions_voice_outbox.cc"),
                            str(ROOT / "main/provisions_voice_outbox_esp.cc"),
                            str(ROOT / "main/provisions_voice_recording.cc"),
                            str(ROOT / "main/provisions_voice_recorder.cc"),
                            str(ROOT / "main/provisions_dictation_recorder.cc"),
                            str(ROOT / "main/provisions_dictation.cc"), str(ROOT / "main/provisions_dictation_store.cc"),
                            str(ROOT / "main/provisions_voice_wire.cc"), str(ROOT / "main/provisions_timers.cc"), str(path / "json.o"), "-lcrypto",
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=30, env={**os.environ, "ASAN_OPTIONS":
                "detect_leaks=0" if sys.platform == "darwin" else "detect_leaks=1"})


if __name__ == "__main__":
    unittest.main()
