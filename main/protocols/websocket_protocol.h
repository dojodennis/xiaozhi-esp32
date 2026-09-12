#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_

#include "protocol.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <web_socket.h>

#include <atomic>
#include <cstdint>
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
#include "provisions_output_fence_runtime.h"
class ProvisionsVoiceClosures;
#endif
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
#include "provisions_voice_recorder.h"
#include "provisions_websocket.h"
#endif

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class WebsocketProtocol : public Protocol, public std::enable_shared_from_this<WebsocketProtocol> {
public:
    WebsocketProtocol();
    ~WebsocketProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;

#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
    // Install once, before the first connection. Root retains Store/Physical.
    bool BindOutputFenceRuntime(
        const std::shared_ptr<provisions::output_fence::OutputFenceRuntime>& runtime);
    bool BindVoiceClosureHandler(const std::shared_ptr<ProvisionsVoiceClosures>& handler);
#endif

#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    bool SendGatewayHeartbeat();
    bool IsGatewayHeartbeatExpired() const;
#endif
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
    bool TimersNegotiated() const { return timers_enabled_.load() && IsAudioChannelOpened(); }
    bool SendTimerReceipt(const std::string& text) { return TimersNegotiated() && SendText(text); }
    bool DictationNegotiated() const { return dictation_enabled_.load() && IsAudioChannelOpened(); }
    bool SendDictationControl(const std::string& text) {
        return DictationNegotiated() && SendText(text);
    }
    bool GetCaptureContext(provisions::VoiceContext& context) const;
    bool AcceptCaptureContext(const provisions::VoiceContext& context, bool reassignment = false);
    // Run on the application's bounded network task, never the button/audio task.
    bool SendStoredRecording(const provisions::VoiceReplay& replay, bool deferred,
                             const std::function<bool()>& current, bool alarm_stop = false);
    void InterruptStoredRecording();
    bool IsTransportBusy() const { return operation_owner_.load() != nullptr; }
#endif

private:
#if CONFIG_PROVISIONS_OUTPUT_FENCE_V1
    friend class provisions::output_fence::OutputFenceRuntime;
    using FenceDispatch = provisions::output_fence::OutputFenceRuntime::Dispatch;
    std::shared_ptr<provisions::output_fence::OutputFenceRuntime> output_fence_runtime_;
    std::shared_ptr<ProvisionsVoiceClosures> voice_closure_handler_;
    std::atomic<bool> output_fence_selected_{false};
    std::atomic<bool> output_fence_endpoint_allowed_{false};
    std::atomic<uint64_t> output_fence_authentication_generation_{0};
    bool OutputFenceContextCurrent(const FenceDispatch& dispatch) const;
    bool SendOutputFenceReply(const FenceDispatch& dispatch, const std::string& text);
    void HandleOutputFenceFrame(const char* data, size_t size,
                                const std::shared_ptr<ProvisionsWebSocket>& original,
                                uint32_t generation);
#endif
    EventGroupHandle_t event_group_handle_;
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
    using Connection = ProvisionsWebSocket;
#else
    using Connection = WebSocket;
#endif
    std::shared_ptr<Connection> websocket_;
    int version_ = 1;
    std::atomic<uint32_t> connection_generation_{0};
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
    std::atomic<TaskHandle_t> operation_owner_{nullptr};
    std::atomic<bool> close_requested_{false};
    std::atomic<bool> upload_active_{false};
    std::atomic<bool> capture_enabled_{false};
    std::atomic<bool> timers_enabled_{false};
    std::atomic<bool> dictation_enabled_{false};
    mutable std::mutex capture_context_mutex_;
    provisions::VoiceContext capture_context_{};
    bool BeginOperation();
    void EndOperation();
    bool OpenAudioChannelImpl();
#endif
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    std::atomic<bool> gateway_authenticated_{false};
    std::atomic<bool> gateway_hello_pending_{false};
    std::atomic<int64_t> last_gateway_activity_us_{0};
#endif

    void ParseServerHello(const cJSON* root);
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    void RejectServerHello(const char* message);
#endif
    bool SendText(const std::string& text) override;
    std::string GetHelloMessage();
};

#endif
