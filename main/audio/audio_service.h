#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <model_path.h>
#include "esp_ae_rate_cvt.h"
#include "esp_audio_enc.h"
#include "esp_audio_types.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"

#include "audio/voice_upload_gate.h"
#include "audio_codec.h"
#include "audio_debugger.h"
#include "audio_engine.h"
#include "ogg_demuxer.h"
#include "protocol.h"

/*
 * There are two types of audio data flow:
 * 1. (MIC) -> [Audio Engine] -> {Encode Queue} -> [Opus Encoder] -> {Send Queue} -> (Server)
 * 2. (Server) -> {Decode Queue} -> [Opus Decoder] -> {Playback Queue} -> (Speaker)
 *
 * We use dedicated tasks for input, output, and Opus encoding/decoding.
 *
 * Decode Queue and Send Queue are the main queues, because Opus packets are quite smaller than PCM
 * packets.
 *
 */

#define OPUS_FRAME_DURATION_MS 60
#define MAX_ENCODE_TASKS_IN_QUEUE 2
#define MAX_PLAYBACK_TASKS_IN_QUEUE 2
#define MAX_DECODE_PACKETS_IN_QUEUE (1200 / OPUS_FRAME_DURATION_MS)
#define MAX_SEND_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define AUDIO_TESTING_MAX_DURATION_MS 10000
#define MAX_TIMESTAMPS_IN_QUEUE 3

#define AUDIO_POWER_TIMEOUT_MS 15000
#define AUDIO_POWER_CHECK_INTERVAL_MS 1000

#define AS_EVENT_AUDIO_TESTING_RUNNING (1 << 0)
#define AS_EVENT_WAKE_WORD_RUNNING (1 << 1)
#define AS_EVENT_AUDIO_PROCESSOR_RUNNING (1 << 2)
#define AS_EVENT_AUDIO_INPUT_STOP_REQUEST (1 << 4)
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
#define AS_EVENT_LOCAL_RECORDING_RUNNING (1 << 5)
#endif

#define AS_OPUS_GET_FRAME_DRU_ENUM(duration_ms)                  \
    ((duration_ms) == 5     ? ESP_OPUS_ENC_FRAME_DURATION_5_MS   \
     : (duration_ms) == 10  ? ESP_OPUS_ENC_FRAME_DURATION_10_MS  \
     : (duration_ms) == 20  ? ESP_OPUS_ENC_FRAME_DURATION_20_MS  \
     : (duration_ms) == 40  ? ESP_OPUS_ENC_FRAME_DURATION_40_MS  \
     : (duration_ms) == 60  ? ESP_OPUS_ENC_FRAME_DURATION_60_MS  \
     : (duration_ms) == 80  ? ESP_OPUS_ENC_FRAME_DURATION_80_MS  \
     : (duration_ms) == 100 ? ESP_OPUS_ENC_FRAME_DURATION_100_MS \
     : (duration_ms) == 120 ? ESP_OPUS_ENC_FRAME_DURATION_120_MS \
                            : -1)

