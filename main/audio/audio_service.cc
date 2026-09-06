#include "audio_service.h"
#include <esp_log.h>
#include <cstring>

#define RATE_CVT_CFG(_src_rate, _dest_rate, _channel)                                        \
    (esp_ae_rate_cvt_cfg_t) {                                                                \
        .src_rate = (uint32_t)(_src_rate), .dest_rate = (uint32_t)(_dest_rate),              \
        .channel = (uint8_t)(_channel), .bits_per_sample = ESP_AUDIO_BIT16, .complexity = 2, \
        .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,                                        \
    }

#define OPUS_DEC_CFG(_sample_rate, _frame_duration_ms)                                     \
    (esp_opus_dec_cfg_t) {                                                                 \
        .sample_rate = (uint32_t)(_sample_rate), .channel = ESP_AUDIO_MONO,                \
        .frame_duration =                                                                  \
            (esp_opus_dec_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(_frame_duration_ms), \
        .self_delimited = false,                                                           \
    }

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31
#include "engines/afe_audio_engine.h"
#else
#include "engines/lite_audio_engine.h"
#endif

#define TAG "AudioService"

AudioService::AudioService() { event_group_ = xEventGroupCreate(); }

AudioService::~AudioService() {
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
    if (opus_encoder_ != nullptr) {
        esp_opus_enc_close(opus_encoder_);
    }
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
    }
    if (input_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(input_resampler_);
    }
    if (output_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(output_resampler_);
    }
}

void AudioService::Initialize(AudioCodec* codec) {
    codec_ = codec;
    codec_->Start();

    esp_opus_dec_cfg_t opus_dec_cfg =
        OPUS_DEC_CFG(codec->output_sample_rate(), OPUS_FRAME_DURATION_MS);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio decoder, error code: %d", ret);
    } else {
        decoder_sample_rate_ = codec->output_sample_rate();
        decoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        decoder_frame_size_ = decoder_sample_rate_ / 1000 * OPUS_FRAME_DURATION_MS;
    }
    esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
    ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &opus_encoder_);
    if (opus_encoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio encoder, error code: %d", ret);
    } else {
        encoder_sample_rate_ = 16000;
        encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        esp_opus_enc_get_frame_size(opus_encoder_, &encoder_frame_size_, &encoder_outbuf_size_);
        encoder_frame_size_ = encoder_frame_size_ / sizeof(int16_t);
    }

    if (codec->input_sample_rate() != 16000) {
        esp_ae_rate_cvt_cfg_t input_resampler_cfg = RATE_CVT_CFG(
            codec->input_sample_rate(), ESP_AUDIO_SAMPLE_RATE_16K, codec->input_channels());
        auto resampler_ret = esp_ae_rate_cvt_open(&input_resampler_cfg, &input_resampler_);
        if (input_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create input resampler, error code: %d", resampler_ret);
        }
    }

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31
    audio_engine_ = std::make_unique<AfeAudioEngine>();
#else
    audio_engine_ = std::make_unique<LiteAudioEngine>();
#endif
    audio_engine_->OnOutput([this](std::vector<int16_t>&& data) {
        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data));
    });
    audio_engine_->OnVadStateChange([this](bool speaking) {
        voice_detected_ = speaking;
        if (callbacks_.on_vad_change) {
            callbacks_.on_vad_change(speaking);
        }
    });
    audio_engine_->OnWakeWordDetected([this](const std::string& wake_word) {
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
        if (callbacks_.on_wake_word_detected) {
            callbacks_.on_wake_word_detected(wake_word);
        }
    });

    esp_timer_create_args_t audio_power_timer_args = {
        .callback =
            [](void* arg) {
                AudioService* audio_service = (AudioService*)arg;
                audio_service->CheckAndUpdateAudioPowerState();
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "audio_power_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&audio_power_timer_args, &audio_power_timer_);
}

void AudioService::Start() {
    service_stopped_.store(false);
    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING |
                                           AS_EVENT_AUDIO_PROCESSOR_RUNNING |
                                           AS_EVENT_AUDIO_INPUT_STOP_REQUEST);

    esp_timer_start_periodic(audio_power_timer_, 1000000);

#if CONFIG_USE_AUDIO_PROCESSOR
    /* Start the audio input task */
    xTaskCreatePinnedToCore(
        [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->AudioInputTask();
            vTaskDelete(NULL);
        },
        "audio_input", 2048 * 3, this, 8, &audio_input_task_handle_, 0);

    /* Start the audio output task */
    xTaskCreate(
        [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->AudioOutputTask();
            vTaskDelete(NULL);
        },
        "audio_output", 2048 * 2, this, 4, &audio_output_task_handle_);
#else
    /* Start the audio input task */
    xTaskCreate(
        [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->AudioInputTask();
            vTaskDelete(NULL);
        },
        "audio_input", 2048 * 2, this, 8, &audio_input_task_handle_);

    /* Start the audio output task */
    xTaskCreate(
        [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->AudioOutputTask();
            vTaskDelete(NULL);
        },
        "audio_output", 2048, this, 4, &audio_output_task_handle_);
#endif

    /* Start the opus codec task */
    xTaskCreate(
        [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->OpusCodecTask();
            vTaskDelete(NULL);
        },
        "opus_codec", 2048 * 12, this, 2, &opus_codec_task_handle_);
}

