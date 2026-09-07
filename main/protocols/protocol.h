#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "sdkconfig.h"

#include <cJSON.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
#include "provisions_reply_turn.h"
#endif

struct AudioStreamPacket {
    int sample_rate = 0;
    int frame_duration = 0;
    uint32_t voice_upload_generation = 0;
    uint32_t timestamp = 0;
    uint32_t playback_id = 0;
    uint32_t media_position_ms = 0;
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
    // Immutable receive context; a delayed old binary callback cannot attach to
    // a newer alarm after transport replacement.
    std::string source_session_id;
#endif
    std::vector<uint8_t> payload;
};

struct BinaryProtocol2 {
    uint16_t version;
    uint16_t type;          // Message type (0: OPUS, 1: JSON)
    uint32_t reserved;      // Reserved for future use
    uint32_t timestamp;     // Timestamp in milliseconds (used for server-side AEC)
    uint32_t payload_size;  // Payload size in bytes
    uint8_t payload[];      // Payload data
} __attribute__((packed));

struct BinaryProtocol3 {
    uint8_t type;
    uint8_t reserved;
    uint16_t payload_size;
    uint8_t payload[];
} __attribute__((packed));

enum AbortReason { kAbortReasonNone, kAbortReasonWakeWordDetected };

enum ListeningMode {
    kListeningModeAutoStop,
    kListeningModeManualStop,
    kListeningModeRealtime  // 需要 AEC 支持
};

class Protocol {
public:
    virtual ~Protocol() = default;

    inline int server_sample_rate() const { return server_sample_rate_; }
    inline int server_frame_duration() const { return server_frame_duration_; }
    inline std::string session_id() const {
        std::lock_guard<std::mutex> lock(session_mutex_);
        return session_id_;
    }
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    uint32_t voice_turn_id() const { return voice_turn_.id(); }
    bool IsCurrentVoiceTurn(uint32_t id) const { return voice_turn_.IsCurrent(id); }
    void InvalidateVoiceReply() { voice_turn_.Invalidate(); }
#endif

    void OnIncomingAudio(std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback);
    void OnIncomingJson(std::function<void(const cJSON* root)> callback);
    void OnAudioChannelOpened(std::function<void()> callback);
    void OnAudioChannelClosed(std::function<void()> callback);
    void OnNetworkError(std::function<void(const std::string& message)> callback);
    void OnConnected(std::function<void()> callback);
    void OnDisconnected(std::function<void()> callback);

    virtual bool Start() = 0;
    virtual bool OpenAudioChannel() = 0;
    virtual void CloseAudioChannel(bool send_goodbye = true) = 0;
    virtual bool IsAudioChannelOpened() const = 0;
    virtual bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) = 0;
    virtual void SendWakeWordDetected(const std::string& wake_word);
    virtual void SendStartListening(ListeningMode mode);
    virtual void SendStopListening();
    virtual void SendAbortSpeaking(AbortReason reason);
    virtual void SendMcpMessage(const std::string& message);

protected:
    std::function<void(const cJSON* root)> on_incoming_json_;
    std::function<void(std::unique_ptr<AudioStreamPacket> packet)> on_incoming_audio_;
    std::function<void()> on_audio_channel_opened_;
    std::function<void()> on_audio_channel_closed_;
    std::function<void(const std::string& message)> on_network_error_;
    std::function<void()> on_connected_;
    std::function<void()> on_disconnected_;

    int server_sample_rate_ = 24000;
    int server_frame_duration_ = 60;
    std::atomic<bool> error_occurred_{false};
    mutable std::mutex session_mutex_;
    std::string session_id_;
    void SetSessionId(std::string session) {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_id_ = std::move(session);
    }
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    ProvisionsReplyTurn voice_turn_;
#endif
    std::chrono::time_point<std::chrono::steady_clock> last_incoming_time_;

    virtual bool SendText(const std::string& text) = 0;
    virtual void SetError(const std::string& message);
    virtual bool IsTimeout() const;
    static void AddTextFontCapabilities(cJSON* root);
};

#endif  // PROTOCOL_H
