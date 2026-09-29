#include "wifi_board.h"

#include "display.h"
#include "application.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_network.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <orbit_wifi_session.h>
#include <new>
#include <utility>

#include <material_symbols.h>
#include <wifi_manager.h>
#include <wifi_station.h>
#include <ssid_manager.h>
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
#include "blufi.h"
#endif

static const char *TAG = "WifiBoard";

// Connection timeout in seconds
static constexpr int CONNECT_TIMEOUT_SEC = 60;

#if CONFIG_PROVISIONS_GATEWAY_REQUIRED && CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
static std::string OrbitWifiPassword(const uint8_t (&random)[16]) {
    // A non-hex first character keeps Wi-Fi QR scanners from treating the
    // passphrase as a hexadecimal key. The remaining characters are easy to
    // read from the ring when Settings needs a manual password.
    constexpr char first[] = "GHJKLMNPQRSTUVWX";
    constexpr char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    static_assert(sizeof(first) - 1 == 16 && sizeof(alphabet) - 1 == 32);
    std::string password;
    password.reserve(sizeof(random));
    password.push_back(first[random[0] & 0x0f]);
    for (size_t i = 1; i < sizeof(random); ++i)
        password.push_back(alphabet[random[i] & 0x1f]);
    return password;
}
#endif

WifiBoard::WifiBoard() {
    // Create connection timeout timer
    esp_timer_create_args_t timer_args = {
        .callback = OnWifiConnectTimeout,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_connect_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&timer_args, &connect_timer_);
}

WifiBoard::~WifiBoard() {
    if (connect_timer_) {
        esp_timer_stop(connect_timer_);
        esp_timer_delete(connect_timer_);
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

void WifiBoard::StartNetwork() {
    auto& wifi_manager = WifiManager::GetInstance();

    // Initialize WiFi manager
    WifiManagerConfig config;
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    // The pilot is factory-provisioned. Do not expose stock Xiaozhi naming or
    // advanced OTA/sleep controls, and keep saved network names out of logs.
    config.ssid_prefix = "Provisions";
    config.show_ota_config = false;
    config.show_sleep_config = false;
    esp_log_level_set("WifiStation", ESP_LOG_WARN);
    esp_log_level_set("SsidManager", ESP_LOG_WARN);
    esp_log_level_set("wifi", ESP_LOG_WARN);
#else
    config.ssid_prefix = "Xiaozhi";
    config.language = Lang::CODE;
    config.show_ota_config = true;
    config.show_sleep_config = true;
#endif

    // Set a DHCP hostname so the router shows a friendly name instead of "espressif".
    // Uses the same "<prefix>-<last 2 MAC bytes>" scheme as the config AP SSID.
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        char hostname[32];
        snprintf(hostname, sizeof(hostname), "%s-%02X%02X", config.ssid_prefix.c_str(), mac[4], mac[5]);
        config.station_hostname = hostname;
    }
    wifi_manager.Initialize(config);

    // Set unified event callback - forward to NetworkEvent with SSID data
    wifi_manager.SetEventCallback([this](WifiEvent event, const std::string& data) {
        switch (event) {
            case WifiEvent::Scanning:
                OnNetworkEvent(NetworkEvent::Scanning);
                break;
            case WifiEvent::Connecting:
                OnNetworkEvent(NetworkEvent::Connecting, data);
                break;
            case WifiEvent::Connected:
                OnNetworkEvent(NetworkEvent::Connected, data);
                break;
            case WifiEvent::Disconnected:
                OnNetworkEvent(NetworkEvent::Disconnected);
                break;
            case WifiEvent::ConfigModeEnter:
                OnNetworkEvent(NetworkEvent::WifiConfigModeEnter);
                break;
            case WifiEvent::ConfigModeExit:
                OnNetworkEvent(NetworkEvent::WifiConfigModeExit);
                break;
        }
    });

    // Try to connect or enter config mode
    TryWifiConnect();
}

void WifiBoard::TryWifiConnect() {
    auto& ssid_manager = SsidManager::GetInstance();
    bool have_ssid = !ssid_manager.GetSsidList().empty();

    if (have_ssid) {
        // Start connection attempt with timeout
        ESP_LOGI(TAG, "Starting WiFi connection attempt");
        esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL);
        WifiManager::GetInstance().StartStation();
    } else {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        // Missing profiles remain offline until the chef deliberately opens
        // QR setup from the menu; never expose a hotspot automatically.
        ESP_LOGE(TAG, "Factory WiFi profile is missing");
        in_config_mode_ = false;
        Application::GetInstance().SetDeviceState(kDeviceStateIdle);
        GetDisplay()->SetStatus("Unavailable");
#else
        // No SSID configured, enter config mode
        // Wait for the board version to be shown
        vTaskDelay(pdMS_TO_TICKS(1500));
        StartWifiConfigMode();
#endif
    }
}

