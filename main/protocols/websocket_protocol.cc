#include "websocket_protocol.h"
#include "application.h"
#include "board.h"
#include "settings.h"
#include "system_info.h"
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
#include "provisions_endpoint_policy.h"
#endif

#include <esp_log.h>
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
#include <esp_app_desc.h>
#include <esp_timer.h>
#endif
#include <arpa/inet.h>
#include <cJSON.h>
#include <cstring>
#include "assets/lang_config.h"

#define TAG "WS"

WebsocketProtocol::WebsocketProtocol() { event_group_handle_ = xEventGroupCreate(); }

WebsocketProtocol::~WebsocketProtocol() {
    connection_generation_.fetch_add(1);
    websocket_.reset();
    vEventGroupDelete(event_group_handle_);
}

bool WebsocketProtocol::Start() {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    return OpenAudioChannel();
#else
    // Only connect to server when audio channel is needed
    return true;
#endif
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (version_ == 2) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol2) + packet->payload.size());
        auto bp2 = (BinaryProtocol2*)serialized.data();
        bp2->version = htons(version_);
        bp2->type = 0;
        bp2->reserved = 0;
        bp2->timestamp = htonl(packet->timestamp);
        bp2->payload_size = htonl(packet->payload.size());
        memcpy(bp2->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else if (version_ == 3) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol3) + packet->payload.size());
        auto bp3 = (BinaryProtocol3*)serialized.data();
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons(packet->payload.size());
        memcpy(bp3->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else {
        return websocket_->Send(packet->payload.data(), packet->payload.size(), true);
    }
}

bool WebsocketProtocol::SendText(const std::string& text) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (!websocket_->Send(text)) {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        ESP_LOGE(TAG, "Failed to send text frame");
#else
        ESP_LOGE(TAG, "Failed to send text: %s", text.c_str());
#endif
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    return true;
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    return websocket_ != nullptr && websocket_->IsConnected() && gateway_authenticated_.load() &&
           !error_occurred_ && !IsGatewayHeartbeatExpired();
#else
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
#endif
}

void WebsocketProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;  // Websocket doesn't need to send goodbye message
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    gateway_authenticated_.store(false);
    if (gateway_hello_pending_.exchange(false)) {
        xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
    }
#endif
    websocket_.reset();
}

#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
bool WebsocketProtocol::SendGatewayHeartbeat() {
    if (!IsAudioChannelOpened() || session_id_.empty()) {
        return false;
    }
    return SendText("{\"session_id\":\"" + session_id_ + "\",\"type\":\"ping\"}");
}

bool WebsocketProtocol::IsGatewayHeartbeatExpired() const {
    constexpr int64_t kHeartbeatTimeoutUs = 45 * 1000 * 1000;
    const int64_t last_activity_us = last_gateway_activity_us_.load();
    return gateway_authenticated_.load() &&
           (last_activity_us <= 0 || esp_timer_get_time() - last_activity_us > kHeartbeatTimeoutUs);
}
#endif

bool WebsocketProtocol::OpenAudioChannel() {
    const uint32_t connection_generation = connection_generation_.fetch_add(1) + 1;
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    std::string url = ProvisionsEndpointPolicy::WebsocketUrl();
    Settings provisions_settings("provisions", false);
    std::string token = provisions_settings.GetString("device_token");
    version_ = 1;
    gateway_authenticated_.store(false);
    gateway_hello_pending_.store(false);
    last_gateway_activity_us_.store(0);
    session_id_.clear();
    xEventGroupClearBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
    if (!ProvisionsEndpointPolicy::IsAllowedWebsocketUrl(url)) {
        ESP_LOGE(TAG, "Compiled Provisions WebSocket endpoint was rejected");
        SetError("Invalid Provisions gateway endpoint");
        return false;
    }
    if (!ProvisionsEndpointPolicy::IsValidDeviceToken(token)) {
        ESP_LOGE(TAG, "Provisions device credential is missing or malformed");
        SetError("Device is not provisioned");
        return false;
    }
#else
    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    std::string token = settings.GetString("token");
    int version = settings.GetInt("version");
    if (version != 0) {
        version_ = version;
    }
#endif

    error_occurred_ = false;

    auto network = Board::GetInstance().GetNetwork();
    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
#endif
        return false;
    }

