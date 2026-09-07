#include "websocket_protocol.h"
#include "application.h"
#include "board.h"
#include "settings.h"
#include "system_info.h"
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
#include <new>
#endif
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
#include "provisions_endpoint_policy.h"
#include "provisions_json_guard.h"
#endif
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
#include "provisions_timers.h"
#include "provisions_voice_wire.h"
#endif

#include <esp_log.h>
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
#include <esp_app_desc.h>
#include <esp_timer.h>
#endif
#include <arpa/inet.h>
#include <cJSON.h>
#include <cstring>
#include <string_view>
#include "assets/lang_config.h"

#define TAG "WS"

#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
namespace {
constexpr size_t kMaximumProvisionsTextFrameBytes = 32 * 1024;
}
#endif

namespace {
// Check every decoded type key, including duplicates. A reserved command never
// falls through to the generic JSON/App path, even when strict admission fails.
bool ReservedOutputFence(const cJSON* root) {
    const cJSON* item;
    cJSON_ArrayForEach(item, root) {
        if (item->string && strcasecmp(item->string, "type") == 0 && cJSON_IsString(item) &&
            std::string_view(item->valuestring).find("output_fence_v1") == 0)
            return true;
    }
    return false;
}
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
bool UniqueFenceField(const cJSON* object, const char* key) {
    unsigned count = 0;
    const cJSON* item;
    cJSON_ArrayForEach(item, object) {
        if (item->string && strcasecmp(item->string, key) == 0)
            ++count;
    }
    return count == 1 && cJSON_GetObjectItemCaseSensitive(object, key) != nullptr;
}
#endif
}  // namespace

#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
bool WebsocketProtocol::BindOutputFenceRuntime(
    const std::shared_ptr<provisions::output_fence::OutputFenceRuntime>& runtime) {
    if (!runtime || connection_generation_.load() != 0)
        return false;
    std::shared_ptr<provisions::output_fence::OutputFenceRuntime> empty;
    return std::atomic_compare_exchange_strong(&output_fence_runtime_, &empty, runtime);
}
bool WebsocketProtocol::BindVoiceClosureHandler(
    const std::shared_ptr<ProvisionsVoiceClosures>& handler) {
    if (!handler || connection_generation_.load() != 0)
        return false;
    std::shared_ptr<ProvisionsVoiceClosures> empty;
    return std::atomic_compare_exchange_strong(&voice_closure_handler_, &empty, handler);
}
bool WebsocketProtocol::OutputFenceContextCurrent(const FenceDispatch& dispatch) const {
    return dispatch.protocol.get() == this && output_fence_endpoint_allowed_.load() &&
           output_fence_selected_.load() && dispatch.authentication_generation != 0 &&
           dispatch.authentication_generation == output_fence_authentication_generation_.load() &&
           dispatch.connection_generation == connection_generation_.load() &&
           dispatch.socket == std::atomic_load(&websocket_) &&
           dispatch.session == session_id() && dispatch.boot == SystemInfo::GetBootId() &&
           IsAudioChannelOpened();
}
bool WebsocketProtocol::SendOutputFenceReply(const FenceDispatch& dispatch,
                                           const std::string& text) {
    // Captured transport only. This bounded queue submission proves neither
    // delivery nor physical closure, and never reloads a replacement socket.
    return !text.empty() && text.size() <= 2048 && OutputFenceContextCurrent(dispatch) &&
           dispatch.socket->SendAsync(text);
}
void WebsocketProtocol::HandleOutputFenceFrame(
    const char* data, size_t size, const std::shared_ptr<ProvisionsWebSocket>& original,
    uint32_t generation) {
    using namespace provisions::output_fence;
    Message message;
    if (!data || !original || size == 0 || size > 2048 ||
        !ParseMessage(std::string_view(data, size), message))
        return;
    auto owner = weak_from_this().lock();
    auto runtime = std::atomic_load(&output_fence_runtime_);
    if (!owner || !runtime)
        return;
    // No caller-supplied transport/session context and no reconstruction of the
    // grant's original device_connection_id from this fresh recovery socket.
    auto dispatch = std::unique_ptr<FenceDispatch>(new (std::nothrow) FenceDispatch(
        std::move(owner), original, generation, output_fence_authentication_generation_.load(),
        session_id(), SystemInfo::GetBootId(), std::string_view(data, size), std::move(message)));
    runtime->Submit(std::move(dispatch));
}
#endif

