"""Compile actual local-feedback/audio workers with real Ogg assets and sanitizer checks."""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def method(source, signature):
    start = source.index(signature)
    cursor = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[cursor] == "{") - (source[cursor] == "}")
        cursor += 1
    return source[start:cursor]


PROGRAM = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include "ogg_demuxer.h"
#define CONFIG_PROVISIONS_LOCAL_CAPTURE 1
#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1
#define CONFIG_USE_SERVER_AEC 0
#define MAX_PLAYBACK_TASKS_IN_QUEUE 2
#define MAX_SEND_PACKETS_IN_QUEUE 40
#define OPUS_FRAME_DURATION_MS 60
#define AS_EVENT_AUDIO_TESTING_RUNNING 1
#define AS_EVENT_WAKE_WORD_RUNNING 2
#define AS_EVENT_AUDIO_PROCESSOR_RUNNING 4
#define AUDIO_POWER_CHECK_INTERVAL_MS 1000
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
using namespace std::chrono_literals;
void esp_timer_stop(int){}void esp_timer_start_periodic(int,int){}void xEventGroupSetBits(int,int){}
constexpr int ESP_AUDIO_DEC_RECOVERY_NONE=0,ESP_AUDIO_ERR_OK=0;
struct esp_audio_dec_in_raw_t {uint8_t* buffer;uint32_t len,consumed;int frame_recover;};
struct esp_audio_dec_out_frame_t {uint8_t* buffer;uint32_t len,decoded_size;};
struct esp_audio_dec_info_t {};
struct esp_audio_enc_in_frame_t {uint8_t* buffer;uint32_t len;};
struct esp_audio_enc_out_frame_t {uint8_t* buffer;uint32_t len,encoded_bytes;};
uint16_t fingerprint(const uint8_t* data,size_t size){uint16_t value=31;for(size_t i=0;i<size;++i)value=value*33+data[i];return value;}
struct Decoder {
    std::atomic<bool> block{false},entered{false},release{false};std::atomic<int> calls{0},resets{0};
};
int esp_opus_dec_decode(void* raw_decoder,esp_audio_dec_in_raw_t* raw,esp_audio_dec_out_frame_t* out,esp_audio_dec_info_t*){
    auto& decoder=*static_cast<Decoder*>(raw_decoder);++decoder.calls;
    if(decoder.block.exchange(false)){decoder.entered=true;while(!decoder.release)std::this_thread::sleep_for(100us);}
    reinterpret_cast<int16_t*>(out->buffer)[0]=fingerprint(raw->buffer,raw->len);out->decoded_size=2;return ESP_AUDIO_ERR_OK;
}
void esp_opus_dec_reset(void* raw){++static_cast<Decoder*>(raw)->resets;}
int esp_opus_enc_process(void*,esp_audio_enc_in_frame_t*,esp_audio_enc_out_frame_t*){return -1;}
using esp_ae_sample_t=void*;
void esp_ae_rate_cvt_get_max_out_sample_num(void*,size_t size,uint32_t* out){*out=size;}
void esp_ae_rate_cvt_process(void*,esp_ae_sample_t,size_t,esp_ae_sample_t,uint32_t*){}
struct AudioStreamPacket {
    int sample_rate=0,frame_duration=0;std::vector<uint8_t> payload;
    uint32_t timestamp=0,playback_id=0,media_position_ms=0,voice_upload_generation=0;
};
enum AudioTaskType {kAudioTaskTypeEncodeToSendQueue,kAudioTaskTypeEncodeToTestingQueue,kAudioTaskTypeDecodeToPlaybackQueue};
struct AudioTask {
    AudioTaskType type{};std::vector<int16_t> pcm;
    uint32_t timestamp=0,playback_id=0,media_position_ms=0,voice_upload_generation=0;
};
struct Callbacks {
    std::function<void()> on_playback_drained,on_send_queue_available;
    std::function<void(uint32_t,uint32_t)> on_playback_progress;
    std::function<void(uint32_t)> on_recording_error;
};
struct Codec {
    std::mutex mutex;std::vector<int16_t> played;bool output_enabled(){return true;}void EnableOutput(bool){}
    int output_sample_rate(){return 24000;}
    void OutputData(const std::vector<int16_t>& pcm){std::lock_guard<std::mutex> lock(mutex);played.push_back(pcm.at(0));}
    size_t size(){std::lock_guard<std::mutex> lock(mutex);return played.size();}
};
struct AudioService {
    std::atomic<bool> service_stopped_{false};std::atomic<uint32_t> local_recording_press_{0};
    std::mutex audio_queue_mutex_,decoder_mutex_;std::condition_variable audio_queue_cv_;
    std::string_view local_feedback_;size_t local_feedback_offset_=0;bool local_feedback_active_=false;
    OggDemuxer local_feedback_demuxer_;
    std::deque<std::unique_ptr<AudioTask>> audio_encode_queue_,audio_playback_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_send_queue_,audio_decode_queue_,audio_testing_queue_;
    std::deque<uint32_t> timestamp_queue_;
    bool decode_in_flight_=false,output_in_flight_=false,playback_drained_notified_=true;uint32_t playback_generation_=0;
    Decoder decoder;void* opus_decoder_=&decoder;void* opus_encoder_=nullptr;void* output_resampler_=nullptr;
    size_t decoder_frame_size_=1440,encoder_frame_size_=960,encoder_outbuf_size_=2048;
    int decoder_sample_rate_=24000,audio_power_timer_=0,event_group_=0;
    Codec codec;Codec* codec_=&codec;Callbacks callbacks_;
    struct {uint32_t playback_count=0,decode_count=0,encode_count=0;}debug_statistics_;
    struct {bool Allows(uint32_t){return false;}}voice_upload_gate_;
    std::chrono::steady_clock::time_point last_output_time_;
    bool PlayLocalFeedback(const std::string_view&);void CancelLocalFeedback();void FillLocalFeedbackLocked();
    bool IsPlaybackDrainedLocked() const;bool MarkPlaybackDrainedLocked();
    void OpusCodecTask();void AudioOutputTask();void ResetDecoder();void Stop();
    void StopLocalRecording(){local_recording_press_=0;}void CloseVoiceUploadGate(){}
    void SetDecodeSampleRate(int rate,int duration){assert((rate==8000 || rate==12000 || rate==16000 || rate==24000 || rate==48000) && duration==60);}
};
'''

CASES = r'''
template<typename F>void wait_for(F predicate){auto end=std::chrono::steady_clock::now()+2s;while(!predicate() && std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(100us);assert(predicate());}
std::string load(const char* path){std::ifstream input(path,std::ios::binary);assert(input);return {std::istreambuf_iterator<char>(input),{}};}
std::vector<AudioStreamPacket> demux(const std::string& sound){
    OggDemuxer parser;std::vector<AudioStreamPacket> result;
    parser.OnPacket([&](const uint8_t* data,int rate,int duration,size_t size){AudioStreamPacket packet;packet.sample_rate=rate;packet.frame_duration=duration;packet.payload.assign(data,data+size);result.push_back(packet);});
    assert(parser.Process(reinterpret_cast<const uint8_t*>(sound.data()),sound.size())==sound.size());assert(parser.Finish());return result;
}
void assert_fast(std::function<void()> call){auto start=std::chrono::steady_clock::now();call();assert(std::chrono::steady_clock::now()-start<100ms);}
int main(int argc,char** argv){assert(argc==5);const std::string test=argv[1],saved=load(argv[2]),failed=load(argv[3]),success=load(argv[4]);
    if(test=="packet_exactness"){
        for(const auto* sound:{&saved,&failed,&success}){
            auto reference=demux(*sound);assert(!reference.empty());AudioService audio;assert(audio.PlayLocalFeedback(*sound));size_t count=0;
            std::lock_guard<std::mutex> lock(audio.audio_queue_mutex_);
            while(!audio.local_feedback_.empty()){
                audio.FillLocalFeedbackLocked();assert(audio.audio_decode_queue_.size()<=1);
                if(!audio.audio_decode_queue_.empty()){
                    auto packet=std::move(audio.audio_decode_queue_.front());audio.audio_decode_queue_.pop_front();
                    assert(count<reference.size() && packet->payload==reference[count].payload);
                    assert(packet->sample_rate==reference[count].sample_rate && packet->frame_duration==60);if(sound!=&success)assert(packet->sample_rate==24000);++count;
                }
            }
            assert(count==reference.size());assert(audio.IsPlaybackDrainedLocked());
            assert(audio.MarkPlaybackDrainedLocked());assert(!audio.local_feedback_active_);
        }
    }else if(test=="publish_cancel"){
        AudioService audio;audio.audio_playback_queue_.push_back(std::make_unique<AudioTask>());audio.audio_decode_queue_.push_back(std::make_unique<AudioStreamPacket>());
        assert_fast([&]{assert(audio.PlayLocalFeedback(saved));});assert(audio.local_feedback_offset_==0 && audio.decoder.calls==0);
        assert(audio.audio_playback_queue_.empty() && audio.audio_decode_queue_.empty());auto generation=audio.playback_generation_;
        assert_fast([&]{audio.CancelLocalFeedback();});assert(audio.playback_generation_==generation+1 && audio.local_feedback_.empty() && !audio.local_feedback_active_);
        audio.CancelLocalFeedback();assert(audio.playback_generation_==generation+1);
        audio.local_recording_press_=1;assert(!audio.PlayLocalFeedback(saved));audio.local_recording_press_=0;
        assert(!audio.PlayLocalFeedback({}));const std::string oversized(32769,'x');assert(!audio.PlayLocalFeedback(oversized));
        audio.service_stopped_=true;assert(!audio.PlayLocalFeedback(saved));
    }else if(test=="cancel_during_decode"){
        AudioService audio;audio.decoder.block=true;assert(audio.PlayLocalFeedback(saved));std::thread worker([&]{audio.OpusCodecTask();});
        wait_for([&]{return audio.decoder.entered.load();});assert_fast([&]{audio.CancelLocalFeedback();});
        audio.decoder.release=true;
        wait_for([&]{std::lock_guard<std::mutex> lock(audio.audio_queue_mutex_);return !audio.decode_in_flight_;});
        {std::lock_guard<std::mutex> lock(audio.audio_queue_mutex_);assert(audio.audio_playback_queue_.empty() && audio.local_feedback_.empty());}
        audio.Stop();worker.join();
    }else if(test=="replace_during_decode"){
        AudioService audio;audio.decoder.block=true;assert(audio.PlayLocalFeedback(saved));std::thread worker([&]{audio.OpusCodecTask();});
        wait_for([&]{return audio.decoder.entered.load();});assert_fast([&]{audio.CancelLocalFeedback();assert(audio.PlayLocalFeedback(failed));});
        audio.decoder.release=true;
        wait_for([&]{std::lock_guard<std::mutex> lock(audio.audio_queue_mutex_);return audio.audio_playback_queue_.size()==2;});
        auto reference=demux(failed);
        {std::lock_guard<std::mutex> lock(audio.audio_queue_mutex_);size_t index=0;for(const auto& task:audio.audio_playback_queue_){assert(static_cast<uint16_t>(task->pcm.at(0))==fingerprint(reference[index].payload.data(),reference[index].payload.size()));++index;}assert(!audio.local_feedback_.empty());}
        std::this_thread::sleep_for(5ms);{std::lock_guard<std::mutex> lock(audio.audio_queue_mutex_);assert(audio.audio_playback_queue_.size()==2 && audio.audio_decode_queue_.empty());}
        audio.Stop();worker.join();
    }else if(test=="complete_playback"){
        for(const auto* sound:{&saved,&failed,&success}){
            AudioService audio;std::atomic<int> drained{0};audio.callbacks_.on_playback_drained=[&]{++drained;};
            auto reference=demux(*sound);assert(audio.PlayLocalFeedback(*sound));
            std::thread worker([&]{audio.OpusCodecTask();}),output([&]{audio.AudioOutputTask();});
            wait_for([&]{return drained.load()==1;});
            assert(audio.codec.size()==reference.size());
            {std::lock_guard<std::mutex> lock(audio.codec.mutex);for(size_t i=0;i<reference.size();++i)assert(static_cast<uint16_t>(audio.codec.played[i])==fingerprint(reference[i].payload.data(),reference[i].payload.size()));}
            {std::lock_guard<std::mutex> lock(audio.audio_queue_mutex_);assert(!audio.local_feedback_active_ && audio.IsPlaybackDrainedLocked());}
            audio.Stop();worker.join();output.join();assert(drained==1);
        }
    }else if(test=="reset_and_stop"){
        for(bool stop:{false,true}){AudioService audio;std::atomic<int> drained{0};audio.callbacks_.on_playback_drained=[&]{++drained;};assert(audio.PlayLocalFeedback(saved));
            {std::lock_guard<std::mutex> lock(audio.audio_queue_mutex_);audio.FillLocalFeedbackLocked();assert(audio.audio_decode_queue_.size()==1);}
            if(stop)audio.Stop();else audio.ResetDecoder();
            assert(audio.local_feedback_.empty() && !audio.local_feedback_active_ && audio.audio_decode_queue_.empty() && audio.audio_playback_queue_.empty());assert(drained==1);
            if(!stop)assert(audio.PlayLocalFeedback(failed));
        }
    }else if(test=="valid_empty_tail"){
        std::string tail=saved;std::string page(27,'\0');page.replace(0,4,"OggS");tail+=page;
        assert(!demux(tail).empty());AudioService audio;std::atomic<int> drained{0};audio.callbacks_.on_playback_drained=[&]{++drained;};assert(audio.PlayLocalFeedback(tail));
        // Make the final tail-only iteration deterministic: every preceding
        // packet has already been consumed, so no output task can notify later.
        {std::lock_guard<std::mutex> lock(audio.audio_queue_mutex_);
            while(audio.local_feedback_offset_<saved.size()){audio.FillLocalFeedbackLocked();audio.audio_decode_queue_.clear();}
            assert(!audio.local_feedback_.empty() && !audio.IsPlaybackDrainedLocked());}
        std::thread worker([&]{audio.OpusCodecTask();});
        wait_for([&]{return drained.load()==1;});
        {std::lock_guard<std::mutex> lock(audio.audio_queue_mutex_);assert(audio.IsPlaybackDrainedLocked() && !audio.local_feedback_active_);}
        audio.Stop();worker.join();assert(drained==1);
    }else assert(false);
}
'''


class ProvisionsLocalFeedbackReview(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / "main/audio/audio_service.cc").read_text()
        actual = "\n".join(method(source, signature) for signature in (
            "bool AudioService::PlayLocalFeedback(const std::string_view& sound)",
            "void AudioService::CancelLocalFeedback()",
            "void AudioService::FillLocalFeedbackLocked()",
            "bool AudioService::IsPlaybackDrainedLocked() const",
            "bool AudioService::MarkPlaybackDrainedLocked()",
            "void AudioService::ResetDecoder()",
            "void AudioService::Stop()",
            "void AudioService::OpusCodecTask()",
            "void AudioService::AudioOutputTask()",
        ))
        cls.directory = tempfile.TemporaryDirectory(prefix="orbit-feedback-review-")
        cls.path = Path(cls.directory.name)
        (cls.path / "esp_log.h").write_text("#pragma once\n#define ESP_LOGE(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n#define ESP_LOGD(...) ((void)0)\n")
        (cls.path / "review.cc").write_text(PROGRAM + actual + CASES)
        cls.binary = cls.path / "review"
        built = subprocess.run([shutil.which("c++"), "-std=c++17", "-pthread", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                                "-I", str(cls.path), "-I", str(ROOT / "main/audio/demuxer"), str(cls.path / "review.cc"),
                                str(ROOT / "main/audio/demuxer/ogg_demuxer.cc"), "-o", str(cls.binary)], capture_output=True, text=True)
        if built.returncode:
            raise RuntimeError(built.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case, str(ROOT / "main/assets/provisions/saved_on_orbit.ogg"),
                                 str(ROOT / "main/assets/provisions/could_not_save.ogg"), str(ROOT / "main/assets/common/success.ogg")], capture_output=True, text=True, timeout=10,
                                env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0" if sys.platform == "darwin" else "detect_leaks=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_assets_match_manifest_and_packet_delivery(self):
        manifest = json.loads((ROOT / "main/assets/provisions/voice-feedback.json").read_text())
        for entry in manifest["phrases"]:
            data = (ROOT / "main/assets/provisions" / entry["file"]).read_bytes()
            self.assertEqual(len(data), entry["bytes"])
            self.assertEqual(hashlib.sha256(data).hexdigest(), entry["sha256"])
        self.run_case("packet_exactness")

    def test_controls_are_bounded_and_reject_capture_overlap(self):
        self.run_case("publish_cancel")

    def test_cancel_and_replace_during_actual_codec_work(self):
        for case in ("cancel_during_decode", "replace_during_decode"):
            with self.subTest(case=case):
                self.run_case(case)

    def test_full_worker_output_and_final_drain(self):
        self.run_case("complete_playback")

    def test_reset_and_stop_clear_pending_feedback(self):
        self.run_case("reset_and_stop")

    def test_valid_empty_ogg_tail_notifies_drain(self):
        self.run_case("valid_empty_tail")


if __name__ == "__main__":
    unittest.main()