#define AS_OPUS_ENC_CONFIG()                                                                   \
    {                                                                                          \
        .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,                                              \
        .channel = ESP_AUDIO_MONO,                                                             \
        .bits_per_sample = ESP_AUDIO_BIT16,                                                    \
        .bitrate = ESP_OPUS_BITRATE_AUTO,                                                      \
        .frame_duration =                                                                      \
            (esp_opus_enc_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(OPUS_FRAME_DURATION_MS), \
        .application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO,                                    \
        .complexity = 0,                                                                       \
        .enable_fec = false,                                                                   \
        .enable_dtx = true,                                                                    \
        .enable_vbr = true,                                                                    \
    }

struct AudioServiceCallbacks {
    std::function<void(void)> on_send_queue_available;
    std::function<void(const std::string&)> on_wake_word_detected;
    std::function<void(bool)> on_vad_change;
    std::function<void(void)> on_audio_testing_queue_full;
    // Fired when the decode/playback queues and their in-flight work are drained.
    std::function<void(void)> on_playback_drained;
    std::function<void(uint32_t playback_id)> on_playback_error;
    std::function<void(uint32_t playback_id, uint32_t media_position_ms)> on_playback_progress;
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    // Called on the input task for a bounded 10 ms chunk. The receiver copies
    // into preallocated local capture memory; it must not encode, write or send.
    std::function<void(uint32_t, const int16_t*, size_t, size_t)> on_recording_audio;
    std::function<void(uint32_t)> on_recording_error;
    std::function<void(uint32_t)> on_recording_ready;
#endif
};

enum AudioTaskType {
    kAudioTaskTypeEncodeToSendQueue,
    kAudioTaskTypeEncodeToTestingQueue,
    kAudioTaskTypeDecodeToPlaybackQueue,
};

struct AudioTask {
    AudioTaskType type;
    std::vector<int16_t> pcm;
    uint32_t voice_upload_generation = 0;
    uint32_t playback_generation = 0;
    uint32_t timestamp = 0;
    uint32_t playback_id = 0;
    uint32_t media_position_ms = 0;
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
    uint64_t ordinary_owner = 0;
#endif
};

struct DebugStatistics {
    uint32_t input_count = 0;
    uint32_t decode_count = 0;
    uint32_t encode_count = 0;
    uint32_t playback_count = 0;
    uint32_t encode_drop_count = 0;
};

class AudioService {
public:
    AudioService();
    ~AudioService();

    void Initialize(AudioCodec* codec);
    void Start();
    void Stop();
    void EncodeWakeWord();
    std::unique_ptr<AudioStreamPacket> PopWakeWordPacket();
    const std::string& GetLastWakeWord() const;
    bool IsVoiceDetected() const { return voice_detected_; }
    bool IsIdle();
    bool IsPlaybackIdle();
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
    struct FenceSnapshot {
        provisions::audio_admission::ClosureSnapshot metadata;
        bool supported = false, input_closed = false, output_closed = false;
    };
    class CapturePermit final {
    public:
        CapturePermit(const CapturePermit&) = delete;
        CapturePermit& operator=(const CapturePermit&) = delete;
        uint32_t Press() const { return press_; }
        uint64_t InputGeneration() const { return input_generation_; }
        std::optional<uint64_t> SourceInputGeneration() const { return source_input_generation_; }
        bool IsSealedReplay() const { return replay_; }

    private:
        friend class AudioService;
        CapturePermit(uint32_t press, uint64_t generation, std::optional<uint64_t> source,
                      bool replay)
            : press_(press),
              input_generation_(generation),
              source_input_generation_(source),
              replay_(replay) {}
        const uint32_t press_;
        const uint64_t input_generation_;
        const std::optional<uint64_t> source_input_generation_;
        const bool replay_;
        mutable provisions::audio_admission::Reservation token_;
        // No RAII release: dropping a holder cannot prove actual work has settled.
    };
    using CapturePermitPtr = std::shared_ptr<const CapturePermit>;
    struct CaptureClosureSnapshot {
        uint64_t gate_generation = 0, input_generation = 0, closed_input_generation = 0;
        uint32_t press = 0, active_press = 0, closed_press = 0;
        size_t preparation = 0, read_or_append = 0, encode = 0, recorder_work = 0, upload = 0;
        bool exact_parent = false, input_sealed = false, workers_closed = false;
    };
    CapturePermitPtr ReserveCaptureParent(uint32_t press);
    // Root must provide validated immutable loader lineage. Zero press is ONLY post-restart.
    CapturePermitPtr ReserveSealedReplayParent(uint32_t source_press,
                                               std::optional<uint64_t> source_input_generation,
                                               bool post_restart_loader);
    bool StartLocalRecording(uint32_t press, const CapturePermitPtr& permit);
    bool SealCaptureInput(const CapturePermitPtr& permit);
    bool ReserveCaptureWork(const CapturePermitPtr& permit,
                            provisions::audio_admission::Producer producer,
                            provisions::audio_admission::Reservation& reservation);
    bool CanPublishCaptureWork(const CapturePermitPtr& permit,
                               const provisions::audio_admission::Reservation& reservation);
    bool CompleteCaptureWork(const CapturePermitPtr& permit,
                             provisions::audio_admission::Reservation& reservation);
    bool ReleaseCaptureParent(const CapturePermitPtr& permit);
    CaptureClosureSnapshot GetCaptureClosureSnapshot(const CapturePermitPtr& permit);
    provisions::audio_admission::Gate& AudioAdmission() { return audio_admission_; }
    uint64_t BeginAudioFenceClose();
    bool HoldAudioFence(const provisions::audio_admission::FenceIdentity& identity,
                        uint64_t generation);
    FenceSnapshot GetAudioFenceSnapshot(const provisions::audio_admission::FenceIdentity& identity);
    bool OpenAudioFenceAfterTerminal(const provisions::audio_admission::FenceIdentity& identity,
                                     uint64_t generation);
    bool ReserveTimerPreparation(const provisions::audio_admission::TimerIdentity& identity);
    bool ReleaseTimerPreparation(const provisions::audio_admission::TimerIdentity& identity);
    bool ClaimRetainedTimerOutput(uint32_t id,
                                  const provisions::audio_admission::TimerIdentity& identity);
    bool PushTimerPacket(uint32_t owner, std::unique_ptr<AudioStreamPacket> packet);
    bool BeginOrdinaryOutput(uint64_t owner);
    bool PushOrdinaryPacket(uint64_t owner, std::unique_ptr<AudioStreamPacket> packet);
    bool SealOrdinaryOutput(uint64_t owner, bool discard = false);
    bool IsOrdinaryOutputClosed(uint64_t owner);
    bool RetireOrdinaryOutput(uint64_t owner);
    void DiscardAudioTesting();
#endif
    bool IsWakeWordRunning() const {
        return xEventGroupGetBits(event_group_) & AS_EVENT_WAKE_WORD_RUNNING;
    }
    bool IsAudioProcessorRunning() const {
        return xEventGroupGetBits(event_group_) & AS_EVENT_AUDIO_PROCESSOR_RUNNING;
    }
    bool IsAfeWakeWord();

    void EnableWakeWordDetection(bool enable);
    void ReleaseWakeWordResources();
    void EnableVoiceProcessing(bool enable);
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    // Button-task operations: atomics only, no locks, allocation or I/O.
    void FenceLocalRecording(uint32_t press);
    void ReleaseLocalRecordingFence(uint32_t press);
    // Main-task reconciliation after queues/decoder have been cancelled.
    void ReconcileLocalRecording(uint32_t press);
    void StartLocalRecording(uint32_t press);
    void StopLocalRecording(uint32_t expected_press = 0);
    // Includes a discarded in-flight read, not just the physical button flag.
    bool IsLocalRecordingClosed(uint32_t press) const;
    // A closed older press alone does not imply that a newer press is idle.
    bool IsLocalInputIdle() const;
    bool IsLocalRecordingReady(uint32_t press) const;
#endif
    void CloseVoiceUploadGate();
    template <typename Action>
    bool WithVoiceUploadLease(const AudioStreamPacket& packet, Action&& action) {
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
        (void)packet;
        (void)action;
        return false;  // Unsettled realtime/AFE workers are unsupported in the matched build.
#else
        return voice_upload_gate_.WithSendLease(packet.voice_upload_generation,
                                                std::forward<Action>(action));
#endif
    }
    void EnableAudioTesting(bool enable);
    void EnableDeviceAec(bool enable);

    void SetCallbacks(AudioServiceCallbacks& callbacks);

    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait = false);
    // True when a non-waiting PushPacketToDecodeQueue() would not be refused
    // for lack of room. Lets a producer keep a frame it cannot hand over yet
    // instead of losing it inside the moved-from push.
    bool HasDecodeQueueRoom();
    std::unique_ptr<AudioStreamPacket> PopPacketFromSendQueue();
    void PlaySound(const std::string_view& sound);
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
    // A timer owns output until its durable terminal fact is acknowledged.
    bool ClaimTimerOutput(uint32_t id) {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
        if (!timer_preparation_work_ || !timer_preparation_work_->Allowed() || ordinary_owner_ != 0)
            return false;
#endif
        if (id == 0 || !IsPlaybackDrainedLocked() || local_recording_press_.load() != 0 ||
            local_input_press_.load() != 0 ||
            local_physical_boundary_.load() != local_output_boundary_.load())
            return false;
        uint32_t empty = 0;
        if (!timer_output_owner_.compare_exchange_strong(empty, id))
            return false;
        // The physical button fence is deliberately lock-free. Recheck it after
        // the claim so an edge arriving during admission retains input priority.
        if (local_recording_press_.load() != 0 || local_input_press_.load() != 0 ||
            local_physical_boundary_.load() != local_output_boundary_.load()) {
            timer_output_owner_.compare_exchange_strong(id, 0);
            return false;
        }
        return true;
    }
    bool ReleaseTimerOutput(uint32_t id) {
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        if (!IsPlaybackDrainedLocked() || id == 0 ||
            !timer_output_owner_.compare_exchange_strong(id, 0))
            return false;
        timer_recovery_work_.reset();
        return true;
#else
        return id != 0 && timer_output_owner_.compare_exchange_strong(id, 0);
#endif
    }
    // Embedded sounds only: keep their storage alive through playback. These
    // controls never demux, decode, wait for capacity or write to the codec.
    bool PlayLocalFeedback(const std::string_view& sound);
    void CancelLocalFeedback();
    // Monotonic failure observation for the local-feedback owner. A drained
    // queue can also follow a decode/output failure; it is not a success receipt.
    uint32_t LocalFeedbackErrors() const { return local_feedback_errors_.load(); }
#endif
    bool ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples);
    void ResetDecoder();
    void SetModelsList(srmodel_list_t* models_list);

