#include "ota.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
#include "provisions_endpoint_policy.h"
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_heap_caps.h>
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
#include <psa/crypto.h>
#endif
#ifdef SOC_HMAC_SUPPORTED
#include <esp_hmac.h>
#endif

#include <cstring>
#include <vector>
#include <sstream>
#include <algorithm>

#define TAG "Ota"


Ota::Ota() {
#ifdef ESP_EFUSE_BLOCK_USR_DATA
    // Read Serial Number from efuse user_data
    uint8_t serial_number[33] = {0};
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, serial_number, 32 * 8) == ESP_OK) {
        if (serial_number[0] == 0) {
            has_serial_number_ = false;
        } else {
            serial_number_ = std::string(reinterpret_cast<char*>(serial_number), 32);
            has_serial_number_ = true;
        }
    }
#endif
}

Ota::~Ota() {
}

std::string Ota::GetCheckVersionUrl() {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    return ProvisionsEndpointPolicy::BootstrapUrl();
#else
    Settings settings("wifi", false);
    std::string url = settings.GetString("ota_url");
    if (url.empty()) {
        url = CONFIG_OTA_URL;
    }
    return url;
#endif
}

std::unique_ptr<Http> Ota::SetupHttp() {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    auto user_agent = SystemInfo::GetUserAgent();
    http->SetHeader("Activation-Version", has_serial_number_ ? "2" : "1");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", board.GetUuid());
    if (has_serial_number_) {
        http->SetHeader("Serial-Number", serial_number_.c_str());
        ESP_LOGI(TAG, "Setup HTTP, User-Agent: %s, Serial-Number: %s", user_agent.c_str(), serial_number_.c_str());
    }
    http->SetHeader("User-Agent", user_agent);
    http->SetHeader("Accept-Language", Lang::CODE);
    http->SetHeader("Content-Type", "application/json");
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    Settings settings("provisions", false);
    std::string token = settings.GetString("device_token");
    if (!ProvisionsEndpointPolicy::IsValidDeviceToken(token)) {
        ESP_LOGE(TAG, "Provisions device credential is missing or malformed");
        return nullptr;
    }
    http->SetHeader("Authorization", "Bearer " + token);
    http->SetHeader("Protocol-Version", "1");
    http->SetHeader("X-Provisions-Boot-Id", SystemInfo::GetBootId());
    http->SetHeader("X-Provisions-Firmware-Version", esp_app_get_description()->version);
#endif

    return http;
}

/* 
 * Specification: https://ccnphfhqs21z.feishu.cn/wiki/FjW6wZmisimNBBkov6OcmfvknVd
 */
esp_err_t Ota::CheckVersion() {
    auto& board = Board::GetInstance();
    auto app_desc = esp_app_get_description();

    // Check if there is a new firmware version available
    current_version_ = app_desc->version;
    ESP_LOGI(TAG, "Current version: %s", current_version_.c_str());

    std::string url = GetCheckVersionUrl();
    if (url.length() < 10) {
        ESP_LOGE(TAG, "Check version URL is not properly set");
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    if (!ProvisionsEndpointPolicy::IsAllowedBootstrapUrl(url)) {
        ESP_LOGE(TAG, "Compiled Provisions bootstrap endpoint was rejected");
        return ESP_ERR_INVALID_ARG;
    }
#endif

    auto http = SetupHttp();
    if (http == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    std::string data = board.GetSystemInfoJson();
    std::string method = data.length() > 0 ? "POST" : "GET";
    http->SetContent(std::move(data));

    if (!http->Open(method, url)) {
        int last_error = http->GetLastError();
        ESP_LOGE(TAG, "Failed to open HTTP connection, code=0x%x", last_error);
        return last_error;
    }

    auto status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to check version, status code: %d", status_code);
        return status_code;
    }

    data = http->ReadAll();
    http->Close();

    // Response: { "firmware": { "version": "1.0.0", "url": "http://" } }
    // Parse the JSON response and check if the version is newer
    // If it is, set has_new_version_ to true and store the new version and URL
    
    cJSON *root = cJSON_Parse(data.c_str());
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    has_activation_code_ = false;
    has_activation_challenge_ = false;
    cJSON *activation = cJSON_GetObjectItem(root, "activation");
    if (cJSON_IsObject(activation)) {
        cJSON* message = cJSON_GetObjectItem(activation, "message");
        if (cJSON_IsString(message)) {
            activation_message_ = message->valuestring;
        }
        cJSON* code = cJSON_GetObjectItem(activation, "code");
        if (cJSON_IsString(code)) {
            activation_code_ = code->valuestring;
            has_activation_code_ = true;
        }
        cJSON* challenge = cJSON_GetObjectItem(activation, "challenge");
        if (cJSON_IsString(challenge)) {
            activation_challenge_ = challenge->valuestring;
            has_activation_challenge_ = true;
        }
        cJSON* timeout_ms = cJSON_GetObjectItem(activation, "timeout_ms");
        if (cJSON_IsNumber(timeout_ms)) {
            activation_timeout_ms_ = timeout_ms->valueint;
        }
    }

    has_mqtt_config_ = false;
    cJSON *mqtt = cJSON_GetObjectItem(root, "mqtt");
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    if (cJSON_IsObject(mqtt)) {
        ESP_LOGW(TAG, "Ignoring MQTT configuration for the Provisions gateway build");
    }
#else
    if (cJSON_IsObject(mqtt)) {
        Settings settings("mqtt", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, mqtt) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (settings.GetInt(item->string) != item->valueint) {
                    settings.SetInt(item->string, item->valueint);
                }
            }
        }
        has_mqtt_config_ = true;
    } else {
        ESP_LOGI(TAG, "No mqtt section found !");
    }