void WifiBoard::OnNetworkEvent(NetworkEvent event, const std::string& data) {
    switch (event) {
        case NetworkEvent::Connected:
            // Stop timeout timer
            esp_timer_stop(connect_timer_);
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
            // make sure blufi resources has been released
            Blufi::GetInstance().deinit();
#endif
            in_config_mode_ = false;
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
            ESP_LOGI(TAG, "Connected to the configured WiFi profile");
#else
            ESP_LOGI(TAG, "Connected to WiFi: %s", data.c_str());
#endif
            break;
        case NetworkEvent::Scanning:
            ESP_LOGI(TAG, "WiFi scanning");
            break;
        case NetworkEvent::Connecting:
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
            ESP_LOGI(TAG, "Connecting to the configured WiFi profile");
#else
            ESP_LOGI(TAG, "WiFi connecting to %s", data.c_str());
#endif
            break;
        case NetworkEvent::Disconnected:
            ESP_LOGW(TAG, "WiFi disconnected");
            break;
        case NetworkEvent::WifiConfigModeEnter:
            ESP_LOGI(TAG, "WiFi config mode entered");
            in_config_mode_ = true;
            break;
        case NetworkEvent::WifiConfigModeExit:
            ESP_LOGI(TAG, "WiFi config mode exited");
            in_config_mode_ = false;
            // Try to connect with the new credentials
            TryWifiConnect();
            break;
        default:
            break;
    }

    // Notify external callback if set
    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

void WifiBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
}

void WifiBoard::OnWifiConnectTimeout(void* arg) {
    auto* board = static_cast<WifiBoard*>(arg);
    if (board->orbit_setup_running_.load())
        return;
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    // WifiStation owns reconnect/backoff. Do not race a late Connected event by
    // stopping it from the timer task; only update the UI on the application task.
    ESP_LOGW(TAG, "Configured WiFi connection is still unavailable");
    Application::GetInstance().Schedule([board]() {
        if (board->orbit_setup_running_.load())
            return;
        if (WifiManager::GetInstance().IsConnected()) {
            return;
        }
        board->in_config_mode_ = false;
        board->GetDisplay()->SetStatus("Unavailable");
    });
#else
    ESP_LOGW(TAG, "WiFi connection timeout, entering config mode");

    WifiManager::GetInstance().StopStation();
    board->StartWifiConfigMode();
#endif
}

void WifiBoard::StartWifiConfigMode() {
    in_config_mode_ = true;
    // Transition to wifi configuring state
    Application::GetInstance().SetDeviceState(kDeviceStateWifiConfiguring);
#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
    auto& wifi_manager = WifiManager::GetInstance();

    wifi_manager.StartConfigAp();

    // Show config prompt after a short delay
    Application::GetInstance().Schedule([&wifi_manager]() {
        std::string hint = Lang::Strings::CONNECT_TO_HOTSPOT;
        hint += wifi_manager.GetApSsid();
        hint += Lang::Strings::ACCESS_VIA_BROWSER;
        hint += wifi_manager.GetApWebUrl();

        Application::GetInstance().Alert(Lang::Strings::WIFI_CONFIG_MODE, hint.c_str(), "gear", Lang::Sounds::OGG_WIFICONFIG);
    });
#elif CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    auto &blufi = Blufi::GetInstance();
    // initialize esp-blufi protocol
    blufi.init();
#endif
}

