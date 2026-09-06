#ifndef _ES8311_AUDIO_CODEC_H
#define _ES8311_AUDIO_CODEC_H

#include "audio_codec.h"

#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <mutex>


class Es8311AudioCodec : public AudioCodec {
private:
    const audio_codec_data_if_t* data_if_ = nullptr;
    const audio_codec_ctrl_if_t* ctrl_if_ = nullptr;
    const audio_codec_if_t* codec_if_ = nullptr;
    const audio_codec_gpio_if_t* gpio_if_ = nullptr;

    esp_codec_dev_handle_t dev_ = nullptr;
    gpio_num_t pa_pin_ = GPIO_NUM_NC;
    bool pa_inverted_ = false;
    std::mutex data_if_mutex_;
    mutable portMUX_TYPE output_dma_mutex_ = portMUX_INITIALIZER_UNLOCKED;
    bool output_write_active_ = false;
    uint32_t output_dma_remaining_ = 0;
    static bool OnOutputSent(i2s_chan_handle_t handle, i2s_event_data_t* event, void* context);

    void CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din);
    void ResetCodec();
    void UpdateDeviceState();

    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

protected:
    // Applies a runtime-only pilot level without updating the audio NVS key.
    void SetOutputVolumeForSession(int volume);

public:
    Es8311AudioCodec(void* i2c_master_handle, i2c_port_t i2c_port, int input_sample_rate, int output_sample_rate,
        gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
        gpio_num_t pa_pin, uint8_t es8311_addr, bool use_mclk = true, bool pa_inverted = false);
    virtual ~Es8311AudioCodec();

    virtual void SetOutputVolume(int volume) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;
    bool PrepareInputCapture() override;
    bool IsOutputDrained() const override;
};

#endif // _ES8311_AUDIO_CODEC_H