#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    token = "Bearer " + token;
    websocket_->SetHeader("Authorization", token.c_str());
#else
    if (!token.empty()) {
        // If token not has a space, add "Bearer " prefix
        if (token.find(" ") == std::string::npos) {
            token = "Bearer " + token;
        }
        websocket_->SetHeader("Authorization", token.c_str());
    }
#endif
    websocket_->SetHeader("Protocol-Version", std::to_string(version_).c_str());
    websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    websocket_->SetHeader("X-Provisions-Boot-Id", SystemInfo::GetBootId().c_str());
    websocket_->SetHeader("X-Provisions-Firmware-Version", esp_app_get_description()->version);
#endif

    websocket_->OnData([this, connection_generation](const char* data, size_t len, bool binary) {
        if (connection_generation != connection_generation_.load()) {
            return;
        }
        if (binary) {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
            if (!gateway_authenticated_.load()) {
                ESP_LOGW(TAG, "Ignoring audio received before gateway authentication");
                return;
            }
#endif
            if (on_incoming_audio_ != nullptr) {
                if (version_ == 2) {
                    BinaryProtocol2* bp2 = (BinaryProtocol2*)data;
                    bp2->version = ntohs(bp2->version);
                    bp2->type = ntohs(bp2->type);
                    bp2->timestamp = ntohl(bp2->timestamp);
                    bp2->payload_size = ntohl(bp2->payload_size);
                    auto payload = (uint8_t*)bp2->payload;
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = bp2->timestamp,
                        .payload = std::vector<uint8_t>(payload, payload + bp2->payload_size)}));
                } else if (version_ == 3) {
                    BinaryProtocol3* bp3 = (BinaryProtocol3*)data;
                    bp3->type = bp3->type;
                    bp3->payload_size = ntohs(bp3->payload_size);
                    auto payload = (uint8_t*)bp3->payload;
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>(payload, payload + bp3->payload_size)}));
                } else {
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + len)}));
                }
            }
        } else {
            // Parse JSON data
            auto root = cJSON_ParseWithLength(data, len);
            if (root == nullptr) {
                ESP_LOGE(TAG, "Invalid JSON message");
                return;
            }
            auto type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type)) {
                if (strcmp(type->valuestring, "hello") == 0) {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
                    if (gateway_authenticated_.load()) {
                        gateway_authenticated_.store(false);
                        SetError("Unexpected gateway hello");
                        cJSON_Delete(root);
                        return;
                    }
#endif
                    ParseServerHello(root);
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
                } else if (!gateway_authenticated_.load()) {
                    RejectServerHello("Expected authenticated gateway hello");
                    cJSON_Delete(root);
                    return;
                } else {
                    auto message_session = cJSON_GetObjectItem(root, "session_id");
                    if (!cJSON_IsString(message_session) ||
                        session_id_ != message_session->valuestring) {
                        gateway_authenticated_.store(false);
                        SetError("Invalid gateway message session");
                        cJSON_Delete(root);
                        return;
                    }
                    if (strcmp(type->valuestring, "pong") == 0) {
                        if (cJSON_GetArraySize(root) != 2) {
                            gateway_authenticated_.store(false);
                            SetError("Invalid gateway pong");
                            cJSON_Delete(root);
                            return;
                        }
                    } else if (on_incoming_json_ != nullptr) {
                        on_incoming_json_(root);
                    }
#endif
#if !CONFIG_PROVISIONS_GATEWAY_REQUIRED
                } else {
                    if (on_incoming_json_ != nullptr) {
                        on_incoming_json_(root);
                    }
#endif
                }
            } else {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
                gateway_authenticated_.store(false);
                ESP_LOGE(TAG, "Rejecting gateway message without a type");
                SetError("Invalid gateway message");
#else
                ESP_LOGE(TAG, "Missing message type, data: %s", std::string(data, len).c_str());
#endif
            }
            cJSON_Delete(root);
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        if (gateway_authenticated_.load()) {
            last_gateway_activity_us_.store(esp_timer_get_time());
        }
#endif
    });

    websocket_->OnDisconnected([this, connection_generation]() {
        if (connection_generation != connection_generation_.load()) {
            return;
        }
        ESP_LOGI(TAG, "Websocket disconnected");
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        gateway_authenticated_.store(false);
        if (gateway_hello_pending_.exchange(false)) {
            SetError(Lang::Strings::SERVER_NOT_CONNECTED);
            xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
        }
#endif
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });

