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
struct esp_audio_enc_out_frame_t {uint8_t* buffer;uint32_t len;uint32_t encoded_bytes;};
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
'''
SUPPORT = fixture.PROGRAM.split("int main() {")[0].replace(
    "bytes==VoiceOutbox::kMaxFrameBytes && caps==",
    "(bytes==VoiceOutbox::kMaxFrameBytes || bytes==VoiceRecording::kMaxSamples*sizeof(int16_t)) && caps==").replace(
        "n==fail_n", "(fail_n==0 || n==fail_n)")

WORKER = r'''
struct TestTask {
    std::mutex mutex;std::condition_variable changed;unsigned notifications=0;bool waiting=false;
    std::thread thread;
};
TestTask* task=nullptr;
thread_local TestTask* current_task=nullptr;
std::atomic<int64_t> clock_us{0};
std::vector<std::pair<VoiceRecorder::Result,uint32_t>> notices;
struct Offered {VoiceCaptureReceipt receipt;uint32_t press;size_t slot;};
std::vector<Offered> offered;
std::shared_ptr<const VoiceReplay> held;
bool hold_replay=false;
bool fail_encoder=false;
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
    value->changed.wait(lock,[&]{return value->notifications>0;});value->waiting=false;
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
    assert(recorder.Start([](auto result,uint32_t press){notices.emplace_back(result,press);},
        [](auto replay){
            VoiceCaptureReceipt receipt;receipt.capture=replay->capture;receipt.bytes=replay->bytes;
            receipt.digest=replay->digest;offered.push_back({receipt,replay->press,replay->slot});
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
    assert(!held&&!task);reset();clock_us=0;notices.clear();offered.clear();hold_replay=false;fail_encoder=false;
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
int main(){normal_cases();context_cases();}
'''


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
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                            "-I", str(path), "-I", str(ROOT / "main"), *flags,
                            str(path / "review.cc"), str(ROOT / "main/provisions_voice_outbox.cc"),
                            str(ROOT / "main/provisions_voice_outbox_esp.cc"),
                            str(ROOT / "main/provisions_voice_recording.cc"),
                            str(ROOT / "main/provisions_voice_recorder.cc"), "-lcrypto",
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=30, env={**os.environ, "ASAN_OPTIONS":
                "detect_leaks=0" if sys.platform == "darwin" else "detect_leaks=1"})


if __name__ == "__main__":
    unittest.main()