private:
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
    provisions::audio_admission::Gate audio_admission_;  // Blocked before codec acquisition.
    std::atomic<uint64_t> input_closed_generation_{0}, output_closed_generation_{0};
    std::atomic<uint64_t> capture_input_generation_{0}, capture_closed_input_generation_{0};
    std::atomic<uint32_t> capture_last_press_{0}, capture_closed_press_{0};
    CapturePermitPtr capture_permit_;
    uint32_t last_capture_reserve_press_ = 0;
    std::unique_ptr<AudioAdmissionWork> testing_work_, timer_preparation_work_,
        timer_recovery_work_, ordinary_work_;
    provisions::audio_admission::TimerIdentity timer_preparation_identity_{};
    uint64_t ordinary_owner_ = 0, last_ordinary_owner_ = 0;
    bool ordinary_sealed_ = false;
    void ServiceInputFence();
    void ServiceOutputFence();
    const provisions::audio_admission::Reservation* TimerParentLocked(uint32_t id);
    bool PushFencedPacket(std::unique_ptr<AudioStreamPacket> packet, bool wait,
                          uint64_t ordinary_owner, uint32_t timer_owner = 0);
    bool OrdinaryClosedLocked(uint64_t owner) const;
    CaptureClosureSnapshot CaptureClosedLocked(const CapturePermitPtr& permit) const;
    const provisions::audio_admission::Reservation* OutputParentLocked(uint32_t timer,
                                                                       uint64_t ordinary);
