#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
#include "provisions_crash_record.h"
#endif

#define TAG "main"

extern "C" void app_main(void)
{
    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        // NVS also owns the only key for acknowledged offline recordings.
        // Preserve it for recovery; automatic erasure would destroy that audio.
        ESP_LOGE(TAG, "NVS initialization failed; preserving configuration and recording key");
#else
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
#endif
    }
    ESP_ERROR_CHECK(ret);
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
    // Before anything else can crash again: persist the last panic record and
    // this boot's reset reason for the gateway hello.
    provisions::crash::CaptureAtBoot();
#endif

    // Initialize and run the application
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();  // This function runs the main event loop and never returns
}