void AudioService::Stop() {
    esp_timer_stop(audio_power_timer_);
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    const uint32_t recording_press = local_recording_press_.load();
    StopLocalRecording();
    if (recording_press != 0 && callbacks_.on_recording_error)
        callbacks_.on_recording_error(recording_press);
#endif
    CloseVoiceUploadGate();
    service_stopped_.store(true);
    xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING |
                                         AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    bool notify_drained = false;
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        ++playback_generation_;
        audio_encode_queue_.clear();
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
        local_feedback_ = {};
        local_feedback_active_ = false;
#endif
        audio_send_queue_.clear();
        audio_decode_queue_.clear();
        audio_playback_queue_.clear();
        audio_testing_queue_.clear();
        notify_drained = MarkPlaybackDrainedLocked();
        audio_queue_cv_.notify_all();
    }
    if (notify_drained && callbacks_.on_playback_drained) {
        callbacks_.on_playback_drained();
    }
}

bool AudioService::ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples) {
    if (!codec_->input_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableInput(true);
    }

    if (codec_->input_sample_rate() != sample_rate) {
        if (input_resampler_ == nullptr)
            return false;
        data.resize(samples * codec_->input_sample_rate() / sample_rate * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
        if (input_resampler_ != nullptr) {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            uint32_t in_sample_num = data.size() / codec_->input_channels();
            uint32_t output_samples = 0;
            if (esp_ae_rate_cvt_get_max_out_sample_num(input_resampler_, in_sample_num,
                                                       &output_samples) != ESP_AE_ERR_OK ||
                output_samples == 0)
                return false;
            auto resampled = std::vector<int16_t>(output_samples * codec_->input_channels());
            uint32_t actual_output = output_samples;
            if (esp_ae_rate_cvt_process(input_resampler_, (esp_ae_sample_t)data.data(),
                                        in_sample_num, (esp_ae_sample_t)resampled.data(),
                                        &actual_output) != ESP_AE_ERR_OK ||
                actual_output == 0 || actual_output > output_samples)
                return false;
            resampled.resize(actual_output * codec_->input_channels());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
    }

    /* Update the last input time */
    last_input_time_ = std::chrono::steady_clock::now();
    debug_statistics_.input_count++;

#if CONFIG_USE_AUDIO_DEBUGGER
    // 音频调试：发送原始音频数据
    if (audio_debugger_ == nullptr) {
        audio_debugger_ = std::make_unique<AudioDebugger>();
    }
    audio_debugger_->Feed(data);
#endif

    return true;
}

void AudioService::AudioInputTask() {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    std::vector<int16_t> recording_data;
    recording_data.reserve(320);
    uint32_t prepared_press = 0;
    uint32_t preparing_press = 0;
    std::chrono::steady_clock::time_point preparation_deadline;
#endif
    constexpr EventBits_t kAudioInputActiveBits = AS_EVENT_AUDIO_TESTING_RUNNING |
                                                  AS_EVENT_WAKE_WORD_RUNNING |
                                                  AS_EVENT_AUDIO_PROCESSOR_RUNNING
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
                                                  | AS_EVENT_LOCAL_RECORDING_RUNNING
#endif
        ;

    while (true) {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        // A released read has returned before this point. Publish closure even
        // when clearing the active event would otherwise leave this task asleep.
        if (local_input_press_.load() != local_recording_press_.load())
            local_input_press_.store(0, std::memory_order_release);
#endif
        EventBits_t bits = xEventGroupWaitBits(
            event_group_, kAudioInputActiveBits | AS_EVENT_AUDIO_INPUT_STOP_REQUEST, pdFALSE,
            pdFALSE, portMAX_DELAY);

        if (service_stopped_.load()) {
            // ADC continuous mode keeps its hardware mutex from start until stop,
            // so the input task that started it must also stop it before exiting.
            if (codec_->input_enabled()) {
                codec_->EnableInput(false);
            }
            break;
        }

        if (bits & AS_EVENT_AUDIO_INPUT_STOP_REQUEST) {
            xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_INPUT_STOP_REQUEST);

            // Recheck the active state in this task. Audio capture may have been
            // enabled after the timer posted the stop request.
            bits = xEventGroupGetBits(event_group_);
            if ((bits & kAudioInputActiveBits) == 0) {
                if (codec_->input_enabled()) {
                    codec_->EnableInput(false);
                }
                // Do not process the stale active bits returned by waitBits().
                continue;
            }
        }

#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        uint32_t recording_press = 0;
        {
            std::lock_guard<std::mutex> lock(local_recording_mutex_);
            recording_press = local_recording_press_.load(std::memory_order_acquire);
            if (local_physical_boundary_.load(std::memory_order_acquire) != recording_press)
                recording_press = 0;
            local_input_press_.store(recording_press, std::memory_order_release);
        }
        if (recording_press != 0) {
            if (prepared_press != recording_press) {
                if (preparing_press != recording_press) {
                    preparing_press = recording_press;
                    preparation_deadline =
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
                }
                bool output_drained = false;
                {
                    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                    output_drained = !output_in_flight_ && codec_->IsOutputDrained();
                }
                if (!output_drained && std::chrono::steady_clock::now() < preparation_deadline) {
                    vTaskDelay(pdMS_TO_TICKS(1));
                    continue;
                }
                bool prepared = output_drained && codec_->PrepareInputCapture();
                if (prepared && input_resampler_ != nullptr) {
                    std::lock_guard<std::mutex> lock(input_resampler_mutex_);
                    prepared = esp_ae_rate_cvt_reset(input_resampler_) == ESP_AE_ERR_OK;
                }
                if (local_recording_press_.load(std::memory_order_acquire) != recording_press ||
                    local_physical_boundary_.load(std::memory_order_acquire) != recording_press)
                    continue;
                if (!prepared) {
                    StopLocalRecording(recording_press);
                    if (callbacks_.on_recording_error)
                        callbacks_.on_recording_error(recording_press);
                    continue;
                }
                prepared_press = recording_press;
                local_prepared_press_.store(recording_press, std::memory_order_release);
                if (callbacks_.on_recording_ready)
                    callbacks_.on_recording_ready(recording_press);
            }
            // Read the microphone on its owning task, independently of the
            // network and the realtime encoder's upload gate. A release (or a
            // newer press) discards an in-flight read instead of admitting any
            // post-release samples into the frozen recording.
            const bool read = ReadAudioData(recording_data, 16000, 160);
            if (local_recording_press_.load(std::memory_order_acquire) == recording_press &&
                local_physical_boundary_.load(std::memory_order_acquire) == recording_press) {
                const size_t channels = codec_->input_channels();
                if (read && (channels == 1 || channels == 2) &&
                    recording_data.size() % channels == 0 && callbacks_.on_recording_audio) {
                    callbacks_.on_recording_audio(recording_press, recording_data.data(),
                                                  recording_data.size() / channels, channels);
                } else if (callbacks_.on_recording_error) {
                    StopLocalRecording(recording_press);
                    callbacks_.on_recording_error(recording_press);
                }
            }
            continue;
        }
#endif
        if (audio_input_need_warmup_.exchange(false)) {
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        /* Used for audio testing in NetworkConfiguring mode by clicking the BOOT button */
        if (bits & AS_EVENT_AUDIO_TESTING_RUNNING) {
            if (audio_testing_queue_.size() >=
                AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS) {
                ESP_LOGW(TAG, "Audio testing queue is full, stopping audio testing");
                EnableAudioTesting(false);
                continue;
            }
            std::vector<int16_t> data;
            int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
            if (ReadAudioData(data, 16000, samples)) {
                // If input channels is 2, we need to fetch the left channel data
                if (codec_->input_channels() == 2) {
                    auto mono_data = std::vector<int16_t>(data.size() / 2);
                    for (size_t i = 0, j = 0; i < mono_data.size(); ++i, j += 2) {
                        mono_data[i] = data[j];
                    }
                    data = std::move(mono_data);
                }
                PushTaskToEncodeQueue(kAudioTaskTypeEncodeToTestingQueue, std::move(data));
                continue;
            }
        }

        /* Feed the selected audio engine */
        if (bits & (AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING)) {
            int samples = 160;  // 10ms
            std::vector<int16_t> data;
            if (ReadAudioData(data, 16000, samples)) {
                audio_engine_->Feed(std::move(data));
                continue;
            }
        }

        // Read timeout/error should not terminate the input task.
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGW(TAG, "Audio input task stopped");
}

void AudioService::AudioOutputTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        while (audio_playback_queue_.empty() && !service_stopped_.load()) {
            const bool drained = MarkPlaybackDrainedLocked();
            if (drained && callbacks_.on_playback_drained) {
                lock.unlock();
                callbacks_.on_playback_drained();
                lock.lock();
            }
            if (!audio_playback_queue_.empty() || service_stopped_.load())
                break;
            if (!playback_drained_notified_) {
                audio_queue_cv_.wait_for(lock, std::chrono::milliseconds(10));
            } else {
                audio_queue_cv_.wait(lock);
            }
        }
        if (service_stopped_.load()) {
            break;
        }

        auto task = std::move(audio_playback_queue_.front());
        audio_playback_queue_.pop_front();
        output_in_flight_ = true;
        audio_queue_cv_.notify_all();
        lock.unlock();

        if (!codec_->output_enabled()) {
            esp_timer_stop(audio_power_timer_);
            esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
            codec_->EnableOutput(true);
        }

        lock.lock();
        const bool current =
            task->playback_generation == playback_generation_ && !service_stopped_.load()
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
            && local_recording_press_.load() == 0 &&
            local_physical_boundary_.load() == local_output_boundary_.load()
#endif
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
            && (timer_output_owner_.load() == 0 || timer_output_owner_.load() == task->playback_id)
#endif
            ;
        lock.unlock();
        bool played = false;
        if (current)
            played = codec_->OutputData(task->pcm);
        if (played && task->playback_id != 0 && callbacks_.on_playback_progress) {
            callbacks_.on_playback_progress(task->playback_id, task->media_position_ms);
        }

        /* Update the last output time */
        if (played) {
            last_output_time_ = std::chrono::steady_clock::now();
            debug_statistics_.playback_count++;
        }

        bool notify_drained = false;
        lock.lock();
        const bool failed = current && !played && task->playback_generation == playback_generation_;
        if (failed) {
            ++playback_generation_;
            audio_decode_queue_.clear();
            audio_playback_queue_.clear();
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
            local_feedback_ = {};
            local_feedback_active_ = false;
#endif
        }
#if CONFIG_USE_SERVER_AEC
        /* Record the timestamp for server AEC */
        if (task->timestamp > 0) {
            timestamp_queue_.push_back(task->timestamp);
        }
#endif
        // Keep output in flight while failure consumers invalidate delivery.
        // Otherwise an older queued drain event could observe empty queues and
        // report successful completion before this error callback arrives.
        if (failed && callbacks_.on_playback_error) {
            lock.unlock();
            callbacks_.on_playback_error(task->playback_id);
            lock.lock();
        }
        output_in_flight_ = false;
        notify_drained = MarkPlaybackDrainedLocked();
        audio_queue_cv_.notify_all();
        lock.unlock();

        if (notify_drained && callbacks_.on_playback_drained) {
            callbacks_.on_playback_drained();
        }
    }

    ESP_LOGW(TAG, "Audio output task stopped");
}

void AudioService::OpusCodecTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() {
            return service_stopped_.load() || !audio_encode_queue_.empty() ||
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
                   (!local_feedback_.empty() &&
                    audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE) ||
#endif
                   (!audio_decode_queue_.empty() &&
                    audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE);
        });
        if (service_stopped_.load()) {
            break;
        }

