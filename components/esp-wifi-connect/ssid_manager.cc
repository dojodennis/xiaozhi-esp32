#include "ssid_manager.h"
#include "sdkconfig.h"

#include <esp_log.h>
#include <nvs_flash.h>
#include <algorithm>

#define TAG "SsidManager"
#define NVS_NAMESPACE "wifi"
#define MAX_WIFI_SSID_COUNT 10

SsidManager::SsidManager() { LoadFromNvs(); }

SsidManager::~SsidManager() {}

void SsidManager::Clear() {
    ssid_list_.clear();
    SaveToNvs();
}

void SsidManager::LoadFromNvs() {
    ssid_list_.clear();

    // Load ssid and password from NVS from namespace "wifi"
    // ssid, ssid1, ssid2, ... ssid9
    // password, password1, password2, ... password9
    nvs_handle_t nvs_handle;
    auto ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        // The namespace doesn't exist, just return
        ESP_LOGW(TAG, "NVS namespace %s doesn't exist", NVS_NAMESPACE);
        return;
    }
    for (int i = 0; i < MAX_WIFI_SSID_COUNT; i++) {
        std::string ssid_key = "ssid";
        if (i > 0) {
            ssid_key += std::to_string(i);
        }
        std::string password_key = "password";
        if (i > 0) {
            password_key += std::to_string(i);
        }

        char ssid[33];
        char password[65];
        size_t length = sizeof(ssid);
        if (nvs_get_str(nvs_handle, ssid_key.c_str(), ssid, &length) != ESP_OK) {
            continue;
        }
        length = sizeof(password);
        if (nvs_get_str(nvs_handle, password_key.c_str(), password, &length) != ESP_OK) {
            continue;
        }
        ssid_list_.push_back({ssid, password});
    }
    nvs_close(nvs_handle);
}

bool SsidManager::SaveToNvs() {
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) != ESP_OK)
        return false;
    bool ok = true;
    for (int i = 0; i < MAX_WIFI_SSID_COUNT; i++) {
        std::string ssid_key = "ssid";
        if (i > 0) {
            ssid_key += std::to_string(i);
        }
        std::string password_key = "password";
        if (i > 0) {
            password_key += std::to_string(i);
        }

        if (i < ssid_list_.size()) {
            ok =
                (nvs_set_str(nvs_handle, ssid_key.c_str(), ssid_list_[i].ssid.c_str()) == ESP_OK) &&
                ok;
            ok = (nvs_set_str(nvs_handle, password_key.c_str(), ssid_list_[i].password.c_str()) ==
                  ESP_OK) &&
                 ok;
        } else {
            nvs_erase_key(nvs_handle, ssid_key.c_str());
            nvs_erase_key(nvs_handle, password_key.c_str());
        }
    }
    ok = ok && nvs_commit(nvs_handle) == ESP_OK;
    nvs_close(nvs_handle);
    return ok;
}

bool SsidManager::AddSsid(const std::string& ssid, const std::string& password) {
    auto previous = ssid_list_;
    for (auto& item : ssid_list_) {
#if !CONFIG_PROVISIONS_GATEWAY_REQUIRED
        ESP_LOGI(TAG, "compare [%s:%d] [%s:%d]", item.ssid.c_str(), item.ssid.size(), ssid.c_str(),
                 ssid.size());
#endif
        if (item.ssid == ssid) {
#if !CONFIG_PROVISIONS_GATEWAY_REQUIRED
            ESP_LOGW(TAG, "SSID %s already exists, overwrite it", ssid.c_str());
#endif
            item.password = password;
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
            // A successful setup selection is the preferred profile, including
            // reselecting an existing network. Persist order and password together.
            const auto index = &item - ssid_list_.data();
            std::rotate(ssid_list_.begin(), ssid_list_.begin() + index,
                        ssid_list_.begin() + index + 1);
#endif
            if (SaveToNvs())
                return true;
            ssid_list_ = std::move(previous);
            return false;
        }
    }

    if (ssid_list_.size() >= MAX_WIFI_SSID_COUNT) {
        ESP_LOGW(TAG, "SSID list is full, pop one");
        ssid_list_.pop_back();
    }
    // Add the new ssid to the front of the list
    ssid_list_.insert(ssid_list_.begin(), {ssid, password});
    if (SaveToNvs())
        return true;
    ssid_list_ = std::move(previous);
    return false;
}

void SsidManager::RemoveSsid(int index) {
    if (index < 0 || index >= ssid_list_.size()) {
        ESP_LOGW(TAG, "Invalid index %d", index);
        return;
    }
    ssid_list_.erase(ssid_list_.begin() + index);
    SaveToNvs();
}

void SsidManager::SetDefaultSsid(int index) {
    if (index < 0 || index >= ssid_list_.size()) {
        ESP_LOGW(TAG, "Invalid index %d", index);
        return;
    }
    // Move the ssid at index to the front of the list
    auto item = ssid_list_[index];
    ssid_list_.erase(ssid_list_.begin() + index);
    ssid_list_.insert(ssid_list_.begin(), item);
    SaveToNvs();
}
