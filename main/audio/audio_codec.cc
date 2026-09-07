#include "audio_codec.h"
#include "board.h"
#include "settings.h"

#include <driver/i2s_common.h>
#include <esp_log.h>
#include <cstring>

#define TAG "AudioCodec"

AudioCodec::AudioCodec() {}

AudioCodec::~AudioCodec() {}

bool AudioCodec::OutputData(std::vector<int16_t>& data) {
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
    if (!OutputContextAllows())
        return false;
#endif
    return !data.empty() && Write(data.data(), data.size()) == static_cast<int>(data.size());
}

bool AudioCodec::InputData(std::vector<int16_t>& data) {
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
    if (!InputContextAllows(AudioAdmissionWork::Producer::InputRead))
        return false;
#endif
    int samples = Read(data.data(), data.size());
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
    if (!InputContextAllows(AudioAdmissionWork::Producer::InputRead))
        return false;
#endif
    if (!data.empty() && samples == static_cast<int>(data.size())) {
        return true;
    }
    return false;
}

void AudioCodec::Start() {
    Settings settings("audio", false);
    output_volume_ = settings.GetInt("output_volume", output_volume_);
    if (output_volume_ <= 0) {
        ESP_LOGW(TAG, "Output volume value (%d) is too small, setting to default (10)",
                 output_volume_);
        output_volume_ = 10;
    }

    ESP_LOGI(TAG, "Audio codec started");
}

void AudioCodec::SetOutputVolume(int volume) {
    output_volume_ = volume;
    ESP_LOGI(TAG, "Set output volume to %d", output_volume_);

    Settings settings("audio", true);
    settings.SetInt("output_volume", output_volume_);
}

void AudioCodec::SetInputGain(float gain) {
    input_gain_ = gain;
    ESP_LOGI(TAG, "Set input gain to %.1f", input_gain_);
}

void AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    input_enabled_ = enable;
    ESP_LOGI(TAG, "Set input enable to %s", enable ? "true" : "false");
}

void AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    output_enabled_ = enable;
    ESP_LOGI(TAG, "Set output enable to %s", enable ? "true" : "false");
}

#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
bool AudioCodec::InputContextAllows(provisions::audio_admission::Producer expected) const {
    auto* gate = admission_.load();
    auto* token = input_context_token_.load();
    return gate && token && input_context_task_.load() != nullptr &&
           input_context_task_.load() == xTaskGetCurrentTaskHandle() &&
           gate->AllowsCaptureInput(*token, expected);
}
bool AudioCodec::InputDataAdmitted(std::vector<int16_t>& data,
                                   const provisions::audio_admission::Reservation& token) {
    const auto task = xTaskGetCurrentTaskHandle();
    if (task && input_context_task_.load() == task)
        return false;
    std::unique_lock<std::mutex> lock(input_context_mutex_, std::try_to_lock);
    auto* gate = admission_.load();
    if (!lock.owns_lock() || !gate || !task ||
        !gate->AllowsCaptureInput(token, provisions::audio_admission::Producer::InputRead))
        return false;
    input_context_task_.store(task);
    input_context_token_.store(&token);
    struct ClearContext {
        AudioCodec* codec;
        ~ClearContext() {
            codec->input_context_token_.store(nullptr);
            codec->input_context_task_.store(nullptr);
        }
    } clear{this};
    // Preserve the board's virtual meter callback inside the retained operation.
    const bool result = InputData(data);
    return result &&
           gate->AllowsCaptureInput(token, provisions::audio_admission::Producer::InputRead);
}
bool AudioCodec::PrepareInputCaptureAdmitted(
    const provisions::audio_admission::Reservation& token) {
    const auto task = xTaskGetCurrentTaskHandle();
    if (task && input_context_task_.load() == task)
        return false;
    std::unique_lock<std::mutex> lock(input_context_mutex_, std::try_to_lock);
    auto* gate = admission_.load();
    if (!lock.owns_lock() || !gate || !task ||
        !gate->AllowsCaptureInput(token, provisions::audio_admission::Producer::InputPreparation))
        return false;
    input_context_task_.store(task);
    input_context_token_.store(&token);
    struct ClearContext {
        AudioCodec* codec;
        ~ClearContext() {
            codec->input_context_token_.store(nullptr);
            codec->input_context_task_.store(nullptr);
        }
    } clear{this};
    const bool result = PrepareInputCapture();
    return result &&
           gate->AllowsCaptureInput(token, provisions::audio_admission::Producer::InputPreparation);
}
bool AudioCodec::OutputContextAllows() const {
    auto* gate = admission_.load();
    auto* token = output_context_token_.load();
    return gate && token && output_context_task_.load() != nullptr &&
           output_context_task_.load() == xTaskGetCurrentTaskHandle() &&
           gate->AllowsPublication(*token, provisions::audio_admission::Producer::Output);
}
bool AudioCodec::OutputDataAdmitted(std::vector<int16_t>& data,
                                    const provisions::audio_admission::Reservation& token) {
    const auto task = xTaskGetCurrentTaskHandle();
    if (task && output_context_task_.load() == task)
        return false;
    std::unique_lock<std::mutex> lock(output_context_mutex_, std::try_to_lock);
    auto* gate = admission_.load();
    if (!lock.owns_lock() || !gate || !task ||
        !gate->AllowsPublication(token, provisions::audio_admission::Producer::Output))
        return false;
    output_context_task_.store(task);
    output_context_token_.store(&token);
    struct ClearContext {
        AudioCodec* codec;
        ~ClearContext() {
            codec->output_context_token_.store(nullptr);
            codec->output_context_task_.store(nullptr);
        }
    } clear{this};
    const bool result = OutputData(data);
    return result && gate->AllowsPublication(token, provisions::audio_admission::Producer::Output);
}
#endif