#if CONFIG_PROVISIONS_LOCAL_CAPTURE
        if (audio_decode_queue_.empty() &&
            audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE) {
            FillLocalFeedbackLocked();
            const bool notify_drained = MarkPlaybackDrainedLocked();
            if (notify_drained && callbacks_.on_playback_drained) {
                lock.unlock();
                callbacks_.on_playback_drained();
                lock.lock();
            }
        }
#endif
        /* Decode the audio from decode queue */
        if (!audio_decode_queue_.empty() &&
            audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE) {
            auto packet = std::move(audio_decode_queue_.front());
            audio_decode_queue_.pop_front();
            decode_in_flight_ = true;
            const uint32_t generation = playback_generation_;
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto task = std::make_unique<AudioTask>();
            task->type = kAudioTaskTypeDecodeToPlaybackQueue;
            task->playback_generation = generation;
            task->timestamp = packet->timestamp;
            task->playback_id = packet->playback_id;
            task->media_position_ms = packet->media_position_ms;

            SetDecodeSampleRate(packet->sample_rate, packet->frame_duration);
            bool decoded = false;
            if (opus_decoder_ != nullptr) {
                task->pcm.resize(decoder_frame_size_);
                esp_audio_dec_in_raw_t raw = {
                    .buffer = (uint8_t*)(packet->payload.data()),
                    .len = (uint32_t)(packet->payload.size()),
                    .consumed = 0,
                    .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
                };
                esp_audio_dec_out_frame_t out_frame = {
                    .buffer = (uint8_t*)(task->pcm.data()),
                    .len = (uint32_t)(task->pcm.size() * sizeof(int16_t)),
                    .decoded_size = 0,
                };
                esp_audio_dec_info_t dec_info = {};
                std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
                auto ret = esp_opus_dec_decode(opus_decoder_, &raw, &out_frame, &dec_info);
                decoder_lock.unlock();
                bool complete_packet = true;
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
                if (packet->playback_id >= 0x80000000u) {
                    complete_packet = raw.consumed == packet->payload.size() &&
                                      out_frame.decoded_size == 1440 * sizeof(int16_t) &&
                                      packet->sample_rate == 24000 && packet->frame_duration == 60;
                }
#endif
                if (ret == ESP_AUDIO_ERR_OK && complete_packet) {
                    task->pcm.resize(out_frame.decoded_size / sizeof(int16_t));
                    if (decoder_sample_rate_ != codec_->output_sample_rate() &&
                        output_resampler_ != nullptr) {
                        uint32_t target_size = 0;
                        esp_ae_rate_cvt_get_max_out_sample_num(output_resampler_, task->pcm.size(),
                                                               &target_size);
                        std::vector<int16_t> resampled(target_size);
                        uint32_t actual_output = target_size;
                        esp_ae_rate_cvt_process(output_resampler_,
                                                (esp_ae_sample_t)task->pcm.data(), task->pcm.size(),
                                                (esp_ae_sample_t)resampled.data(), &actual_output);
                        resampled.resize(actual_output);
                        task->pcm = std::move(resampled);
                    }
                    decoded = true;
                } else {
                    ESP_LOGE(TAG, "Failed to decode complete audio packet, error code: %d", ret);
                }
            } else {
                ESP_LOGE(TAG, "Audio decoder is not configured");
            }

            lock.lock();
            if (decoded && generation == playback_generation_ && !service_stopped_.load()) {
                audio_playback_queue_.push_back(std::move(task));
            }
            decode_in_flight_ = false;
            debug_statistics_.decode_count++;
            const bool notify_drained = MarkPlaybackDrainedLocked();
            audio_queue_cv_.notify_all();
            lock.unlock();
            if (notify_drained && callbacks_.on_playback_drained) {
                callbacks_.on_playback_drained();
            }
            lock.lock();
        }
        /* Encode the audio to send queue */
        if (!audio_encode_queue_.empty()) {
            auto task = std::move(audio_encode_queue_.front());
            audio_encode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto packet = std::make_unique<AudioStreamPacket>();
            packet->frame_duration = OPUS_FRAME_DURATION_MS;
            packet->sample_rate = 16000;
            packet->timestamp = task->timestamp;

            if (opus_encoder_ != nullptr && task->pcm.size() == encoder_frame_size_) {
                std::vector<uint8_t> buf(encoder_outbuf_size_);
                esp_audio_enc_in_frame_t in = {
                    .buffer = (uint8_t*)(task->pcm.data()),
                    .len = (uint32_t)(encoder_frame_size_ * sizeof(int16_t)),
                };
                esp_audio_enc_out_frame_t out = {
                    .buffer = buf.data(),
                    .len = (uint32_t)encoder_outbuf_size_,
                    .encoded_bytes = 0,
                };
                auto ret = esp_opus_enc_process(opus_encoder_, &in, &out);
                if (ret == ESP_AUDIO_ERR_OK) {
                    packet->payload.assign(buf.data(), buf.data() + out.encoded_bytes);
                    packet->voice_upload_generation = task->voice_upload_generation;

                    if (task->type == kAudioTaskTypeEncodeToSendQueue) {
                        bool queued = false;
                        {
                            std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                            /* A Talk release increments the generation before it drains
                             * queues. This second check prevents an encoder that was
                             * already in flight from publishing after release, including
                             * if a new Talk turn has since started. */
                            if (voice_upload_gate_.Allows(task->voice_upload_generation)) {
                                /* Never let a full send queue stall encoding: stale realtime
                                 * audio is useless to the server, so drop the oldest packet. */
                                if (audio_send_queue_.size() >= MAX_SEND_PACKETS_IN_QUEUE) {
                                    audio_send_queue_.pop_front();
                                }
                                audio_send_queue_.push_back(std::move(packet));
                                queued = true;
                            }
                        }
                        if (queued && callbacks_.on_send_queue_available) {
                            callbacks_.on_send_queue_available();
                        }
                    } else if (task->type == kAudioTaskTypeEncodeToTestingQueue) {
                        std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                        audio_testing_queue_.push_back(std::move(packet));
                    }
                    debug_statistics_.encode_count++;
                } else {
                    ESP_LOGE(TAG, "Failed to encode audio, error code: %d", ret);
                }
            } else {
                ESP_LOGE(TAG,
                         "Failed to encode audio: encoder not configured or invalid frame size "
                         "(got %u, expected %u)",
                         task->pcm.size(), encoder_frame_size_);
            }
            lock.lock();
        }
    }

    ESP_LOGW(TAG, "Opus codec task stopped");
}