WebsocketProtocol::WebsocketProtocol() { event_group_handle_ = xEventGroupCreate(); }

WebsocketProtocol::~WebsocketProtocol() {
    connection_generation_.fetch_add(1);
    std::atomic_store(&websocket_, std::shared_ptr<Connection>{});
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
    const auto websocket = std::atomic_load(&websocket_);
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
    const auto owner = operation_owner_.load();
    if (owner != nullptr && owner != xTaskGetCurrentTaskHandle())
        return false;
#endif
    if (websocket == nullptr || !websocket->IsConnected()) {
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

        return websocket->Send(serialized.data(), serialized.size(), true);
    } else if (version_ == 3) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol3) + packet->payload.size());
        auto bp3 = (BinaryProtocol3*)serialized.data();
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons(packet->payload.size());
        memcpy(bp3->payload, packet->payload.data(), packet->payload.size());

        return websocket->Send(serialized.data(), serialized.size(), true);
    } else {
        return websocket->Send(packet->payload.data(), packet->payload.size(), true);
    }
}

bool WebsocketProtocol::SendText(const std::string& text) {
    const auto websocket = std::atomic_load(&websocket_);
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
    const auto owner = operation_owner_.load();
    if (owner != nullptr && owner != xTaskGetCurrentTaskHandle())
        return false;
#endif
    if (websocket == nullptr || !websocket->IsConnected()) {
        return false;
    }

#if CONFIG_PROVISIONS_LOCAL_CAPTURE
    // Main-task heartbeats/abort controls only enter a bounded queue. Bulk
    // upload and handshake callers wait for the I/O worker's actual result.
    const bool sent = owner == nullptr ? websocket->SendAsync(text) : websocket->Send(text);
#else
    const bool sent = websocket->Send(text);
#endif
    if (!sent) {
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
    const auto websocket = std::atomic_load(&websocket_);
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    return gateway_authenticated_.load() && websocket != nullptr && websocket->IsConnected() &&
           !error_occurred_ && !IsGatewayHeartbeatExpired();
#else
    return websocket != nullptr && websocket->IsConnected() && !error_occurred_ && !IsTimeout();
#endif
}

void WebsocketProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;  // Websocket doesn't need to send goodbye message
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    gateway_authenticated_.store(false);
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
    timers_enabled_.store(false);
    dictation_enabled_.store(false);
#endif
    connection_generation_.fetch_add(1);
    if (gateway_hello_pending_.exchange(false)) {
        xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
    }
#endif
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
    capture_enabled_.store(false);
    close_requested_.store(true);
    if (auto websocket = std::atomic_load(&websocket_))
        websocket->Close();
#else
    std::atomic_store(&websocket_, std::shared_ptr<Connection>{});
#endif
}

#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
bool WebsocketProtocol::SendGatewayHeartbeat() {
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
    if (IsTransportBusy())
        return true;
#endif
    if (!IsAudioChannelOpened() || this->session_id().empty()) {
        return false;
    }
    return SendText("{\"session_id\":\"" + this->session_id() + "\",\"type\":\"ping\"}");
}

bool WebsocketProtocol::IsGatewayHeartbeatExpired() const {
    constexpr int64_t kHeartbeatTimeoutUs = 45 * 1000 * 1000;
    const int64_t last_activity_us = last_gateway_activity_us_.load();
    return gateway_authenticated_.load() &&
           (last_activity_us <= 0 || esp_timer_get_time() - last_activity_us > kHeartbeatTimeoutUs);
}
#endif

bool WebsocketProtocol::OpenAudioChannel() {
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
    if (!BeginOperation())
        return false;
    if (close_requested_.exchange(false))
        std::atomic_store(&websocket_, std::shared_ptr<Connection>{});
    const bool opened = OpenAudioChannelImpl();
    if (!opened) {
        gateway_authenticated_.store(false);
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
        timers_enabled_.store(false);
        dictation_enabled_.store(false);
#endif
        capture_enabled_.store(false);
    }
    EndOperation();
    return opened && IsAudioChannelOpened();
}

