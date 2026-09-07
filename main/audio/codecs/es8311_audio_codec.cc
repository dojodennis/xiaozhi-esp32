#include "es8311_audio_codec.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "Es8311AudioCodec"

Es8311AudioCodec::Es8311AudioCodec(void* i2c_master_handle, i2c_port_t i2c_port,
                                   int input_sample_rate, int output_sample_rate, gpio_num_t mclk,
                                   gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
                                   gpio_num_t pa_pin, uint8_t es8311_addr, bool use_mclk,
                                   bool pa_inverted) {
    duplex_ = true;            // 是否双工
    input_reference_ = false;  // 是否使用参考输入，实现回声消除
    input_channels_ = 1;       // 输入通道数
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    pa_pin_ = pa_pin;
    pa_inverted_ = pa_inverted;
    input_gain_ = 30;

    assert(input_sample_rate_ == output_sample_rate_);
    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    // Do initialize of related interface: data_if, ctrl_if and gpio_if
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != NULL);

    // Output
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = i2c_port,
        .addr = es8311_addr,
        .bus_handle = i2c_master_handle,
    };
    ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(ctrl_if_ != NULL);
    ResetCodec();

    gpio_if_ = audio_codec_new_gpio();
    assert(gpio_if_ != NULL);

    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = ctrl_if_;
    es8311_cfg.gpio_if = gpio_if_;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    es8311_cfg.pa_pin = pa_pin;
    es8311_cfg.use_mclk = use_mclk;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    es8311_cfg.pa_reverted = pa_inverted_;
    codec_if_ = es8311_codec_new(&es8311_cfg);

    if (codec_if_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create Es8311AudioCodec");
    } else {
        ESP_LOGI(TAG, "Es8311AudioCodec initialized");
    }
}

void Es8311AudioCodec::ResetCodec() {
    // Hold the ES8311 digital blocks in reset for several milliseconds, as
    // recommended by the initialization guide. Normal codec initialization
    // releases the reset and starts the state machine.
    uint8_t reset_value = 0x1F;
    ESP_ERROR_CHECK(
        static_cast<esp_err_t>(ctrl_if_->write_reg(ctrl_if_, 0x00, 1, &reset_value, 1)));
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_LOGI(TAG, "ES8311 software reset complete");
}

Es8311AudioCodec::~Es8311AudioCodec() {
    esp_codec_dev_delete(dev_);

    audio_codec_delete_codec_if(codec_if_);
    audio_codec_delete_ctrl_if(ctrl_if_);
    audio_codec_delete_gpio_if(gpio_if_);
    audio_codec_delete_data_if(data_if_);
}

void Es8311AudioCodec::UpdateDeviceState() {
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
    // The matched admission model never allows simultaneous mic and speaker owners.
    if (input_enabled_ && output_enabled_) {
        input_enabled_ = false;
        output_enabled_ = false;
        fence_rx_closed_.store(false);
        fence_tx_closed_.store(false);
    }
#endif
    if ((input_enabled_ || output_enabled_) && dev_ == nullptr) {
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
        // Vendor open/set_fmt can enable the selected channel and TX clocks for RX.
        // Keep conservative facts until the respective owning task stops hardware.
        if (input_enabled_)
            fence_rx_closed_.store(false);
        fence_tx_closed_.store(false);
#endif
        esp_codec_dev_cfg_t dev_cfg = {
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
            .dev_type = input_enabled_ ? ESP_CODEC_DEV_TYPE_IN : ESP_CODEC_DEV_TYPE_OUT,
#else
            .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
#endif
            .codec_if = codec_if_,
            .data_if = data_if_,
        };
        dev_ = esp_codec_dev_new(&dev_cfg);
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 1,
            .channel_mask = 0,
            .sample_rate = (uint32_t)input_sample_rate_,
            .mclk_multiple = 0,
        };
        const char* stage = "allocate";
        int result = dev_ ? ESP_CODEC_DEV_OK : ESP_ERR_NO_MEM;
        if (result == ESP_CODEC_DEV_OK) {
            stage = "open";
            result = esp_codec_dev_open(dev_, &fs);
        }
        if (result == ESP_CODEC_DEV_OK
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
            && input_enabled_
#endif
        ) {
            stage = "input gain";
            result = esp_codec_dev_set_in_gain(dev_, input_gain_);
        }
        if (result == ESP_CODEC_DEV_OK
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
            && output_enabled_
#endif
        ) {
            stage = "output volume";
            result = esp_codec_dev_set_out_vol(dev_, output_volume_);
        }
        if (result != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "Codec activation failed at %s: %d", stage, result);
            // A runtime acquisition failure must reach the recording error
            // callback, not abort the device. Retry starts with a fresh handle.
            if (dev_) {
                esp_codec_dev_close(dev_);
                esp_codec_dev_delete(dev_);
                dev_ = nullptr;
            }
            input_enabled_ = false;
            output_enabled_ = false;
        }
    } else if (!input_enabled_ && !output_enabled_ && dev_ != nullptr) {
        const int result = esp_codec_dev_close(dev_);
        if (result != ESP_CODEC_DEV_OK)
            ESP_LOGE(TAG, "Codec close failed: %d", result);
        esp_codec_dev_delete(dev_);
        dev_ = nullptr;
    }
    if (pa_pin_ != GPIO_NUM_NC) {
        int level = output_enabled_ ? 1 : 0;
        gpio_set_level(pa_pin_, pa_inverted_ ? !level : level);
    }
}

