#ifndef PROVISIONS_OUTPUT_FENCE_RUNTIME_H
#define PROVISIONS_OUTPUT_FENCE_RUNTIME_H

#include "provisions_output_fence.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <array>
#include <atomic>
#include <memory>

class WebsocketProtocol;
class ProvisionsWebSocket;

namespace provisions::output_fence {

// Application owns Store/Physical until Stopped(). Only the actual WebSocket
// callback can mint a Dispatch; no public DTO or authority Boolean admits work.
class OutputFenceRuntime final : private AbortAuthority,
                                 public std::enable_shared_from_this<OutputFenceRuntime> {
public:
    static std::shared_ptr<OutputFenceRuntime> Create(Store& store, Physical& physical,
                                                     std::string device_id);
    bool Start();
    void Stop();
    bool Stopped() const;
    ReadinessSnapshot Snapshot() const;

private:
    friend class ::WebsocketProtocol;
    struct Dispatch final {
        Dispatch(std::shared_ptr<WebsocketProtocol> protocol,
                 std::shared_ptr<ProvisionsWebSocket> socket, uint32_t connection_generation,
                 uint64_t authentication_generation, std::string session, std::string boot,
                 std::string_view raw, Message message);
        Dispatch(const Dispatch&) = delete;
        Dispatch& operator=(const Dispatch&) = delete;
        Dispatch(Dispatch&&) = default;
        Dispatch& operator=(Dispatch&&) = delete;
        const std::shared_ptr<WebsocketProtocol> protocol;
        const std::shared_ptr<ProvisionsWebSocket> socket;
        const uint32_t connection_generation;
        const uint64_t authentication_generation;
        const std::string session;
        const std::string boot;
        std::array<char, 2048> raw{};
        const size_t size;
        const Message message;
        bool Same(const Dispatch& other) const;
        bool Current() const;
    };

    OutputFenceRuntime(Store& store, Physical& physical, std::string device_id);
    bool Submit(std::unique_ptr<Dispatch> dispatch);
    bool Allows(const Identity& identity, const PhoneReceipt& receipt) override;
    static void Worker(void* argument);
    void Run();

    Store& store_;
    Physical& physical_;
    const std::string device_id_;
    mutable std::mutex mutex_;
    ReadinessSnapshot snapshot_{};
    TaskHandle_t task_ = nullptr;
    bool started_ = false;
    bool hydrated_ = false;
    bool stopped_ = false;
    std::atomic<bool> stopping_{false};
    std::unique_ptr<Dispatch> pending_;
    std::unique_ptr<Dispatch> active_;
    bool allowance_open_ = false;  // Worker-only, valid for exactly one Core call.
};
}  // namespace provisions::output_fence
#endif
