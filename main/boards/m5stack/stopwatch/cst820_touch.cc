#include "cst820_touch.h"

#include <esp_log.h>

namespace {

constexpr char kTag[] = "StopwatchTouch";
constexpr uint8_t kStatusRegister = 0x00;
constexpr uint8_t kChipIdRegister = 0xA7;
constexpr uint8_t kSoftwareVersionRegister = 0xA9;
constexpr int kI2cTimeoutMs = 50;

}  // namespace

StopwatchCst820Touch::~StopwatchCst820Touch() {
    if (device_ != nullptr) {
        i2c_master_bus_rm_device(device_);
    }
}

bool StopwatchCst820Touch::Begin(i2c_master_bus_handle_t bus, uint8_t address) {
    if (bus == nullptr || device_ != nullptr) {
        return false;
    }

    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 100 * 1000,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = 0,
        },
    };
    if (i2c_master_bus_add_device(bus, &config, &device_) != ESP_OK) {
        device_ = nullptr;
        return false;
    }

    uint8_t chip_id = 0;
    uint8_t software_version = 0;
    if (!ReadRegister(kChipIdRegister, &chip_id, 1) ||
        !ReadRegister(kSoftwareVersionRegister, &software_version, 1) || chip_id == 0 ||
        software_version == 0) {
        i2c_master_bus_rm_device(device_);
        device_ = nullptr;
        return false;
    }

    ESP_LOGI(kTag, "CST820 ready chip=0x%02x software=0x%02x", chip_id, software_version);
    return true;
}

bool StopwatchCst820Touch::ReadPressed(bool& pressed) {
    if (device_ == nullptr) {
        return false;
    }

    // The M5Stack reference driver reads status, gesture, finger count and
    // coordinates as one seven-byte frame starting at register 0x00.
    uint8_t frame[7] = {};
    if (!ReadRegister(kStatusRegister, frame, sizeof(frame))) {
        return false;
    }

    const uint8_t finger_count = frame[2];
    const uint8_t event = (frame[3] & 0xC0) >> 6;
    pressed = finger_count > 0 && (event == 0 || event == 2);
    return true;
}

bool StopwatchCst820Touch::ReadRegister(uint8_t reg, uint8_t* data, size_t size) {
    return i2c_master_transmit_receive(device_, &reg, 1, data, size, kI2cTimeoutMs) == ESP_OK;
}