bool WebsocketProtocol::OpenAudioChannelImpl() {
    capture_enabled_.store(false);
    if (close_requested_.load())
        return false;
#endif
    const uint32_t connection_generation = connection_generation_.fetch_add(1) + 1;
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
    output_fence_selected_.store(false);
    output_fence_endpoint_allowed_.store(false);
#endif
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    std::string url = ProvisionsEndpointPolicy::WebsocketUrl();
    Settings provisions_settings("provisions", false);
    std::string token = provisions_settings.GetString("device_token");
    version_ = 1;
    gateway_authenticated_.store(false);
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
    timers_enabled_.store(false);
    dictation_enabled_.store(false);
#endif
    gateway_hello_pending_.store(false);
    last_gateway_activity_us_.store(0);
    SetSessionId({});
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

#if CONFIG_PROVISIONS_LOCAL_CAPTURE
    const auto websocket = std::make_shared<Connection>();
#else
    auto network = Board::GetInstance().GetNetwork();
    const std::shared_ptr<Connection> websocket = network->CreateWebSocket(1);
#endif
    std::atomic_store(&websocket_, websocket);
    if (websocket == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
#endif
        return false;
    }

#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    token = "Bearer " + token;
    websocket->SetHeader("Authorization", token.c_str());
#else
    if (!token.empty()) {
        // If token not has a space, add "Bearer " prefix
        if (token.find(" ") == std::string::npos) {
            token = "Bearer " + token;
        }
        websocket->SetHeader("Authorization", token.c_str());
    }
#endif
    websocket->SetHeader("Protocol-Version", std::to_string(version_).c_str());
    websocket->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    websocket->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
    // Both the compiled TLS URL and provisioned device token were checked above.
    output_fence_endpoint_allowed_.store(true);
#endif
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    websocket->SetHeader("X-Provisions-Boot-Id", SystemInfo::GetBootId().c_str());
    websocket->SetHeader("X-Provisions-Firmware-Version", esp_app_get_description()->version);
#endif

    websocket->OnData([this, connection_generation, owner = weak_from_this(),
                       original = std::weak_ptr<Connection>(websocket)](
                          const char* data, size_t len, bool binary) {
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
        const auto keep_alive = owner.lock();
        if (!keep_alive)
            return;
#endif
        if (connection_generation != connection_generation_.load()) {
            return;
        }
        if (binary) {
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
            const auto packet_session = session_id();
            if (connection_generation != connection_generation_.load())
                return;
#endif
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
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
                        .source_session_id = packet_session,
#endif
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
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
                        .source_session_id = packet_session,
#endif
                        .payload = std::vector<uint8_t>(payload, payload + bp3->payload_size)}));
                } else {
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
                        .source_session_id = packet_session,
#endif
                        .payload = std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + len)}));
                }
            }
        } else {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
            // cJSON exposes decoded strings as NUL-terminated buffers. Reject
            // embedded NUL representations before parsing so a sentence such
            // as `safe\u0000hidden` cannot be validated as only its prefix.
            const std::string_view raw_frame(data, len);
            if (
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
                raw_frame.size() > kMaximumProvisionsTextFrameBytes ||
                ProvisionsJsonGuard::ContainsEmbeddedNul(raw_frame) ||
#endif
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
                !::provisions::timers::WithinJsonBudget(raw_frame) ||
#endif
                raw_frame.find('\0') != std::string_view::npos ||
                raw_frame.find("\\u0000") != std::string_view::npos) {
                gateway_authenticated_.store(false);
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
                timers_enabled_.store(false);
                dictation_enabled_.store(false);
#endif
                ESP_LOGE(TAG, "Rejecting gateway JSON containing an embedded NUL");
                SetError("Invalid gateway message");
                return;
            }
#endif
            // Parse exactly one JSON value. cJSON_ParseWithLength() accepts a
            // valid prefix followed by garbage, so retain the parse end and
            // permit only JSON whitespace after the root value.
            const char* parse_end = nullptr;
            auto root = cJSON_ParseWithLengthOpts(data, len, &parse_end, false);
            if (root != nullptr) {
                const char* const frame_end = data + len;
                while (parse_end < frame_end && (*parse_end == ' ' || *parse_end == '\t' ||
                                                 *parse_end == '\r' || *parse_end == '\n')) {
                    ++parse_end;
                }
                if (parse_end != frame_end) {
                    cJSON_Delete(root);
                    root = nullptr;
                }
            }
            if (root == nullptr) {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
                gateway_authenticated_.store(false);
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
                timers_enabled_.store(false);
                dictation_enabled_.store(false);
#endif
                ESP_LOGE(TAG, "Rejecting malformed gateway JSON");
                SetError("Invalid gateway message");
#else
                ESP_LOGE(TAG, "Invalid JSON message");
#endif
                return;
            }
            if (ReservedOutputFence(root)) {
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
                HandleOutputFenceFrame(data, len, original.lock(), connection_generation);
#endif
                cJSON_Delete(root);
                return;
            }
            auto type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type)) {
                if (strcmp(type->valuestring, "hello") == 0) {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
                    if (gateway_authenticated_.load()) {
                        gateway_authenticated_.store(false);
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
                        timers_enabled_.store(false);
                        dictation_enabled_.store(false);
#endif
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
                        this->session_id() != message_session->valuestring) {
                        gateway_authenticated_.store(false);
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
                        timers_enabled_.store(false);
                        dictation_enabled_.store(false);
#endif
                        SetError("Invalid gateway message session");
                        cJSON_Delete(root);
                        return;
                    }
                    if (strcmp(type->valuestring, "pong") == 0) {
                        if (cJSON_GetArraySize(root) != 2) {
                            gateway_authenticated_.store(false);
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
                            timers_enabled_.store(false);
                            dictation_enabled_.store(false);
#endif
                            SetError("Invalid gateway pong");
                            cJSON_Delete(root);
                            return;
                        }
                    } else if (on_incoming_json_ != nullptr) {
                        on_incoming_json_(root);
                    }
#endif
#if !CONFIG_PROVISIONS_GATEWAY_REQUIRED
                }
                else {
                    if (on_incoming_json_ != nullptr) {
                        on_incoming_json_(root);
                    }
#endif
                }
            } else {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
                gateway_authenticated_.store(false);
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
                timers_enabled_.store(false);
                dictation_enabled_.store(false);
#endif
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

    websocket->OnDisconnected([this, connection_generation, owner = weak_from_this()]() {
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
        const auto keep_alive = owner.lock();
        if (!keep_alive)
            return;
#endif
        if (connection_generation != connection_generation_.load()) {
            return;
        }
        ESP_LOGI(TAG, "Websocket disconnected");
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        gateway_authenticated_.store(false);
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
        timers_enabled_.store(false);
        dictation_enabled_.store(false);
#endif
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
    if (!websocket->Connect(url.c_str())) {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        gateway_hello_pending_.store(false);
#endif
        ESP_LOGE(TAG, "Failed to connect to websocket server, code=%d", websocket->GetLastError());
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
        || !gateway_authenticated_.load() || websocket == nullptr || !websocket->IsConnected()
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
    cJSON_AddBoolToObject(features, "turn_ids", true);
    if (Application::GetInstance().HasProvisionsTimerSnapshotConsumer()) {
        cJSON_AddBoolToObject(features, "galley_timer_snapshot_v1", true);
    }
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
    cJSON_AddBoolToObject(features, "audio_capture", true);
    cJSON_AddBoolToObject(features, "audio_retry", true);
    cJSON_AddBoolToObject(features, "timers_v1", true);
    cJSON_AddBoolToObject(features, "timer_claim_recovery_v1", true);
    cJSON_AddBoolToObject(features, "dictation_v1", true);
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
    const auto runtime = std::atomic_load(&output_fence_runtime_);
    if (runtime && std::atomic_load(&voice_closure_handler_)) {
        cJSON_AddBoolToObject(features, "output_fence_v1", true);
        cJSON_AddBoolToObject(features, "output_receipts_v1", true);
        cJSON_AddBoolToObject(features, "capture_closure_v1", true);
        const auto snapshot = runtime->Snapshot();
        auto diagnostic = cJSON_AddObjectToObject(root, "output_fence");
        cJSON_AddNumberToObject(diagnostic, "version", 1);
        using provisions::output_fence::Readiness;
        const char* state = snapshot.state == Readiness::Uncommissioned ? "uncommissioned"
                            : snapshot.state == Readiness::RecoveryRequired ? "recovery_required"
                            : snapshot.state == Readiness::Ready ? "ready" : "blocked";
        cJSON_AddStringToObject(diagnostic, "state", state);
        if (snapshot.fence_epoch)
            cJSON_AddNumberToObject(diagnostic, "fence_epoch", *snapshot.fence_epoch);
        else
            cJSON_AddNullToObject(diagnostic, "fence_epoch");
    }
#endif
#endif
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
    auto authenticated =
        cJSON_IsObject(provisions) ? cJSON_GetObjectItem(provisions, "authenticated") : nullptr;
    auto turn_ids =
        cJSON_IsObject(provisions) ? cJSON_GetObjectItem(provisions, "turn_ids") : nullptr;
    if (!cJSON_IsNumber(version) || version->valuedouble != 1 || !cJSON_IsTrue(authenticated) ||
        !cJSON_IsTrue(turn_ids)) {
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
    auto format =
        cJSON_IsObject(audio_params) ? cJSON_GetObjectItem(audio_params, "format") : nullptr;
    auto sample_rate =
        cJSON_IsObject(audio_params) ? cJSON_GetObjectItem(audio_params, "sample_rate") : nullptr;
    auto channels =
        cJSON_IsObject(audio_params) ? cJSON_GetObjectItem(audio_params, "channels") : nullptr;
    auto frame_duration = cJSON_IsObject(audio_params)
                              ? cJSON_GetObjectItem(audio_params, "frame_duration")
                              : nullptr;
    if (!cJSON_IsString(format) || strcmp(format->valuestring, "opus") != 0 ||
        !cJSON_IsNumber(sample_rate) || sample_rate->valuedouble != 24000 ||
        !cJSON_IsNumber(channels) || channels->valuedouble != 1 ||
        !cJSON_IsNumber(frame_duration) || frame_duration->valuedouble != 60) {
        ESP_LOGE(TAG, "Provisions server hello has unapproved audio parameters");
        RejectServerHello("Invalid gateway audio parameters");
        return;
    }

    SetSessionId(session_id->valuestring);
    server_sample_rate_ = sample_rate->valueint;
    server_frame_duration_ = frame_duration->valueint;
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
    unsigned timer_flags = 0, recovery_flags = 0, dictation_flags = 0;
    const cJSON* feature;
    cJSON_ArrayForEach (feature, provisions) {
        if (feature->string && strcmp(feature->string, "timers_v1") == 0)
            ++timer_flags;
        if (feature->string && strcmp(feature->string, "timer_claim_recovery_v1") == 0)
            ++recovery_flags;
        if (feature->string && strcmp(feature->string, "dictation_v1") == 0)
            ++dictation_flags;
    }
    timers_enabled_.store(
        timer_flags == 1 && recovery_flags == 1 &&
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(provisions, "timers_v1")) &&
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(provisions, "timer_claim_recovery_v1")));
    dictation_enabled_.store(dictation_flags == 1 && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
                                                         provisions, "dictation_v1")));
    auto capture_feature = cJSON_GetObjectItemCaseSensitive(provisions, "audio_capture");
    auto capture_context = cJSON_GetObjectItemCaseSensitive(provisions, "capture_context");
    ::provisions::VoiceContext context;
    if (!cJSON_IsTrue(capture_feature) ||
        !::provisions::ParseVoiceContext(capture_context, context)) {
        RejectServerHello("Local capture is unavailable at this gateway");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(capture_context_mutex_);
        capture_context_ = context;
        capture_enabled_.store(true);
    }
#endif
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
    output_fence_selected_.store(false);
    const auto selected = cJSON_GetObjectItemCaseSensitive(provisions, "output_fence_v1");
    if (selected) {
        bool valid = cJSON_IsBool(selected) && UniqueFenceField(provisions, "output_fence_v1");
        if (cJSON_IsTrue(selected)) {
            for (const auto key : {"type", "transport", "version", "session_id", "provisions",
                                   "audio_params"})
                valid = valid && UniqueFenceField(root, key);
            for (const auto key : {"authenticated", "turn_ids", "audio_capture",
                                   "output_receipts_v1", "capture_closure_v1"})
                valid = valid && UniqueFenceField(provisions, key) &&
                        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(provisions, key));
            valid = valid && UniqueFenceField(provisions, "capture_context") &&
                    output_fence_endpoint_allowed_.load() &&
                    std::atomic_load(&output_fence_runtime_) != nullptr &&
                    std::atomic_load(&voice_closure_handler_) != nullptr;
            const auto generation = output_fence_authentication_generation_.load();
            valid = valid && generation < provisions::output_fence::kMaximumInteger;
            if (valid)
                output_fence_authentication_generation_.store(generation + 1);
        }
        if (!valid) {
            RejectServerHello("Invalid output fence selection");
            return;
        }
        output_fence_selected_.store(cJSON_IsTrue(selected));
    }