void AudioService::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    if (decoder_sample_rate_ == sample_rate && decoder_duration_ms_ == frame_duration) {
        return;
    }
    std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
        opus_decoder_ = nullptr;
    }
    decoder_lock.unlock();
    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(sample_rate, frame_duration);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio decoder, error code: %d", ret);
        return;
    }
    decoder_sample_rate_ = sample_rate;
    decoder_duration_ms_ = frame_duration;
    decoder_frame_size_ = decoder_sample_rate_ / 1000 * frame_duration;

    auto codec = Board::GetInstance().GetAudioCodec();
    if (decoder_sample_rate_ != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", decoder_sample_rate_,
                 codec->output_sample_rate());
        if (output_resampler_ != nullptr) {
            esp_ae_rate_cvt_close(output_resampler_);
            output_resampler_ = nullptr;
        }
        esp_ae_rate_cvt_cfg_t output_resampler_cfg =
            RATE_CVT_CFG(decoder_sample_rate_, codec->output_sample_rate(), ESP_AUDIO_MONO);
        auto resampler_ret = esp_ae_rate_cvt_open(&output_resampler_cfg, &output_resampler_);
        if (output_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create output resampler, error code: %d", resampler_ret);
        }
    }
}

void AudioService::PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm) {
    if (type == kAudioTaskTypeEncodeToSendQueue && !voice_upload_gate_.IsOpen()) {
        return;
    }
    auto task = std::make_unique<AudioTask>();
    task->type = type;
    task->pcm = std::move(pcm);

    uint32_t dropped_total = 0;
    {
        /* Push the task to the encode queue */
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);

        if (type == kAudioTaskTypeEncodeToSendQueue) {
            const uint32_t generation = voice_upload_gate_.CurrentGeneration();
            if (!voice_upload_gate_.Allows(generation)) {
                return;
            }
            task->voice_upload_generation = generation;
        }

        /* If the task is to send queue, we need to set the timestamp */
        if (type == kAudioTaskTypeEncodeToSendQueue && !timestamp_queue_.empty()) {
            if (timestamp_queue_.size() <= MAX_TIMESTAMPS_IN_QUEUE) {
                task->timestamp = timestamp_queue_.front();
            } else {
                ESP_LOGW(TAG, "Timestamp queue (%u) is full, dropping timestamp",
                         timestamp_queue_.size());
            }
            timestamp_queue_.pop_front();
        }

        /* Microphone audio is realtime, so drop the oldest frame instead of blocking.
         * Blocking here would stall the audio engine task (AFE fetch) and deadlock the
         * whole input pipeline when the send queue stops being drained (e.g. network
         * congestion or a failed UDP send). */
        if (audio_encode_queue_.size() >= MAX_ENCODE_TASKS_IN_QUEUE) {
            audio_encode_queue_.pop_front();
            dropped_total = ++debug_statistics_.encode_drop_count;
        }
        audio_encode_queue_.push_back(std::move(task));
        audio_queue_cv_.notify_all();
    }

    /* Log outside the lock (UART writes are slow and would starve the codec task),
     * at most once per second. */
    if (dropped_total > 0) {
        int64_t now = esp_timer_get_time();
        if (now - last_encode_drop_log_time_ >= 1000000) {
            last_encode_drop_log_time_ = now;
            ESP_LOGW(TAG, "Encode queue is full, dropping oldest frame (dropped %lu so far)",
                     (unsigned long)dropped_total);
        }
    }
}