void Es8311AudioCodec::CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
                                            gpio_num_t dout, gpio_num_t din) {
    assert(input_sample_rate_ == output_sample_rate_);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg =
            {
                .sample_rate_hz = (uint32_t)output_sample_rate_,
                .clk_src = I2S_CLK_SRC_DEFAULT,
                .mclk_multiple = I2S_MCLK_MULTIPLE_256,
#ifdef I2S_HW_VERSION_2
                .ext_clk_freq_hz = 0,
#endif
            },
        .slot_cfg = {.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
                     .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                     .slot_mode = I2S_SLOT_MODE_STEREO,
                     .slot_mask = I2S_STD_SLOT_BOTH,
                     .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
                     .ws_pol = false,
                     .bit_shift = true,
#ifdef I2S_HW_VERSION_2
                     .left_align = true,
                     .big_endian = false,
                     .bit_order_lsb = false
#endif
        },
        .gpio_cfg = {.mclk = mclk,
                     .bclk = bclk,
                     .ws = ws,
                     .dout = dout,
                     .din = din,
                     .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false}}};

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    i2s_event_callbacks_t callbacks = {};
    callbacks.on_sent = OnOutputSent;
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(tx_handle_, &callbacks, this));
#if !CONFIG_PROVISIONS_OUTPUT_FENCE_V1
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
#endif
    ESP_LOGI(TAG, "Duplex channels created");
}

void Es8311AudioCodec::SetOutputVolume(int volume) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (dev_ != nullptr) {
        const int result = esp_codec_dev_set_out_vol(dev_, volume);
        if (result != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "Codec volume failed: %d", result);
            return;
        }
    }
    AudioCodec::SetOutputVolume(volume);
}

void Es8311AudioCodec::SetOutputVolumeForSession(int volume) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    output_volume_ = volume;
    ESP_LOGI(TAG, "Set session output volume to %d", output_volume_);
    if (dev_ != nullptr) {
        const int result = esp_codec_dev_set_out_vol(dev_, output_volume_);
        if (result != ESP_CODEC_DEV_OK)
            ESP_LOGE(TAG, "Codec session volume failed: %d", result);
    }
}

void Es8311AudioCodec::EnableInput(bool enable) {
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
    if (enable && !InputContextAllows(AudioAdmissionWork::Producer::InputPreparation))
        return;
#endif
    std::lock_guard<std::mutex> lock(data_if_mutex_);
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
    if (enable && !InputContextAllows(AudioAdmissionWork::Producer::InputPreparation))
        return;
    if (enable) {
        // Duplex RX requires TX clocks. Neither channel was enabled at construction.
        const auto tx = i2s_channel_enable(tx_handle_);
        if (tx != ESP_OK && tx != ESP_ERR_INVALID_STATE)
            return;
        fence_tx_closed_.store(false);
        const auto rx = i2s_channel_enable(rx_handle_);
        if (rx != ESP_OK && rx != ESP_ERR_INVALID_STATE)
            return;
        fence_rx_closed_.store(false);
    }
#endif
    if (codec_if_ == nullptr) {
        return;
    }
    if (enable == input_enabled_) {
        return;
    }
    AudioCodec::EnableInput(enable);
    UpdateDeviceState();
}

void Es8311AudioCodec::EnableOutput(bool enable) {
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
    // Activation must carry the caller's exact retained ordinary/timer reservation.
    if (enable)
        return;
#endif
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (codec_if_ == nullptr) {
        return;
    }
    if (enable == output_enabled_) {
        return;
    }
    AudioCodec::EnableOutput(enable);
    UpdateDeviceState();
}