void WifiBoard::EnterWifiConfigMode() {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    ESP_LOGW(TAG, "Interactive WiFi configuration is disabled on factory-provisioned builds");
    GetDisplay()->SetStatus("Unavailable");
    return;
#endif
    ESP_LOGI(TAG, "EnterWifiConfigMode called");
    GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);

    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();

    if (state == kDeviceStateSpeaking || state == kDeviceStateNotifying ||
        state == kDeviceStateListening || state == kDeviceStateIdle) {
        // Reset protocol (close audio channel, reset protocol)
        Application::GetInstance().ResetProtocol();

        xTaskCreate([](void* arg) {
            auto* board = static_cast<WifiBoard*>(arg);

            // Wait for 1 second to allow speaking to finish gracefully
            vTaskDelay(pdMS_TO_TICKS(1000));

            // Stop any ongoing connection attempt
            esp_timer_stop(board->connect_timer_);
            WifiManager::GetInstance().StopStation();

            // Enter config mode
            board->StartWifiConfigMode();

            vTaskDelete(NULL);
        }, "wifi_cfg_delay", 4096, this, 2, NULL);
        return;
    }

    if (state != kDeviceStateStarting) {
        ESP_LOGE(TAG, "EnterWifiConfigMode called but device state is not starting or speaking, device state: %d", state);
        return;
    }

    // Stop any ongoing connection attempt
    esp_timer_stop(connect_timer_);
    WifiManager::GetInstance().StopStation();

    StartWifiConfigMode();
}

bool WifiBoard::IsInWifiConfigMode() const {
    return WifiManager::GetInstance().IsConfigMode();
}

NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

const char* WifiBoard::GetNetworkStateIcon() {
    auto& wifi = WifiManager::GetInstance();

    if (wifi.IsConfigMode()) {
        return MATERIAL_SYMBOLS_WIFI;
    }
    if (!wifi.IsConnected()) {
        return MATERIAL_SYMBOLS_WIFI_OFF;
    }

    int rssi = wifi.GetRssi();
    if (rssi >= -65) {
        return MATERIAL_SYMBOLS_WIFI;
    } else if (rssi >= -75) {
        return MATERIAL_SYMBOLS_WIFI_2_BAR;
    }
    return MATERIAL_SYMBOLS_WIFI_1_BAR;
}

std::string WifiBoard::GetBoardJson() {
    auto& wifi = WifiManager::GetInstance();
    std::string json = R"({"type":")" + std::string(BOARD_TYPE) + R"(",)";
    json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";
    json += R"("manufacturer":")" + std::string(BOARD_MANUFACTURER) + R"(",)";

    if (!wifi.IsConfigMode()) {
        json += R"("ssid":")" + wifi.GetSsid() + R"(",)";
        json += R"("rssi":)" + std::to_string(wifi.GetRssi()) + R"(,)";
        json += R"("channel":)" + std::to_string(wifi.GetChannel()) + R"(,)";
        json += R"("ip":")" + wifi.GetIpAddress() + R"(",)";
    }

    json += R"("mac":")" + SystemInfo::GetMacAddress() + R"("})";
    return json;
}

void WifiBoard::SetPowerSaveLevel(PowerSaveLevel level) {
    WifiPowerSaveLevel wifi_level;
    switch (level) {
        case PowerSaveLevel::LOW_POWER:
            wifi_level = WifiPowerSaveLevel::LOW_POWER;
            break;
        case PowerSaveLevel::BALANCED:
            wifi_level = WifiPowerSaveLevel::BALANCED;
            break;
        case PowerSaveLevel::PERFORMANCE:
        default:
            wifi_level = WifiPowerSaveLevel::PERFORMANCE;
            break;
    }
    WifiManager::GetInstance().SetPowerSaveLevel(wifi_level);
}

