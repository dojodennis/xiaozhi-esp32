#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_


#include "protocol.h"

#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <atomic>
#include <cstdint>

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class WebsocketProtocol : public Protocol {
public:
    WebsocketProtocol();
    ~WebsocketProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;

#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    bool SendGatewayHeartbeat();
    bool IsGatewayHeartbeatExpired() const;
#endif

private:
    EventGroupHandle_t event_group_handle_;
    std::unique_ptr<WebSocket> websocket_;
    int version_ = 1;
    std::atomic<uint32_t> connection_generation_{0};
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
