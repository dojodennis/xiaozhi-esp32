/*
 * WiFi Manager Implementation
 */

#include "wifi_manager.h"
#include "wifi_configuration_ap.h"
#include "wifi_station.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <nvs_flash.h>

#define TAG "WifiManager"

WifiManager& WifiManager::GetInstance() {
    static WifiManager instance;
    return instance;
}

WifiManager::WifiManager() = default;

WifiManager::~WifiManager() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    // Event callbacks may query manager state while unregister waits for them.
    if (station_active_ && station_)
        station_->Stop();
    if (config_mode_active_ && config_ap_)
        config_ap_->Stop();
    if (initialized_)
        esp_wifi_deinit();
}

void WifiManager::NotifyEvent(WifiEvent event, const std::string& data) {
    // Copy callback under lock, invoke without lock to avoid deadlock
    std::function<void(WifiEvent, const std::string&)> callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = event_callback_;
    }
    if (callback) {
        callback(event, data);
    }
}

bool WifiManager::Initialize(const WifiManagerConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (initialized_) {
        ESP_LOGW(TAG, "Already initialized");
        return true;
    }

    config_ = config;
    ESP_LOGI(TAG, "Initializing...");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Initialize netif
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Netif init failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Create event loop
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Event loop create failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Initialize WiFi driver
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(ret));
        return false;
    }

    station_ = std::make_unique<WifiStation>();
    config_ap_ = std::make_unique<WifiConfigurationAp>();

    initialized_ = true;
    ESP_LOGI(TAG, "Initialized");
    return true;
}

bool WifiManager::IsInitialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

// ==================== Station Mode ====================

void WifiManager::StartStation() {
    std::unique_lock<std::mutex> lifecycle(lifecycle_mutex_);
    bool stopped_ap;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_ || station_active_)
            return;
        stopped_ap = config_mode_active_;
        config_mode_active_ = false;
    }
    // Do not hold mutex_ across IDF registration/unregistration or synchronous
    // station callbacks: the event task needs it to publish/query manager state.
    if (stopped_ap)
        config_ap_->Stop();
    station_->SetScanIntervalRange(config_.station_scan_min_interval_seconds,
                                   config_.station_scan_max_interval_seconds);
    station_->SetFailureRetryCnt(config_.station_failure_retry_cnt);
    station_->SetHostname(config_.station_hostname);
    station_->OnScanBegin([this]() { NotifyEvent(WifiEvent::Scanning); });
    station_->OnConnect(
        [this](const std::string& ssid) { NotifyEvent(WifiEvent::Connecting, ssid); });
    station_->OnConnected(
        [this](const std::string& ssid) { NotifyEvent(WifiEvent::Connected, ssid); });
    station_->OnDisconnected(
        [this](int reason) { NotifyEvent(WifiEvent::Disconnected, std::to_string(reason)); });
    station_->Start();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        station_active_ = true;
    }
    lifecycle.unlock();
    if (stopped_ap)
        NotifyEvent(WifiEvent::ConfigModeExit);
}

void WifiManager::StopStation() {
    std::unique_lock<std::mutex> lifecycle(lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!station_active_)
            return;
        station_active_ = false;
    }
    station_->Stop();
    lifecycle.unlock();
    NotifyEvent(WifiEvent::Disconnected);
}

bool WifiManager::IsConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return station_active_ && station_ && station_->IsConnected();
}

std::string WifiManager::GetSsid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!station_active_ || !station_)
        return "";
    return station_->GetSsid();
}

std::string WifiManager::GetIpAddress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!station_active_ || !station_)
        return "";
    return station_->GetIpAddress();
}

int WifiManager::GetRssi() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!station_active_ || !station_ || !station_->IsConnected())
        return 0;
    return station_->GetRssi();
}

int WifiManager::GetChannel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!station_active_ || !station_ || !station_->IsConnected())
        return 0;
    return station_->GetChannel();
}

std::string WifiManager::GetMacAddress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mac_address_.empty()) {
        return mac_address_;
    }

    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        char buf[18];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
                 mac[4], mac[5]);
        mac_address_ = buf;
    }
    return mac_address_;
}

// ==================== Config AP Mode ====================

void WifiManager::StartConfigAp() {
    std::unique_lock<std::mutex> lifecycle(lifecycle_mutex_);
    bool stopped_station;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_ || config_mode_active_)
            return;
        stopped_station = station_active_;
        station_active_ = false;
    }
    if (stopped_station)
        station_->Stop();
    config_ap_->SetSsidPrefix(config_.ssid_prefix);
    config_ap_->SetLanguage(config_.language);
    config_ap_->SetShowOtaConfig(config_.show_ota_config);
    config_ap_->SetShowSleepConfig(config_.show_sleep_config);
    config_ap_->OnExitRequested([this]() { StopConfigAp(); });
    config_ap_->Start();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_mode_active_ = true;
    }
    lifecycle.unlock();
    if (stopped_station)
        NotifyEvent(WifiEvent::Disconnected);
    NotifyEvent(WifiEvent::ConfigModeEnter);
}

void WifiManager::StopConfigAp() {
    std::unique_lock<std::mutex> lifecycle(lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!config_mode_active_)
            return;
        config_mode_active_ = false;
    }
    config_ap_->Stop();
    lifecycle.unlock();
    // The board's ConfigModeExit callback starts the saved station profiles.
    NotifyEvent(WifiEvent::ConfigModeExit);
}

bool WifiManager::IsConfigMode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_mode_active_;
}

std::string WifiManager::GetApSsid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_mode_active_ || !config_ap_)
        return "";
    return config_ap_->GetSsid();
}

std::string WifiManager::GetApWebUrl() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_mode_active_ || !config_ap_)
        return "";
    return config_ap_->GetWebServerUrl();
}

// ==================== Power ====================

void WifiManager::SetPowerSaveLevel(WifiPowerSaveLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!station_active_ || !station_) {
        return;
    }
    station_->SetPowerSaveLevel(level);
}

// ==================== Event ====================

void WifiManager::SetEventCallback(std::function<void(WifiEvent, const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    event_callback_ = std::move(callback);
}

// These methods never call user callbacks while holding the manager lock.
bool WifiManager::PrepareOrbitSetup(const std::string& password) {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || config_mode_active_ || password.size() < 12)
        return false;
    config_ap_->PrepareOrbitSetup(password);
    return true;
}
void WifiManager::DiscardOrbitSetup() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_mode_active_)
        config_ap_->DiscardOrbitSetup();
}
void WifiManager::CancelOrbitSetup() {
    // The AP object lives for the manager lifetime. Do not wait for StopConfigAp
    // (which joins HTTP) on the application task merely to signal cancellation.
    config_ap_->CancelOrbitSetup();
}
int WifiManager::OrbitSetupResult() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(config_ap_->OrbitSetupResult());
}