std::string WifiBoard::GetDeviceStatusJson() {
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    if (auto codec = board.GetAudioCodec()) {
        cJSON_AddNumberToObject(audio_speaker, "volume", codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen
    auto screen = cJSON_CreateObject();
    if (auto backlight = board.GetBacklight()) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    if (auto display = board.GetDisplay(); display && display->height() > 64) {
        if (auto theme = display->GetTheme()) {
            cJSON_AddStringToObject(screen, "theme", theme->name().c_str());
        }
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int level = 0;
    bool charging = false, discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        auto battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto& wifi = WifiManager::GetInstance();
    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi.GetSsid().c_str());
    int rssi = wifi.GetRssi();
    const char* signal = rssi >= -60 ? "strong" : (rssi >= -70 ? "medium" : "weak");
    cJSON_AddStringToObject(network, "signal", signal);
    cJSON_AddItemToObject(root, "network", network);

    // Chip temperature
    float temp = 0.0f;
    if (board.GetTemperature(temp)) {
        auto chip = cJSON_CreateObject();
        cJSON_AddNumberToObject(chip, "temperature", temp);
        cJSON_AddItemToObject(root, "chip", chip);
    }

    auto str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}

bool WifiBoard::StartWifiSetup(std::function<void(std::string)> ready,
                               std::function<void(bool)> finished) {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED && CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
    if (orbit_setup_running_.exchange(true))
        return false;
    orbit_setup_cancelled_.store(false);
    struct Work {
        WifiBoard* board;
        std::function<void(std::string)> ready;
        std::function<void(bool)> finished;
    };
    auto* work = new (std::nothrow) Work{this, std::move(ready), std::move(finished)};
    if (!work) {
        orbit_setup_running_.store(false);
        return false;
    }
    esp_timer_stop(connect_timer_);
    auto started = xTaskCreate(
        [](void* raw) {
            std::unique_ptr<Work> work(static_cast<Work*>(raw));
            auto& wifi = WifiManager::GetInstance();
            uint8_t random[16];
            esp_fill_random(random, sizeof(random));
            std::string password = OrbitWifiPassword(random);
            std::fill(random, random + sizeof(random), 0);
            bool saved = false;
            if (wifi.PrepareOrbitSetup(password) && !work->board->orbit_setup_cancelled_.load()) {
                wifi.StartConfigAp();
                if (wifi.IsConfigMode()) {
                    work->ready("WIFI:T:WPA;S:" + wifi.GetApSsid() + ";P:" + password + ";;");
                    int result;
                    do {
                        vTaskDelay(pdMS_TO_TICKS(100));
                        result = wifi.OrbitSetupResult();
                    } while (result == static_cast<int>(OrbitWifiSession::Result::Active));
                    saved = result == static_cast<int>(OrbitWifiSession::Result::Saved);
                    // Let the bounded HTTP response finish before closing services.
                    if (saved)
                        vTaskDelay(pdMS_TO_TICKS(250));
                    wifi.StopConfigAp();  // Existing ConfigModeExit restarts saved station
                                          // profiles.
                }
            }
            wifi.DiscardOrbitSetup();
            std::fill(password.begin(), password.end(), '\0');
            work->board->orbit_setup_running_.store(false);
            work->finished(saved);
            work.reset();
            vTaskDelete(nullptr);
        },
        "orbit_wifi_setup", 6144, work, 4, nullptr);
    if (started != pdPASS) {
        delete work;
        orbit_setup_running_.store(false);
        return false;
    }
    return true;
#else
    return false;
#endif
}
void WifiBoard::CancelWifiSetup() {
    if (orbit_setup_running_.load()) {
        orbit_setup_cancelled_.store(true);
        WifiManager::GetInstance().CancelOrbitSetup();
    }
}
