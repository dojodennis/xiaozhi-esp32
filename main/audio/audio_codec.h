#ifndef _AUDIO_CODEC_H
#define _AUDIO_CODEC_H

#include <driver/i2s_std.h>
#include <esp_idf_version.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <functional>
#include <string>
#include <vector>

#include "board.h"
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
#include <freertos/task.h>
#include <atomic>
#include "provisions_audio_admission.h"

// Synchronous work only: its stack lifetime includes the actual operation and callbacks.
class AudioAdmissionWork final {
public:
    using Producer = provisions::audio_admission::Producer;
    explicit AudioAdmissionWork(provisions::audio_admission::Gate* gate, Producer producer,
                                const provisions::audio_admission::Reservation* parent = nullptr)
        : gate_(gate),
          admitted_(gate && (parent ? gate->ReserveMedia(*parent, producer, token_)
                                    : gate->Reserve(producer, token_))) {}
    AudioAdmissionWork(provisions::audio_admission::Gate& gate, Producer producer,
                       const provisions::audio_admission::Reservation& parent)
        : gate_(&gate), admitted_(gate.ReserveMedia(parent, producer, token_)) {}
    AudioAdmissionWork(provisions::audio_admission::Gate& gate,
                       const provisions::audio_admission::TimerIdentity& timer)
        : gate_(&gate), admitted_(gate.ReserveTimerRecovery(timer, token_)) {}
    ~AudioAdmissionWork() { Complete(); }
    void Complete() {
        if (admitted_) {
            gate_->Complete(token_);
            admitted_ = false;
        }
    }
    bool Allowed() const { return admitted_ && gate_->AllowsPublication(token_); }
    bool Admitted() const { return admitted_; }
    provisions::audio_admission::Reservation& Token() { return token_; }

private:
    provisions::audio_admission::Gate* gate_;
    provisions::audio_admission::Reservation token_;
    bool admitted_;
};
#endif

#define AUDIO_CODEC_DMA_DESC_NUM 6
#define AUDIO_CODEC_DMA_FRAME_NUM 240

// ESP-IDF 6 removed i2s_port_t and changed i2s_chan_config_t::id to an integer.
// Keep numeric I2S controller IDs usable on targets where IDF 5 does not expose
// every value through the target-specific i2s_port_t enum (for example ESP32-C3).
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#define XIAOZHI_I2S_PORT(port) (port)
#else
#define XIAOZHI_I2S_PORT(port) static_cast<i2s_port_t>(port)
#endif

class AudioCodec {
public:
    AudioCodec();
    virtual ~AudioCodec();

    virtual void SetOutputVolume(int volume);
    virtual void SetInputGain(float gain);
    virtual void EnableInput(bool enable);
    virtual void EnableOutput(bool enable);

    virtual bool OutputData(std::vector<int16_t>& data);
    virtual bool InputData(std::vector<int16_t>& data);
    // Called only by the input task before a new local recording. Unsupported
    // codecs fail closed rather than relabeling buffered audio as a new press.
    virtual bool PrepareInputCapture() { return false; }
    virtual bool IsOutputDrained() const { return true; }
    virtual void Start();
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
    void BindAudioAdmission(provisions::audio_admission::Gate& gate) { admission_.store(&gate); }
    bool OutputDataAdmitted(std::vector<int16_t>& data,
                            const provisions::audio_admission::Reservation& token);
    bool InputDataAdmitted(std::vector<int16_t>& data,
                           const provisions::audio_admission::Reservation& token);
    bool PrepareInputCaptureAdmitted(const provisions::audio_admission::Reservation& token);
    virtual bool SupportsOutputFence() const { return false; }
    // Called only by the respective input/output task, never a timer callback.
    virtual bool CloseInputForFence() { return false; }
    virtual bool CloseOutputForFence() { return false; }
    virtual bool IsInputClosedForFence() const { return false; }
    virtual bool IsOutputClosedForFence() const { return false; }
    virtual bool EnableOutputAdmitted(const provisions::audio_admission::Reservation&) {
        return false;
    }
#endif

    inline bool duplex() const { return duplex_; }
    inline bool input_reference() const { return input_reference_; }
    inline int input_sample_rate() const { return input_sample_rate_; }
    inline int output_sample_rate() const { return output_sample_rate_; }
    inline int input_channels() const { return input_channels_; }
    inline int output_channels() const { return output_channels_; }
    inline int output_volume() const { return output_volume_; }
    inline float input_gain() const { return input_gain_; }
    inline bool input_enabled() const { return input_enabled_; }
    inline bool output_enabled() const { return output_enabled_; }

protected:
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
    std::atomic<provisions::audio_admission::Gate*> admission_{nullptr};
    bool OutputContextAllows() const;
    bool InputContextAllows(provisions::audio_admission::Producer expected) const;
    // One actual input task invocation. try_lock denies reentrancy/concurrent borrowing.
    std::mutex input_context_mutex_, output_context_mutex_;
    std::atomic<TaskHandle_t> output_context_task_{nullptr};
    std::atomic<const provisions::audio_admission::Reservation*> output_context_token_{nullptr};
    std::atomic<TaskHandle_t> input_context_task_{nullptr};
    std::atomic<const provisions::audio_admission::Reservation*> input_context_token_{nullptr};
#endif
    i2s_chan_handle_t tx_handle_ = nullptr;
    i2s_chan_handle_t rx_handle_ = nullptr;

    bool duplex_ = false;
    bool input_reference_ = false;
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
    std::atomic<bool> input_enabled_{false}, output_enabled_{false};
#else
    bool input_enabled_ = false;
    bool output_enabled_ = false;
#endif
    int input_sample_rate_ = 0;
    int output_sample_rate_ = 0;
    int input_channels_ = 1;
    int output_channels_ = 1;
    int output_volume_ = 70;
    float input_gain_ = 0.0;

    virtual int Read(int16_t* dest, int samples) = 0;
    virtual int Write(const int16_t* data, int samples) = 0;
};

#endif  // _AUDIO_CODEC_H
