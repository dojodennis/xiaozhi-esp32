#include "application.h"
#include "axp2101.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "kitchen_helper_display.h"
#include "power_save_timer.h"
#include "wifi_board.h"
#include "../core-s3/cores3_audio_codec.h"

#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_ili9341.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>

#define TAG "ProvisionsCoreS3"

class Pmic : public Axp2101 {
public:
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        uint8_t data = ReadReg(0x90);
        data |= 0b10110100;
        WriteReg(0x90, data);
        WriteReg(0x99, (0b11110 - 5));
        WriteReg(0x97, (0b11110 - 2));
        WriteReg(0x69, 0b00110101);
        WriteReg(0x30, 0b111111);
        WriteReg(0x90, 0xBF);
        WriteReg(0x94, 33 - 5);
        WriteReg(0x95, 33 - 5);
    }

    void SetBrightness(uint8_t brightness) {
        brightness = ((brightness + 641) >> 5);
        WriteReg(0x99, brightness);
    }
};

class CustomBacklight : public Backlight {
public:
    explicit CustomBacklight(Pmic* pmic) : pmic_(pmic) {}

    void SetBrightnessImpl(uint8_t brightness) override {
        (void)brightness;
        pmic_->SetBrightness(target_brightness_);
        brightness_ = target_brightness_;
    }

private:
    Pmic* pmic_;
};

class Aw9523 : public I2cDevice {
public:
    Aw9523(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        WriteReg(0x02, 0b00000111);
        WriteReg(0x03, 0b10001111);
        WriteReg(0x04, 0b00011000);
        WriteReg(0x05, 0b00001100);
        WriteReg(0x11, 0b00010000);
        WriteReg(0x12, 0b11111111);
        WriteReg(0x13, 0b11111111);
    }

    void ResetIli9342() {
        ESP_LOGI(TAG, "Reset ILI9342");
        WriteReg(0x03, 0b10000001);
        vTaskDelay(pdMS_TO_TICKS(20));
        WriteReg(0x03, 0b10000011);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
};

class ProvisionsCoreS3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Pmic* pmic_ = nullptr;
    Aw9523* aw9523_ = nullptr;
    KitchenHelperDisplay* display_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;
    Button talk_button_;

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(10);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() { pmic_->PowerOff(); });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_config = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {.enable_internal_pullup = 1},
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &i2c_bus_));
    }

    void InitializePower() {
        ESP_LOGI(TAG, "Initialize AXP2101 and AW9523");
        pmic_ = new Pmic(i2c_bus_, 0x34);
        aw9523_ = new Aw9523(i2c_bus_, 0x58);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    void InitializeSpi() {
        spi_bus_config_t bus_config = {};
        bus_config.mosi_io_num = GPIO_NUM_37;
        bus_config.miso_io_num = GPIO_NUM_NC;
        bus_config.sclk_io_num = GPIO_NUM_36;
        bus_config.quadwp_io_num = GPIO_NUM_NC;
        bus_config.quadhd_io_num = GPIO_NUM_NC;
        bus_config.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus_config, SPI_DMA_CH_AUTO));
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_3;
        io_config.dc_gpio_num = GPIO_NUM_35;
        io_config.spi_mode = 2;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        aw9523_->ResetIli9342();
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));

        display_ = new KitchenHelperDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                            DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                            DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeTalkButton() {
        talk_button_.OnPressDown([]() { Application::GetInstance().StartListening(); });
        talk_button_.OnPressUp([]() { Application::GetInstance().StopListening(); });
    }

public:
    ProvisionsCoreS3Board() : talk_button_(TALK_BUTTON_GPIO, false) {
        InitializePowerSaveTimer();
        InitializeI2c();
        InitializePower();
        InitializeSpi();
        InitializeDisplay();
        InitializeTalkButton();
        GetBacklight()->RestoreBrightness();
    }

    AudioCodec* GetAudioCodec() override {
        static CoreS3AudioCodec audio_codec(
            i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_AW88298_ADDR, AUDIO_CODEC_ES7210_ADDR, AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    Display* GetDisplay() override { return display_; }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = pmic_->IsCharging();
        discharging = pmic_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }

        level = pmic_->GetBatteryLevel();
        return true;
    }

    void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }

    Backlight* GetBacklight() override {
        static CustomBacklight backlight(pmic_);
        return &backlight;
    }
};

DECLARE_BOARD(ProvisionsCoreS3Board);
