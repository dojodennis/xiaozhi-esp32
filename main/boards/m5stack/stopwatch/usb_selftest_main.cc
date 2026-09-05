#include "sdkconfig.h"

#if CONFIG_ORBIT_USB_SELFTEST

#if !CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_STOPWATCH || !CONFIG_PROVISIONS_GATEWAY_REQUIRED || \
    CONFIG_SECURE_BOOT || CONFIG_SECURE_FLASH_ENC_ENABLED || !CONFIG_LV_USE_SNAPSHOT
#error "USB self-test is restricted to the reversible C152 snapshot-enabled bench"
#endif

#include "assets/lang_config.h"
#include "board.h"
#include "display/lcd_display.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "usb_selftest_packet.h"

#include <driver/usb_serial_jtag.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <array>
#include <cstring>

constexpr uint16_t kStopwatchInitialClearColor = 0x0000;
LV_FONT_DECLARE(font_noto_sans_basic_30_4);
#include "crest_display.h"

// Every NVS handle in this diagnostic image is read-only, even if shared code
// requests read/write. Missing identity fails closed before board construction.
extern "C" esp_err_t __real_nvs_open(const char*, nvs_open_mode_t, nvs_handle_t*);
extern "C" esp_err_t __wrap_nvs_open(const char* name, nvs_open_mode_t, nvs_handle_t* handle) {
    return __real_nvs_open(name, NVS_READONLY, handle);
}