#endif
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
    std::atomic<uint32_t> timer_output_owner_{0};
#endif
    AudioCodec* codec_ = nullptr;
    AudioServiceCallbacks callbacks_;
    std::unique_ptr<AudioEngine> audio_engine_;
    std::unique_ptr<AudioDebugger> audio_debugger_;
    void* opus_encoder_ = nullptr;
    void* opus_decoder_ = nullptr;
    std::mutex decoder_mutex_;
    std::mutex input_resampler_mutex_;
    esp_ae_rate_cvt_handle_t input_resampler_ = nullptr;
    esp_ae_rate_cvt_handle_t output_resampler_ = nullptr;

    // Encoder/Decoder state
    int encoder_sample_rate_ = 16000;
    int encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
    int encoder_frame_size_ = 0;
    int encoder_outbuf_size_ = 0;
    int decoder_sample_rate_ = 0;
    int decoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
    int decoder_frame_size_ = 0;
    DebugStatistics debug_statistics_;
    int64_t last_encode_drop_log_time_ = 0;
    srmodel_list_t* models_list_ = nullptr;

    EventGroupHandle_t event_group_;

    // Audio encode / decode
    TaskHandle_t audio_input_task_handle_ = nullptr;
    TaskHandle_t audio_output_task_handle_ = nullptr;
    TaskHandle_t opus_codec_task_handle_ = nullptr;
    std::mutex audio_queue_mutex_;
    std::condition_variable audio_queue_cv_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_decode_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_send_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_testing_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_encode_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_playback_queue_;
    bool decode_in_flight_ = false;
    bool output_in_flight_ = false;
    bool playback_drained_notified_ = true;
    uint32_t playback_generation_ = 0;
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
    std::string_view local_feedback_;
    size_t local_feedback_offset_ = 0;
    bool local_feedback_active_ = false;
    std::atomic<uint32_t> local_feedback_errors_{0};
    OggDemuxer local_feedback_demuxer_;
    void FillLocalFeedbackLocked();
#endif
    // For server AEC
    std::deque<uint32_t> timestamp_queue_;

    bool audio_engine_initialized_ = false;
    bool voice_detected_ = false;
#if CONFIG_USE_DEVICE_AEC
    bool device_aec_enabled_ = true;
#else
    bool device_aec_enabled_ = false;
#endif
    std::atomic<bool> service_stopped_{true};
    std::atomic<bool> audio_input_need_warmup_{false};
    VoiceUploadGate voice_upload_gate_;
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    std::mutex local_recording_mutex_;
    std::atomic<uint32_t> local_recording_press_{0};
    std::atomic<uint32_t> local_input_press_{0};
    std::atomic<uint32_t> local_prepared_press_{0};
    std::atomic<uint32_t> local_physical_boundary_{0};
    std::atomic<uint32_t> local_output_boundary_{0};
#endif

    esp_timer_handle_t audio_power_timer_ = nullptr;
    std::chrono::steady_clock::time_point last_input_time_;
    std::chrono::steady_clock::time_point last_output_time_;

    void AudioInputTask();
    void AudioOutputTask();
    void OpusCodecTask();
    void PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm);
    bool InitializeAudioEngine();
    void SetDecodeSampleRate(int sample_rate, int frame_duration);
    void CheckAndUpdateAudioPowerState();
    bool IsPlaybackDrainedLocked() const;
    bool MarkPlaybackDrainedLocked();
};

#endif