#endif
    gateway_authenticated_.store(true);
    gateway_hello_pending_.store(false);
    last_gateway_activity_us_.store(esp_timer_get_time());
    ESP_LOGI(TAG, "Authenticated Provisions gateway session");
#else
    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        SetSessionId(session_id->valuestring);
        ESP_LOGI(TAG, "Session ID: %s", this->session_id().c_str());
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
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
    timers_enabled_.store(false);
    dictation_enabled_.store(false);
#endif
    gateway_hello_pending_.store(false);
    SetError(message);
    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}
#endif

#if CONFIG_PROVISIONS_LOCAL_CAPTURE
void WebsocketProtocol::InterruptStoredRecording() {
    if (upload_active_.load()) {
        if (auto websocket = std::atomic_load(&websocket_))
            websocket->Close();
    }
}
bool WebsocketProtocol::BeginOperation() {
    TaskHandle_t empty = nullptr;
    return operation_owner_.compare_exchange_strong(empty, xTaskGetCurrentTaskHandle());
}
void WebsocketProtocol::EndOperation() {
    if (close_requested_.exchange(false)) {
        gateway_authenticated_.store(false);
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
        timers_enabled_.store(false);
        dictation_enabled_.store(false);
#endif
        capture_enabled_.store(false);
        std::atomic_store(&websocket_, std::shared_ptr<Connection>{});
    }
    operation_owner_.store(nullptr);
}
bool WebsocketProtocol::GetCaptureContext(provisions::VoiceContext& context) const {
    if (!capture_enabled_.load() || !gateway_authenticated_.load())
        return false;
    std::lock_guard<std::mutex> lock(capture_context_mutex_);
    context = capture_context_;
    return true;
}
bool WebsocketProtocol::AcceptCaptureContext(const provisions::VoiceContext& context,
                                             bool reassignment) {
    if (!capture_enabled_.load() || !gateway_authenticated_.load() ||
        !provisions::VoiceRecording::ValidContext(context))
        return false;
    std::lock_guard<std::mutex> lock(capture_context_mutex_);
    if (!reassignment && context.conversation_id != capture_context_.conversation_id)
        return false;
    capture_context_ = context;
    return true;
}
bool WebsocketProtocol::SendStoredRecording(const provisions::VoiceReplay& replay, bool deferred,
                                            const std::function<bool()>& current) {
    if (replay.capture.IsDictation() && (!deferred || !DictationNegotiated()))
        return false;
    if (!BeginOperation())
        return false;
    upload_active_.store(true);
    const auto generation = connection_generation_.load();
    const auto websocket = std::atomic_load(&websocket_);
    provisions::VoiceContext context;
    bool ok = current && current() && IsAudioChannelOpened() && GetCaptureContext(context) &&
              replay.capture.conversation_id == context.conversation_id &&
              replay.bytes <= provisions::VoiceOutbox::kMaxFrameBytes && replay.frames != nullptr &&
              voice_turn_.Begin();
    const uint32_t turn = voice_turn_id();
    const std::string session = this->session_id();
    auto still_current = [&]() {
        return generation == connection_generation_.load() && gateway_authenticated_.load() &&
               current();
    };
    bool started = false;
    if (ok) {
        const auto start = provisions::VoiceCaptureStart(replay, session, turn, deferred);
        ok = !start.empty() && still_current() && SendText(start);
        started = ok;
    }
    size_t offset = 0, count = 0;
    while (ok && offset < replay.bytes) {
        if (offset + 2 > replay.bytes) {
            ok = false;
            break;
        }
        const size_t bytes =
            replay.frames[offset] | (static_cast<size_t>(replay.frames[offset + 1]) << 8);
        offset += 2;
        if (bytes == 0 || bytes > 2048 || bytes > replay.bytes - offset) {
            ok = false;
            break;
        }
        ok = still_current() && websocket && websocket->Send(replay.frames + offset, bytes, true);
        offset += bytes;
        ++count;
    }
    ok = ok && count == replay.capture.packet_count && still_current();
    if (ok) {
        ok = SendText(
            "{\"session_id\":\"" + session +
            "\",\"type\":\"listen\",\"state\":\"stop\",\"turn_id\":" + std::to_string(turn) + "}");
    } else if (started && generation == connection_generation_.load() &&
               gateway_authenticated_.load()) {
        SendText("{\"session_id\":\"" + session + "\",\"type\":\"abort\"}");
    }
    if (!ok)
        voice_turn_.Invalidate();
    upload_active_.store(false);
    EndOperation();
    return ok;
}
#endif