#endif

    has_websocket_config_ = false;
    cJSON *websocket = cJSON_GetObjectItem(root, "websocket");
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    if (cJSON_IsObject(websocket)) {
        cJSON* url = cJSON_GetObjectItem(websocket, "url");
        cJSON* version = cJSON_GetObjectItem(websocket, "version");
        const bool valid_version =
            version == nullptr || (cJSON_IsNumber(version) && version->valueint == 1);
        if (cJSON_IsString(url) && valid_version &&
            ProvisionsEndpointPolicy::IsAllowedWebsocketUrl(url->valuestring)) {
            Settings settings("websocket", true);
            settings.SetString("url", ProvisionsEndpointPolicy::WebsocketUrl());
            settings.EraseKey("token");
            settings.SetInt("version", 1);
            has_websocket_config_ = true;
        } else {
            ESP_LOGE(TAG, "Bootstrap returned an unapproved WebSocket configuration");
        }
    } else {
        ESP_LOGE(TAG, "Bootstrap response has no WebSocket configuration");
    }
#else
    if (cJSON_IsObject(websocket)) {
        Settings settings("websocket", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, websocket) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (settings.GetInt(item->string) != item->valueint) {
                    settings.SetInt(item->string, item->valueint);
                }
            }
        }
        has_websocket_config_ = true;
    } else {
        ESP_LOGI(TAG, "No websocket section found!");
    }