int Es8311AudioCodec::Read(int16_t* dest, int samples) {
    if (!input_enabled_ || samples <= 0 ||
        esp_codec_dev_read(dev_, dest, samples * sizeof(int16_t)) != ESP_CODEC_DEV_OK)
        return 0;
    return samples;
}

int Es8311AudioCodec::Write(const int16_t* data, int samples) {
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
    if (!OutputContextAllows())
        return 0;
#endif
    if (!output_enabled_ || samples <= 0)
        return 0;
    portENTER_CRITICAL(&output_dma_mutex_);
    output_write_active_ = true;
    portEXIT_CRITICAL(&output_dma_mutex_);
    const int result = esp_codec_dev_write(dev_, (void*)data, samples * sizeof(int16_t));
    // The write returns after copying into DMA. One complete descriptor ring
    // after that copy also covers any partial write before a driver error.
    portENTER_CRITICAL(&output_dma_mutex_);
    output_dma_remaining_ = AUDIO_CODEC_DMA_DESC_NUM;
    output_write_active_ = false;
    portEXIT_CRITICAL(&output_dma_mutex_);
    return result == ESP_CODEC_DEV_OK ? samples : 0;
}

bool Es8311AudioCodec::OnOutputSent(i2s_chan_handle_t, i2s_event_data_t*, void* context) {
    auto* codec = static_cast<Es8311AudioCodec*>(context);
    portENTER_CRITICAL_ISR(&codec->output_dma_mutex_);
    if (codec->output_dma_remaining_ != 0)
        --codec->output_dma_remaining_;
    portEXIT_CRITICAL_ISR(&codec->output_dma_mutex_);
    return false;
}

bool Es8311AudioCodec::IsOutputDrained() const {
    portENTER_CRITICAL(&output_dma_mutex_);
    const bool drained = !output_write_active_ && output_dma_remaining_ == 0;
    portEXIT_CRITICAL(&output_dma_mutex_);
    return drained;
}

bool Es8311AudioCodec::PrepareInputCapture() {
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
    if (!InputContextAllows(AudioAdmissionWork::Producer::InputPreparation))
        return false;
    // Close the previous output-only vendor handle before creating an input-only
    // handle. Its IN_OUT open path would implicitly start the unrelated RX channel.
    if (!input_enabled_ && !CloseOutputForFence())
        return false;
#endif
    EnableInput(true);
    std::lock_guard<std::mutex> lock(data_if_mutex_);
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
    if (!InputContextAllows(AudioAdmissionWork::Producer::InputPreparation))
        return false;
#endif
    if (!input_enabled_ || !dev_ || !rx_handle_)
        return false;
    // This task owns RX reads, so disabling cannot wait on another read. TX
    // remains enabled; restarting RX resets its DMA queue and current pointer.
    const auto stopped = i2s_channel_disable(rx_handle_);
    return (stopped == ESP_OK || stopped == ESP_ERR_INVALID_STATE) &&
           i2s_channel_enable(rx_handle_) == ESP_OK;
}

#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
bool Es8311AudioCodec::EnableOutputAdmitted(const provisions::audio_admission::Reservation& token) {
    const auto* gate = admission_.load();
    if (!gate || !gate->AllowsPublication(token, provisions::audio_admission::Producer::Output))
        return false;
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (!codec_if_ ||
        !gate->AllowsPublication(token, provisions::audio_admission::Producer::Output))
        return false;
    const auto tx = i2s_channel_enable(tx_handle_);
    if (tx != ESP_OK && tx != ESP_ERR_INVALID_STATE)
        return false;
    fence_tx_closed_.store(false);
    AudioCodec::EnableOutput(true);
    UpdateDeviceState();
    return output_enabled_ &&
           gate->AllowsPublication(token, provisions::audio_admission::Producer::Output);
}
bool Es8311AudioCodec::CloseInputForFence() {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    const auto result = i2s_channel_disable(rx_handle_);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
        return false;
    fence_rx_closed_.store(true);
    AudioCodec::EnableInput(false);
    UpdateDeviceState();
    return true;
}
bool Es8311AudioCodec::CloseOutputForFence() {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (!IsOutputDrained())
        return false;
    const auto result = i2s_channel_disable(tx_handle_);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
        return false;
    fence_tx_closed_.store(true);
    AudioCodec::EnableOutput(false);
    UpdateDeviceState();
    return true;
}
#endif