bool AudioService::PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait) {
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
    const auto timer_owner = timer_output_owner_.load();
    if (timer_owner != 0 && packet->playback_id != timer_owner)
        return false;
#endif
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    const uint32_t generation = playback_generation_;
    if (audio_decode_queue_.size() >= MAX_DECODE_PACKETS_IN_QUEUE) {
        if (wait) {
            audio_queue_cv_.wait(lock, [this, generation]() {
                return service_stopped_.load() || generation != playback_generation_ ||
                       audio_decode_queue_.size() < MAX_DECODE_PACKETS_IN_QUEUE;
            });
        } else {
            return false;
        }
    }
    if (service_stopped_.load() || generation != playback_generation_) {
        return false;
    }
    playback_drained_notified_ = false;
    audio_decode_queue_.push_back(std::move(packet));
    audio_queue_cv_.notify_all();
    return true;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (!voice_upload_gate_.IsOpen()) {
        audio_send_queue_.clear();
        return nullptr;
    }
    if (audio_send_queue_.empty()) {
        return nullptr;
    }
    auto packet = std::move(audio_send_queue_.front());
    audio_send_queue_.pop_front();
    if (!voice_upload_gate_.Allows(packet->voice_upload_generation)) {
        audio_send_queue_.clear();
        return nullptr;
    }
    audio_queue_cv_.notify_all();
    return packet;
}

