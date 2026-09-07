#include "provisions_output_fence_runtime.h"

#include "protocols/websocket_protocol.h"

#include <cstring>
#include <new>
#include <utility>

namespace provisions::output_fence {
OutputFenceRuntime::Dispatch::Dispatch(std::shared_ptr<WebsocketProtocol> owner,
                                      std::shared_ptr<ProvisionsWebSocket> connection,
                                      uint32_t generation, uint64_t authentication,
                                      std::string session_id, std::string boot_id,
                                      std::string_view text, Message parsed)
    : protocol(std::move(owner)),
      socket(std::move(connection)),
      connection_generation(generation),
      authentication_generation(authentication),
      session(std::move(session_id)),
      boot(std::move(boot_id)),
      size(text.size()),
      message(std::move(parsed)) {
    if (size <= raw.size())
        std::memcpy(raw.data(), text.data(), size);
}
bool OutputFenceRuntime::Dispatch::Same(const Dispatch& other) const {
    return protocol == other.protocol && socket == other.socket &&
           connection_generation == other.connection_generation &&
           authentication_generation == other.authentication_generation &&
           session == other.session && boot == other.boot && size == other.size &&
           size <= raw.size() && std::memcmp(raw.data(), other.raw.data(), size) == 0;
}
bool OutputFenceRuntime::Dispatch::Current() const {
    return protocol && socket && protocol->OutputFenceContextCurrent(*this);
}
OutputFenceRuntime::OutputFenceRuntime(Store& store, Physical& physical, std::string device_id)
    : store_(store), physical_(physical), device_id_(std::move(device_id)) {}
std::shared_ptr<OutputFenceRuntime> OutputFenceRuntime::Create(Store& store, Physical& physical,
                                                             std::string device_id) {
    if (!ValidId(device_id))
        return {};
    return std::shared_ptr<OutputFenceRuntime>(
        new (std::nothrow) OutputFenceRuntime(store, physical, std::move(device_id)));
}
bool OutputFenceRuntime::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_ || stopping_.load())
        return false;
    started_ = true;
    auto* retained = new (std::nothrow) std::shared_ptr<OutputFenceRuntime>(shared_from_this());
    if (!retained || xTaskCreate(Worker, "orbit_fence", 8192, retained, 3, &task_) != pdPASS) {
        delete retained;
        stopping_.store(true);
        stopped_ = true;
        task_ = nullptr;
        return false;
    }
    return true;
}
void OutputFenceRuntime::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_.store(true);
    pending_.reset();
    if (task_)
        xTaskNotifyGive(task_);
    else if (!started_)
        stopped_ = true;
}
bool OutputFenceRuntime::Stopped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopped_;
}
ReadinessSnapshot OutputFenceRuntime::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}
bool OutputFenceRuntime::Submit(std::unique_ptr<Dispatch> dispatch) {
    if (!dispatch || dispatch->size == 0 || dispatch->size > dispatch->raw.size() ||
        dispatch->message.identity.device_id != device_id_ || !dispatch->Current())
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hydrated_ || stopping_.load() || !dispatch->Current())
        return false;
    if ((active_ && active_->Same(*dispatch)) || (pending_ && pending_->Same(*dispatch)))
        return true;
    if (pending_)
        return false;
    pending_ = std::move(dispatch);
    xTaskNotifyGive(task_);
    return true;
}
bool OutputFenceRuntime::Allows(const Identity& identity, const PhoneReceipt& receipt) {
    if (!allowance_open_)
        return false;
    allowance_open_ = false;
    return !stopping_.load() && active_ && active_->message.command == Command::AbortUnacquired &&
           SameIdentity(active_->message.identity, identity) &&
           SameReceipt(active_->message.receipt, receipt) && active_->Current();
}
void OutputFenceRuntime::Worker(void* argument) {
    auto retained = std::move(*static_cast<std::shared_ptr<OutputFenceRuntime>*>(argument));
    delete static_cast<std::shared_ptr<OutputFenceRuntime>*>(argument);
    retained->Run();
    retained.reset();
    vTaskDelete(nullptr);
}
void OutputFenceRuntime::Run() {
    // Construction, hydration, durable mutations and physical observations all run
    // here. The application must have created its closed global gate before Start.
    Core core(store_, physical_, device_id_, this);
    core.Hydrate();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = core.Snapshot();
        hydrated_ = true;
    }
    while (!stopping_.load()) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_.load())
                break;
            active_ = std::move(pending_);
        }
        if (!active_)
            continue;
        Reply reply;
        if (!stopping_.load() && active_->Current()) {
            allowance_open_ = true;
            reply = core.Handle(std::string_view(active_->raw.data(), active_->size));
            allowance_open_ = false;
        }
        // A lost transport never rolls back durable Core progress. Only its exact
        // captured socket can receive this result, and queue admission is no proof.
        if (!stopping_.load() && !reply.json.empty() && active_->Current())
            active_->protocol->SendOutputFenceReply(*active_, reply.json);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_ = core.Snapshot();
            active_.reset();
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    allowance_open_ = false;
    active_.reset();
    pending_.reset();
    hydrated_ = false;
    task_ = nullptr;
    stopped_ = true;
}
}  // namespace provisions::output_fence