namespace {
OrbitCrestDisplay* display = nullptr;

bool Send(const std::string& text) {
    size_t sent = 0;
    const int64_t deadline = esp_timer_get_time() + 10000000;
    while (sent < text.size() && esp_timer_get_time() < deadline) {
        const int count =
            usb_serial_jtag_write_bytes(text.data() + sent, text.size() - sent, pdMS_TO_TICKS(100));
        if (count > 0)
            sent += count;
    }
    return sent == text.size();
}

void Advance(uint32_t duration_ms, int input = -1, int output = -1) {
    const int64_t deadline = esp_timer_get_time() + duration_ms * 1000LL;
    std::array<int16_t, 128> pcm{};
    do {
        const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        if (input >= 0) {
            pcm.fill(static_cast<int16_t>(input));
            OrbitCrest::input_meter.Observe(pcm.data(), pcm.size(), now);
        }
        if (output >= 0) {
            pcm.fill(static_cast<int16_t>(output));
            OrbitCrest::output_meter.Observe(pcm.data(), pcm.size(), now);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (esp_timer_get_time() < deadline);
}

bool Capture(const char* name) {
    std::string metadata;
    auto* snapshot = display->BenchSnapshot(metadata);
    if (!snapshot)
        return Send("ORBIT_FAIL_V1 snapshot\n") && false;
    auto payload = OrbitUsbSelfTest::Encode(snapshot->data, snapshot->header.w, snapshot->header.h,
                                            snapshot->header.stride);
    const uint32_t width = snapshot->header.w;
    const uint32_t height = snapshot->header.h;
    lv_draw_buf_destroy(snapshot);
    if (payload.empty())
        return false;
    char header[160];
    snprintf(header, sizeof(header), "ORBIT_FRAME_V1 %s %lu %lu %u %08lx\n", name,
             static_cast<unsigned long>(width), static_cast<unsigned long>(height),
             static_cast<unsigned>(payload.size()),
             static_cast<unsigned long>(OrbitUsbSelfTest::Checksum(payload)));
    if (!Send("ORBIT_META_V1 " + std::string(name) + " " + metadata + "\n") || !Send(header))
        return false;
    constexpr char digits[] = "0123456789abcdef";
    for (size_t offset = 0; offset < payload.size(); offset += 256) {
        char prefix[48];
        snprintf(prefix, sizeof(prefix), "ORBIT_DATA_V1 %u ", static_cast<unsigned>(offset));
        std::string line(prefix);
        for (size_t index = offset; index < std::min(offset + 256, payload.size()); ++index) {
            line.push_back(digits[payload[index] >> 4]);
            line.push_back(digits[payload[index] & 15]);
        }
        line.push_back('\n');
        if (!Send(line))
            return false;
        vTaskDelay(1);
    }
    return Send("ORBIT_END_V1 " + std::string(name) + "\n");
}

bool WaitForRun() {
    std::string command;
    const int64_t deadline = esp_timer_get_time() + 180000000;
    int64_t next_ready = 0;
    while (esp_timer_get_time() < deadline) {
        if (esp_timer_get_time() >= next_ready) {
            Send("ORBIT_READY_V1 offline synthetic_only readonly_nvs\n");
            next_ready = esp_timer_get_time() + 1000000;
        }
        char byte;
        if (usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(100)) != 1)
            continue;
        if (byte == '\n') {
            if (command == "ORBIT_SELFTEST_V1 RUN")
                return true;
            command.clear();
        } else if (byte >= 32 && byte <= 126 && command.size() < 40) {
            command.push_back(byte);
        } else {
            command.clear();
        }
    }
    return false;
}

bool Run() {
    display->SetStatus("Ready");
    Advance(300);
    if (!Capture("idle"))
        return false;
    auto& themes = LvglThemeManager::GetInstance();
    if (auto* light = themes.GetTheme("light"))
        display->SetTheme(light);
    Advance(300);
    if (!Capture("light_theme_idle"))
        return false;
    if (auto* dark = themes.GetTheme("dark"))
        display->SetTheme(dark);
    Advance(300);
    if (!Capture("dark_theme_idle"))
        return false;

    display->SetStatus("Listening");
    Advance(400, 150);
    if (!Capture("listening_low"))
        return false;
    Advance(200, 6000);
    if (!Capture("listening_high"))
        return false;
    display->SetStatus("Working");
    Advance(350);
    if (!Capture("thinking"))
        return false;

    display->SetChatMessage("assistant", "Synthetic fixture: this transcript must not appear.");
    display->ShowNotification("Found");
    display->SetStatus("Speaking");
    Advance(400, -1, 4000);
    if (!Capture("speaking_active_a"))
        return false;
    Advance(200, -1, 4000);
    if (!Capture("speaking_active_b"))
        return false;
    Advance(300);
    if (!Capture("speaking_stale_a"))
        return false;
    Advance(250);
    if (!Capture("speaking_stale_b"))
        return false;

    display->SetStatus("Ready");
    Advance(300);
    if (!Capture("result"))
        return false;
    Advance(4200);
    if (!Capture("result_expired_idle"))
        return false;
    display->SetChatMessage("assistant", "Another synthetic transcript that must not render.");
    Advance(300);
    if (!Capture("reply_only"))
        return false;
    Advance(4200);
    display->ShowNotification("Saved");
    display->ShowNotification("HTTP 403 synthetic ignored label");
    Advance(300);
    if (!Capture("unknown_labels_ignored"))
        return false;

    display->SetStatus("Unavailable");
    Advance(350);
    if (!Capture("error"))
        return false;
    display->SetStatus("Ready");
    Advance(350);
    display->SetPowerSaveMode(true);
    Advance(350);
    if (!Capture("sleep"))
        return false;
    display->SetPowerSaveMode(false);
    Advance(350);
    if (!Capture("wake_idle"))
        return false;
    display->SetStatus("Listening");
    Advance(70, 5000);
    display->SetStatus("Ready");
    Advance(70);
    if (!Capture("interrupted_transition"))
        return false;
    Advance(350);
    if (!Capture("final_idle"))
        return false;
    return true;
}

void Worker(void*) {
    const bool passed = WaitForRun() && Run();
    Send(passed ? "ORBIT_DONE_V1 19 synthetic_frames\n" : "ORBIT_FAIL_V1 incomplete\n");
    vTaskDelete(nullptr);
}
}  // namespace

extern "C" void app_main() {
    // Never call the normal Application initialization/run path. No Wi-Fi,
    // gateway, audio service, OTA validation/selector mutation or NVS repair.
    esp_log_level_set("*", ESP_LOG_NONE);
    ESP_ERROR_CHECK(nvs_flash_init());
    nvs_handle_t handle = 0;
    ESP_ERROR_CHECK(nvs_open("board", NVS_READONLY, &handle));
    size_t uuid_size = 0;
    ESP_ERROR_CHECK(nvs_get_str(handle, "uuid", nullptr, &uuid_size));
    nvs_close(handle);
    ESP_ERROR_CHECK(uuid_size == 37 ? ESP_OK : ESP_ERR_INVALID_STATE);
    usb_serial_jtag_driver_config_t serial_config = {
        .tx_buffer_size = 8192,
        .rx_buffer_size = 256,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&serial_config));
    auto& board = Board::GetInstance();
    display = dynamic_cast<OrbitCrestDisplay*>(board.GetDisplay());
    ESP_ERROR_CHECK(display ? ESP_OK : ESP_ERR_INVALID_STATE);
    display->SetupUI();
    ESP_ERROR_CHECK(xTaskCreate(Worker, "orbit_usb_test", 8192, nullptr, 3, nullptr) == pdPASS
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);
}

#endif  // CONFIG_ORBIT_USB_SELFTEST