void AudioService::EncodeWakeWord() {
    if (audio_engine_) {
        audio_engine_->EncodeWakeWordData();
    }
}

const std::string& AudioService::GetLastWakeWord() const {
    static const std::string empty;
    return audio_engine_ ? audio_engine_->GetLastDetectedWakeWord() : empty;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopWakeWordPacket() {
    auto packet = std::make_unique<AudioStreamPacket>();
    if (audio_engine_ && audio_engine_->GetWakeWordOpus(packet->payload)) {
        return packet;
    }
    return nullptr;
}

void AudioService::EnableWakeWordDetection(bool enable) {
    ESP_LOGD(TAG, "%s wake word detection", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!InitializeAudioEngine()) {
            xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
            return;
        }
#if !(CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31)
        auto* lite_engine = static_cast<LiteAudioEngine*>(audio_engine_.get());
        if (!lite_engine->RestoreWakeWordResources()) {
            xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
            return;
        }
#endif
        if (!audio_engine_->HasWakeWord()) {
            xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        audio_engine_->EnableWakeWordDetection(true);
        xEventGroupSetBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    } else {
        if (audio_engine_initialized_) {
            audio_engine_->EnableWakeWordDetection(false);
        }
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    }
}

void AudioService::ReleaseWakeWordResources() {
#if !(CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31)
    if (!audio_engine_initialized_) {
        return;
    }
    if (xEventGroupGetBits(event_group_) &
        (AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING)) {
        ESP_LOGW(TAG, "Cannot release WakeNet while the audio engine is active");
        return;
    }
    static_cast<LiteAudioEngine*>(audio_engine_.get())->ReleaseWakeWordResources();
#endif
}

#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
void AudioService::FenceLocalRecording(uint32_t press) {
    local_physical_boundary_.store(press, std::memory_order_release);
    local_recording_press_.store(0, std::memory_order_release);
}
void AudioService::ReleaseLocalRecordingFence(uint32_t press) {
    auto expected = press;
    if (local_physical_boundary_.compare_exchange_strong(expected, press | 0x80000000U,
                                                         std::memory_order_acq_rel))
        local_recording_press_.store(0, std::memory_order_release);
}
void AudioService::ReconcileLocalRecording(uint32_t press) {
    const uint32_t released = press | 0x80000000U;
    if (local_physical_boundary_.load(std::memory_order_acquire) == released)
        local_output_boundary_.store(released, std::memory_order_release);
}
void AudioService::StartLocalRecording(uint32_t press) {
    std::lock_guard<std::mutex> lock(local_recording_mutex_);
    if (press == 0 || service_stopped_.load() ||
        local_physical_boundary_.load(std::memory_order_acquire) != press)
        return;
    std::lock_guard<std::mutex> queue_lock(audio_queue_mutex_);
    ++playback_generation_;
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
    local_feedback_ = {};
    local_feedback_active_ = false;
#endif
    local_prepared_press_.store(0, std::memory_order_release);
    local_recording_press_.store(press, std::memory_order_release);
    audio_queue_cv_.notify_all();
    xEventGroupSetBits(event_group_, AS_EVENT_LOCAL_RECORDING_RUNNING);
}
bool AudioService::IsLocalRecordingClosed(uint32_t press) const {
    return local_recording_press_.load(std::memory_order_acquire) != press &&
           local_input_press_.load(std::memory_order_acquire) != press;
}
bool AudioService::IsLocalInputIdle() const {
    return local_recording_press_.load(std::memory_order_acquire) == 0 &&
           local_input_press_.load(std::memory_order_acquire) == 0;
}
bool AudioService::IsLocalRecordingReady(uint32_t press) const {
    return press != 0 && local_recording_press_.load(std::memory_order_acquire) == press &&
           local_physical_boundary_.load(std::memory_order_acquire) == press &&
           local_prepared_press_.load(std::memory_order_acquire) == press;
}
void AudioService::StopLocalRecording(uint32_t expected_press) {
    std::lock_guard<std::mutex> lock(local_recording_mutex_);
    if (expected_press != 0 && local_recording_press_.load() != expected_press)
        return;
    local_recording_press_.store(0, std::memory_order_release);
    xEventGroupClearBits(event_group_, AS_EVENT_LOCAL_RECORDING_RUNNING);
}
#endif

void AudioService::EnableVoiceProcessing(bool enable) {
    ESP_LOGD(TAG, "%s voice processing", enable ? "Enabling" : "Disabling");

    if (enable) {
        if (!InitializeAudioEngine()) {
            return;
        }
        ResetDecoder();
        audio_input_need_warmup_ = true;
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        voice_upload_gate_.Open();
        audio_engine_->EnableVoiceProcessing(true);
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    } else {
        CloseVoiceUploadGate();
        if (audio_engine_initialized_) {
            audio_engine_->EnableVoiceProcessing(false);
        }
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    }
}

void AudioService::CloseVoiceUploadGate() {
    // Physical Talk release calls this before queuing work on the main task.
    // Invalidate in-flight encoders first, then drain queued PCM and Opus while
    // holding the mutex shared by the encoder and sender stages.
    voice_upload_gate_.Close();
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    for (auto it = audio_encode_queue_.begin(); it != audio_encode_queue_.end();) {
        if ((*it)->type == kAudioTaskTypeEncodeToSendQueue) {
            it = audio_encode_queue_.erase(it);
        } else {
            ++it;
        }
    }
    audio_send_queue_.clear();
    audio_queue_cv_.notify_all();
}

void AudioService::EnableAudioTesting(bool enable) {
    ESP_LOGI(TAG, "%s audio testing", enable ? "Enabling" : "Disabling");
    if (enable) {
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
        /* Copy audio_testing_queue_ to audio_decode_queue_ */
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_decode_queue_ = std::move(audio_testing_queue_);
        if (!audio_decode_queue_.empty()) {
            playback_drained_notified_ = false;
        }
        audio_queue_cv_.notify_all();
    }
}

void AudioService::EnableDeviceAec(bool enable) {
    ESP_LOGI(TAG, "%s device AEC", enable ? "Enabling" : "Disabling");
    device_aec_enabled_ = enable;

    if (audio_engine_initialized_) {
        audio_engine_->EnableDeviceAec(enable);
    } else {
        ESP_LOGI(TAG, "Deferring AEC change until the audio engine is initialized");
    }
}

void AudioService::SetCallbacks(AudioServiceCallbacks& callbacks) { callbacks_ = callbacks; }

#if CONFIG_PROVISIONS_LOCAL_CAPTURE
bool AudioService::PlayLocalFeedback(const std::string_view& sound) {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (sound.empty() || sound.size() > 32768 || service_stopped_.load() ||
        local_recording_press_.load() != 0 || timer_output_owner_.load() != 0)
        return false;
    ++playback_generation_;
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    local_feedback_ = sound;
    local_feedback_offset_ = 0;
    local_feedback_active_ = true;
    playback_drained_notified_ = false;
    audio_queue_cv_.notify_all();
    return true;
}

void AudioService::CancelLocalFeedback() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (!local_feedback_active_)
        return;
    ++playback_generation_;
    local_feedback_ = {};
    local_feedback_active_ = false;
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_queue_cv_.notify_all();
}