#endif

    has_server_time_ = false;
    cJSON *server_time = cJSON_GetObjectItem(root, "server_time");
    if (cJSON_IsObject(server_time)) {
        cJSON *timestamp = cJSON_GetObjectItem(server_time, "timestamp");
        cJSON *timezone_offset = cJSON_GetObjectItem(server_time, "timezone_offset");
        
        if (cJSON_IsNumber(timestamp)) {
            // 设置系统时间
            struct timeval tv;
            double ts = timestamp->valuedouble;
            
            // 如果有时区偏移，计算本地时间
            if (cJSON_IsNumber(timezone_offset)) {
                ts += (timezone_offset->valueint * 60 * 1000); // 转换分钟为毫秒
            }
            
            tv.tv_sec = (time_t)(ts / 1000);  // 转换毫秒为秒
            tv.tv_usec = (suseconds_t)((long long)ts % 1000) * 1000;  // 剩余的毫秒转换为微秒
            settimeofday(&tv, NULL);
            has_server_time_ = true;
        }
    } else {
        ESP_LOGW(TAG, "No server_time section found!");
    }

    has_new_version_ = false;
    firmware_version_.clear();
    firmware_url_.clear();
    cJSON *firmware = cJSON_GetObjectItem(root, "firmware");
    if (cJSON_IsObject(firmware)) {
        cJSON *version = cJSON_GetObjectItem(firmware, "version");
        if (cJSON_IsString(version)) {
            firmware_version_ = version->valuestring;
        }
        cJSON *url = cJSON_GetObjectItem(firmware, "url");
        if (cJSON_IsString(url)
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
            && cJSON_IsString(version) &&
            ProvisionsEndpointPolicy::FirmwareUrlMatchesVersion(url->valuestring,
                                                                 version->valuestring)
#endif
        ) {
            firmware_url_ = url->valuestring;
        }

        if (cJSON_IsString(version) && !firmware_url_.empty()) {
            // Check if the version is newer, for example, 0.1.0 is newer than 0.0.1
            has_new_version_ = IsNewVersionAvailable(current_version_, firmware_version_);
            if (has_new_version_) {
                ESP_LOGI(TAG, "New version available: %s", firmware_version_.c_str());
            } else {
                ESP_LOGI(TAG, "Current is the latest version");
            }
            // If the force flag is set to 1, the given version is forced to be installed
            cJSON *force = cJSON_GetObjectItem(firmware, "force");
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
            if (cJSON_IsNumber(force) && force->valueint == 1) {
                ESP_LOGW(TAG, "Ignoring forced or downgrade OTA for the Provisions build");
            }
#else
            if (cJSON_IsNumber(force) && force->valueint == 1) {
                has_new_version_ = true;
            }
#endif
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        } else if (cJSON_IsString(url)) {
            ESP_LOGE(TAG, "Bootstrap returned an unapproved firmware URL");
#endif
        }
    } else {
        ESP_LOGW(TAG, "No firmware section found!");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

void Ota::MarkCurrentVersionValid() {
    auto partition = esp_ota_get_running_partition();
    if (strcmp(partition->label, "factory") == 0) {
        ESP_LOGI(TAG, "Running from factory partition, skipping");
        return;
    }

    ESP_LOGI(TAG, "Running partition: %s", partition->label);
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get state of partition");
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking firmware as valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

bool Ota::IsCurrentVersionPendingVerification() const {
    auto partition = esp_ota_get_running_partition();
    if (partition == nullptr || strcmp(partition->label, "factory") == 0) {
        return false;
    }
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(partition, &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

bool Ota::Upgrade(const std::string& firmware_url, std::function<void(int progress, size_t speed)> callback) {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    if (!ProvisionsEndpointPolicy::IsAllowedFirmwareUrl(firmware_url)) {
        ESP_LOGE(TAG, "Refusing firmware URL outside the Provisions preview policy");
        return false;
    }
    std::array<uint8_t, 32> expected_sha256{};
    if (!ProvisionsEndpointPolicy::ExtractFirmwareSha256(firmware_url, expected_sha256)) {
        ESP_LOGE(TAG, "Approved firmware URL did not contain a valid SHA-256 digest");
        return false;
    }
#endif
    ESP_LOGI(TAG, "Upgrading firmware from %s", firmware_url.c_str());
    esp_ota_handle_t update_handle = 0;
    auto update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get update partition");
        return false;
    }

    ESP_LOGI(TAG, "Writing to partition %s at offset 0x%lx", update_partition->label, update_partition->address);
    bool image_header_checked = false;
    std::string image_header;

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    Settings settings("provisions", false);
    std::string token = settings.GetString("device_token");
    if (!ProvisionsEndpointPolicy::IsValidDeviceToken(token)) {
        ESP_LOGE(TAG, "Provisions device credential is missing or malformed");
        return false;
    }
    http->SetHeader("Authorization", "Bearer " + token);
    http->SetHeader("X-Provisions-Boot-Id", SystemInfo::GetBootId());
    http->SetHeader("X-Provisions-Firmware-Version", esp_app_get_description()->version);
#endif
    if (!http->Open("GET", firmware_url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return false;
    }

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to get firmware, status code: %d", http->GetStatusCode());
        http->Close();
        return false;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length == 0 || content_length > update_partition->size) {
        ESP_LOGE(TAG, "Firmware content length is missing or exceeds the OTA partition");
        http->Close();
        return false;
    }

    constexpr size_t PAGE_SIZE = 4096;
    char* buffer = (char*)heap_caps_malloc(PAGE_SIZE, MALLOC_CAP_INTERNAL);
    if (buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        http->Close();
        return false;
    }

    size_t buffer_offset = 0;  // Current data size in buffer
    size_t total_read = 0, recent_read = 0;
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    psa_hash_operation_t sha256_operation = PSA_HASH_OPERATION_INIT;
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_setup(&sha256_operation, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Failed to initialize firmware SHA-256 verification");
        psa_hash_abort(&sha256_operation);
        heap_caps_free(buffer);
        http->Close();
        return false;
    }
#endif
    auto last_calc_time = esp_timer_get_time();
    while (true) {
        const size_t write_offset = buffer_offset;
        int ret = http->Read(buffer + buffer_offset, PAGE_SIZE - buffer_offset);
        if (ret < 0) {
            ESP_LOGE(TAG, "Failed to read HTTP data: %s", esp_err_to_name(ret));
            if (update_handle != 0) {
                esp_ota_abort(update_handle);
            }
            http->Close();
            heap_caps_free(buffer);
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
            psa_hash_abort(&sha256_operation);
#endif
            return false;
        }
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        if (ret > 0 && total_read + static_cast<size_t>(ret) > content_length) {
            ESP_LOGE(TAG, "Firmware response exceeded its declared content length");
            if (update_handle != 0) {
                esp_ota_abort(update_handle);
            }
            psa_hash_abort(&sha256_operation);
            http->Close();
            heap_caps_free(buffer);
            return false;
        }
#endif
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        if (ret > 0 &&
            psa_hash_update(&sha256_operation,
                            reinterpret_cast<const uint8_t*>(buffer + write_offset),
                            static_cast<size_t>(ret)) != PSA_SUCCESS) {
            ESP_LOGE(TAG, "Failed to hash downloaded firmware");
            if (update_handle != 0) {
                esp_ota_abort(update_handle);
            }
            psa_hash_abort(&sha256_operation);
            http->Close();
            heap_caps_free(buffer);
            return false;
        }
#endif

        // Calculate speed and progress every second
        recent_read += ret;
        total_read += ret;
        buffer_offset += ret;
        if (esp_timer_get_time() - last_calc_time >= 1000000 || ret == 0) {
            size_t progress = total_read * 100 / content_length;
            ESP_LOGI(TAG, "Progress: %u%% (%u/%u), Speed: %uB/s", progress, total_read, content_length, recent_read);
            if (callback) {
                callback(progress, recent_read);
            }
            last_calc_time = esp_timer_get_time();
            recent_read = 0;
        }

        if (!image_header_checked) {
            image_header.append(buffer + write_offset, static_cast<size_t>(ret));
            if (image_header.size() >= sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                esp_app_desc_t new_app_info;
                memcpy(&new_app_info, image_header.data() + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(esp_app_desc_t));

#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
                const std::string embedded_version(
                    new_app_info.version,
                    strnlen(new_app_info.version, sizeof(new_app_info.version)));
                const std::string running_version = esp_app_get_description()->version;
                if (new_app_info.magic_word != ESP_APP_DESC_MAGIC_WORD ||
                    !ProvisionsEndpointPolicy::IsApprovedFirmwareImageVersion(
                        firmware_url, running_version, embedded_version)) {
                    ESP_LOGE(TAG, "Firmware image version did not match the approved upgrade");
                    psa_hash_abort(&sha256_operation);
                    http->Close();
                    heap_caps_free(buffer);
                    return false;
                }
#endif

                if (esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle)) {
                    if (update_handle != 0) {
                        esp_ota_abort(update_handle);
                    }
                    ESP_LOGE(TAG, "Failed to begin OTA");
                    http->Close();
                    heap_caps_free(buffer);
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
                    psa_hash_abort(&sha256_operation);
#endif
                    return false;
                }

                image_header_checked = true;
                std::string().swap(image_header);
            }
        }

        // Write to flash when buffer is full (4KB) or it's the last chunk
        bool is_last_chunk = (ret == 0);
        if (image_header_checked &&
            (buffer_offset == PAGE_SIZE || (is_last_chunk && buffer_offset > 0))) {
            auto err = esp_ota_write(update_handle, buffer, buffer_offset);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to write OTA data: %s", esp_err_to_name(err));
                esp_ota_abort(update_handle);
                http->Close();
                heap_caps_free(buffer);
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
                psa_hash_abort(&sha256_operation);
#endif
                return false;
            }

            buffer_offset = 0;
        }

        if (is_last_chunk) {
            break;
        }
    }
    http->Close();
    heap_caps_free(buffer);

    if (!image_header_checked || update_handle == 0 || total_read != content_length) {
        ESP_LOGE(TAG, "Firmware download was incomplete");
        if (update_handle != 0) {
            esp_ota_abort(update_handle);
        }
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        psa_hash_abort(&sha256_operation);
#endif
        return false;
    }

#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    std::array<uint8_t, 32> actual_sha256{};
    size_t actual_sha256_length = 0;
    const psa_status_t sha256_result =
        psa_hash_finish(&sha256_operation, actual_sha256.data(), actual_sha256.size(),
                        &actual_sha256_length);
    if (sha256_result != PSA_SUCCESS || actual_sha256_length != actual_sha256.size() ||
        actual_sha256 != expected_sha256) {
        ESP_LOGE(TAG, "Downloaded firmware SHA-256 did not match its approved URL");
        esp_ota_abort(update_handle);
        return false;
    }
#endif

    esp_err_t err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        } else {
            ESP_LOGE(TAG, "Failed to end OTA: %s", esp_err_to_name(err));
        }
        return false;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Firmware upgrade successful");
    return true;
}