#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    gateway_hello_pending_.store(true);
#endif
    ESP_LOGI(TAG, "Connecting to websocket server: %s with version: %d", url.c_str(), version_);
    if (!websocket_->Connect(url.c_str())) {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        gateway_hello_pending_.store(false);
#endif
        ESP_LOGE(TAG, "Failed to connect to websocket server, code=%d", websocket_->GetLastError());
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    // Send hello message to describe the client
    auto message = GetHelloMessage();
    if (!SendText(message)) {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        gateway_hello_pending_.store(false);
#endif
        return false;
    }

    // Wait for server hello
    EventBits_t bits =
        xEventGroupWaitBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE,
                            pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT)) {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        gateway_hello_pending_.store(false);
#endif
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }
    if (error_occurred_
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        || !gateway_authenticated_.load() || websocket_ == nullptr || !websocket_->IsConnected()
#endif
    ) {
        return false;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    if (on_connected_ != nullptr) {
        on_connected_();
    }
#endif

    return true;
}

std::string WebsocketProtocol::GetHelloMessage() {
    // keys: message type, version, audio_params (format, sample_rate, channels)
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version_);
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    cJSON_AddBoolToObject(features, "mcp", false);
#else
    cJSON_AddBoolToObject(features, "mcp", true);
#endif
    cJSON_AddItemToObject(root, "features", features);
    AddTextFontCapabilities(root);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void WebsocketProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (!cJSON_IsString(transport) || strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGE(TAG, "Server hello did not select the WebSocket transport");
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        RejectServerHello("Invalid gateway transport");
#endif
        return;
    }

#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    auto version = cJSON_GetObjectItem(root, "version");
    auto provisions = cJSON_GetObjectItem(root, "provisions");
    auto authenticated = cJSON_IsObject(provisions)
                             ? cJSON_GetObjectItem(provisions, "authenticated")
                             : nullptr;
    if (!cJSON_IsNumber(version) || version->valuedouble != 1 || !cJSON_IsTrue(authenticated)) {
        ESP_LOGE(TAG, "Provisions gateway rejected protocol or device authentication");
        RejectServerHello("Device authentication failed");
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (!cJSON_IsString(session_id) ||
        !ProvisionsEndpointPolicy::IsCanonicalUuid(session_id->valuestring)) {
        ESP_LOGE(TAG, "Provisions server hello has an invalid session UUID");
        RejectServerHello("Invalid gateway session");
        return;
    }

    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    auto format = cJSON_IsObject(audio_params) ? cJSON_GetObjectItem(audio_params, "format") : nullptr;
    auto sample_rate =
        cJSON_IsObject(audio_params) ? cJSON_GetObjectItem(audio_params, "sample_rate") : nullptr;
    auto channels =
        cJSON_IsObject(audio_params) ? cJSON_GetObjectItem(audio_params, "channels") : nullptr;
    auto frame_duration =
        cJSON_IsObject(audio_params) ? cJSON_GetObjectItem(audio_params, "frame_duration") : nullptr;
    if (!cJSON_IsString(format) || strcmp(format->valuestring, "opus") != 0 ||
        !cJSON_IsNumber(sample_rate) || sample_rate->valuedouble != 24000 ||
        !cJSON_IsNumber(channels) || channels->valuedouble != 1 ||
        !cJSON_IsNumber(frame_duration) || frame_duration->valuedouble != 60) {
        ESP_LOGE(TAG, "Provisions server hello has unapproved audio parameters");
        RejectServerHello("Invalid gateway audio parameters");
        return;
    }

    session_id_ = session_id->valuestring;
    server_sample_rate_ = sample_rate->valueint;
    server_frame_duration_ = frame_duration->valueint;
    gateway_authenticated_.store(true);
    gateway_hello_pending_.store(false);
    last_gateway_activity_us_.store(esp_timer_get_time());
    ESP_LOGI(TAG, "Authenticated Provisions gateway session");
#else
    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }
#endif

    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}

#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
void WebsocketProtocol::RejectServerHello(const char* message) {
    gateway_authenticated_.store(false);
    gateway_hello_pending_.store(false);
    SetError(message);
    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}
#endif