void AudioService::FillLocalFeedbackLocked() {
    if (local_feedback_.empty())
        return;
    if (local_feedback_offset_ == 0)
        local_feedback_demuxer_.Reset();
    std::unique_ptr<AudioStreamPacket> packet;
    local_feedback_demuxer_.OnPacket(
        [&packet](const uint8_t* data, int rate, int duration, size_t size) {
            packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = rate;
            packet->frame_duration = duration;
            packet->payload.assign(data, data + size);
        });
    // Parse only through the next packet, leaving compressed asset bytes in
    // flash. Playback backpressure never moves onto the caller or microphone.
    while (!packet && local_feedback_offset_ < local_feedback_.size() &&
           !local_feedback_demuxer_.HasError()) {
        const auto* byte =
            reinterpret_cast<const uint8_t*>(local_feedback_.data()) + local_feedback_offset_++;
        local_feedback_demuxer_.Process(byte, 1);
    }
    local_feedback_demuxer_.OnPacket({});
    if (local_feedback_offset_ == local_feedback_.size() || local_feedback_demuxer_.HasError())
        local_feedback_ = {};
    if (packet)
        audio_decode_queue_.push_back(std::move(packet));
}
#endif

void AudioService::PlaySound(const std::string_view& ogg) {
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
    if (timer_output_owner_.load() != 0)
        return;
#endif
    if (!codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableOutput(true);
    }

    const auto* buf = reinterpret_cast<const uint8_t*>(ogg.data());
    size_t size = ogg.size();

    auto demuxer = std::make_unique<OggDemuxer>();
    demuxer->OnPacket(
        [this](const uint8_t* data, int sample_rate, int frame_duration_ms, size_t size) {
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = sample_rate;
            packet->frame_duration = frame_duration_ms;
            packet->payload.resize(size);
            std::memcpy(packet->payload.data(), data, size);
            PushPacketToDecodeQueue(std::move(packet), true);
        });
    demuxer->Reset();
    demuxer->Process(buf, size);
}