bool Ota::StartUpgrade(std::function<void(int progress, size_t speed)> callback) {
    return Upgrade(firmware_url_, callback);
}


bool Ota::ParseVersion(const std::string& version, std::vector<int>& components) {
    components.clear();
    if (version.empty() || version.size() > 31 || version.front() == '.' ||
        version.back() == '.') {
        return false;
    }
    std::stringstream ss(version);
    std::string segment;

    while (std::getline(ss, segment, '.')) {
        if (segment.empty() || segment.size() > 5 || components.size() >= 3 ||
            (segment.size() > 1 && segment.front() == '0') ||
            !std::all_of(segment.begin(), segment.end(), [](unsigned char character) {
                return std::isdigit(character);
            })) {
            components.clear();
            return false;
        }
        uint32_t value = 0;
        for (char character : segment) {
            value = value * 10 + static_cast<uint32_t>(character - '0');
        }
        if (value > UINT16_MAX) {
            components.clear();
            return false;
        }
        components.push_back(static_cast<int>(value));
    }
    return components.size() == 3;
}

bool Ota::IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion) {
    std::vector<int> current;
    std::vector<int> newer;
    if (!ParseVersion(currentVersion, current) || !ParseVersion(newVersion, newer)) {
        ESP_LOGE(TAG, "Refusing malformed firmware version");
        return false;
    }
    
    for (size_t i = 0; i < std::min(current.size(), newer.size()); ++i) {
        if (newer[i] > current[i]) {
            return true;
        } else if (newer[i] < current[i]) {
            return false;
        }
    }
    
    return false;
}