bool AudioService::IsIdle() {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    if (local_recording_press_.load() != 0)
        return false;
#endif
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return audio_encode_queue_.empty() && IsPlaybackDrainedLocked() && audio_testing_queue_.empty();
}

bool AudioService::IsPlaybackIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return IsPlaybackDrainedLocked();
}

void AudioService::ResetDecoder() {
    bool notify_drained = false;
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        ++playback_generation_;
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
        local_feedback_ = {};
        local_feedback_active_ = false;
#endif
        std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
        if (opus_decoder_ != nullptr) {
            esp_opus_dec_reset(opus_decoder_);
        }
        decoder_lock.unlock();
        timestamp_queue_.clear();
        audio_decode_queue_.clear();
        audio_playback_queue_.clear();
        audio_testing_queue_.clear();
        notify_drained = MarkPlaybackDrainedLocked();
        audio_queue_cv_.notify_all();
    }
    if (notify_drained && callbacks_.on_playback_drained) {
        callbacks_.on_playback_drained();
    }
}

bool AudioService::IsPlaybackDrainedLocked() const {
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
    if (!local_feedback_.empty())
        return false;
#endif
    return audio_decode_queue_.empty() && audio_playback_queue_.empty() && !decode_in_flight_ &&
           !output_in_flight_ && codec_->IsOutputDrained();
}

bool AudioService::MarkPlaybackDrainedLocked() {
    if (!IsPlaybackDrainedLocked() || playback_drained_notified_) {
        return false;
    }
    playback_drained_notified_ = true;
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
    local_feedback_active_ = false;
#endif
    return true;
}

void AudioService::CheckAndUpdateAudioPowerState() {
    auto now = std::chrono::steady_clock::now();
    auto input_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_input_time_).count();
    auto output_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_output_time_).count();
    if (input_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->input_enabled()) {
        // ADC continuous start/stop must run in the same task. Wake the audio
        // input task instead of closing the codec from the esp_timer task.
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_INPUT_STOP_REQUEST);
    }
    if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled()) {
        // Keep TX clock when duplex RX is active; otherwise RX may stall on some boards.
        if (!(codec_->duplex() && codec_->input_enabled())) {
            codec_->EnableOutput(false);
        }
    }
    if (!codec_->input_enabled() && !codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
    }
}

void AudioService::SetModelsList(srmodel_list_t* models_list) {
    if (audio_engine_initialized_ && models_list_ != models_list) {
        ESP_LOGW(TAG, "Ignoring speech model replacement after audio engine initialization");
        return;
    }
    models_list_ = models_list;
}

bool AudioService::IsAfeWakeWord() {
    return audio_engine_initialized_ && audio_engine_->IsAfeWakeWord();
}

bool AudioService::InitializeAudioEngine() {
    if (!audio_engine_) {
        return false;
    }
    if (audio_engine_initialized_) {
        return true;
    }
    if (!audio_engine_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_)) {
        ESP_LOGE(TAG, "Failed to initialize audio engine");
        return false;
    }
    audio_engine_initialized_ = true;
    audio_engine_->EnableDeviceAec(device_aec_enabled_);
    return true;
}