std::string Ota::GetActivationPayload() {
    if (!has_serial_number_) {
        return "{}";
    }

    std::string hmac_hex;
#ifdef SOC_HMAC_SUPPORTED
    uint8_t hmac_result[32]; // SHA-256 输出为32字节
    
    // 使用Key0计算HMAC
    esp_err_t ret = esp_hmac_calculate(HMAC_KEY0, (uint8_t*)activation_challenge_.data(), activation_challenge_.size(), hmac_result);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HMAC calculation failed: %s", esp_err_to_name(ret));
        return "{}";
    }

    for (size_t i = 0; i < sizeof(hmac_result); i++) {
        char buffer[3];
        sprintf(buffer, "%02x", hmac_result[i]);
        hmac_hex += buffer;
    }
#endif

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "algorithm", "hmac-sha256");
    cJSON_AddStringToObject(payload, "serial_number", serial_number_.c_str());
    cJSON_AddStringToObject(payload, "challenge", activation_challenge_.c_str());
    cJSON_AddStringToObject(payload, "hmac", hmac_hex.c_str());
    auto json_str = cJSON_PrintUnformatted(payload);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(payload);

    ESP_LOGI(TAG, "Activation payload: %s", json.c_str());
    return json;
}

esp_err_t Ota::Activate() {
    if (!has_activation_challenge_) {
        ESP_LOGW(TAG, "No activation challenge found");
        return ESP_FAIL;
    }

    std::string url = GetCheckVersionUrl();
    if (url.back() != '/') {
        url += "/activate";
    } else {
        url += "activate";
    }

    auto http = SetupHttp();
    if (http == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    std::string data = GetActivationPayload();
    http->SetContent(std::move(data));

    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return ESP_FAIL;
    }
    
    auto status_code = http->GetStatusCode();
    if (status_code == 202) {
        return ESP_ERR_TIMEOUT;
    }
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to activate, code: %d, body: %s", status_code, http->ReadAll().c_str());
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Activation successful");
    return ESP_OK;
}
