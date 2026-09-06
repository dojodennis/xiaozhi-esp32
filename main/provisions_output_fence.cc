#include "provisions_output_fence.h"

#include <utility>

namespace provisions::output_fence {
namespace {
bool SameCommon(const Identity& a, const Identity& b) {
    return a.device_id == b.device_id && a.fence_epoch == b.fence_epoch &&
           a.checkpoint_sha256 == b.checkpoint_sha256 && a.lease_id == b.lease_id &&
           a.sequence == b.sequence && a.playback_id == b.playback_id;
}
bool SameRelease(const Identity& a, const Identity& b) {
    return SameCommon(a, b) && a.request_id == b.request_id &&
           a.response_revision == b.response_revision && a.route_epoch == b.route_epoch;
}
}  // namespace
bool SameIdentity(const Identity& a, const Identity& b) {
    return SameRelease(a, b) && a.device_connection_id == b.device_connection_id;
}
bool SameReceipt(const PhoneReceipt& a, const PhoneReceipt& b) {
    return a.completed == b.completed && a.has_started == b.has_started &&
           a.started_at_ms == b.started_at_ms && a.stopped_at_ms == b.stopped_at_ms &&
           a.drained_at_ms == b.drained_at_ms;
}
Core::Core(Store& store, Physical& physical, std::string device_id)
    : store_(store), physical_(physical), device_id_(std::move(device_id)) {
    physical_.BlockAll();
}
bool Core::Hydrate() {
    std::lock_guard<std::mutex> lock(mutex_);
    physical_.BlockAll();
    loaded_ = gate_open_ = false;
    Record loaded;
    Manifest checked;
    if (!ValidId(device_id_) || store_.Load(loaded) != Store::LoadResult::Present ||
        loaded.identity.device_id != device_id_ || !EncodeRecord(loaded, checked))
        return false;
    record_ = std::move(loaded);
    loaded_ = true;
    if (!physical_.Hold(record_.identity))
        return false;
    if (record_.phase == Phase::Terminal)
        return OpenTerminal();
    return true;
}
bool Core::GateOpen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return loaded_ && gate_open_;
}
Reply Core::Respond(Result result) const { return {result, ReplyJson(result, record_.identity)}; }
bool Core::Closed() {
    return !gate_open_ && physical_.Hold(record_.identity) &&
           physical_.Observe(record_.identity).Closed();
}
bool Core::Persist(const Record& next) {
    if (!store_.CompareExchange(record_, next)) {
        physical_.BlockAll();
        loaded_ = gate_open_ =
            false;  // Unknown commit/readback: require a fresh durable hydration.
        return false;
    }
    record_ = next;
    return true;
}
bool Core::OpenTerminal() {
    if (record_.phase != Phase::Terminal)
        return false;
    if (!gate_open_) {
        if (!Closed())
            return false;
        if (!physical_.OpenAfterTerminal(record_.identity)) {
            physical_.BlockAll();
            return false;
        }
        gate_open_ = true;
    }
    return true;
}
Reply Core::Handle(std::string_view text) {
    Message message;
    if (!ParseMessage(text, message) || message.identity.device_id != device_id_)
        return {};
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loaded_)
        return {Result::RecoveryRequired, {}};
    const auto& incoming = message.identity;
    if (message.command == Command::Acquire) {
        if (record_.phase == Phase::Owned && SameIdentity(record_.identity, incoming))
            return Respond(Closed() ? Result::Acquired : Result::RecoveryRequired);
        // Phase never goes backwards on replay, nor can an unresolved owner be replaced.
        if (record_.phase != Phase::Terminal ||
            incoming.fence_epoch <= record_.identity.fence_epoch ||
            incoming.checkpoint_sha256 == record_.identity.checkpoint_sha256 ||
            incoming.playback_id == record_.identity.playback_id ||
            (incoming.lease_id == record_.identity.lease_id &&
             incoming.sequence <= record_.identity.sequence))
            return {};
        physical_.BlockAll();
        gate_open_ = false;
        Record next;
        next.identity = incoming;
        if (!Persist(next))
            return {Result::RecoveryRequired, {}};
        return Respond(Closed() ? Result::Acquired : Result::RecoveryRequired);
    }
    if (message.command == Command::Release) {
        if (!SameRelease(record_.identity, incoming) || record_.phase == Phase::Terminal)
            return {};
        if (record_.phase == Phase::DrainedPendingCommit &&
            !SameReceipt(record_.receipt, message.receipt))
            return {};
        if (!Closed())
            return Respond(Result::RecoveryRequired);
        if (record_.phase == Phase::Owned) {
            Record next = record_;
            next.phase = Phase::DrainedPendingCommit;
            next.receipt = message.receipt;
            if (!Persist(next))
                return {Result::RecoveryRequired, {}};
        }
        return Respond(Closed() ? Result::DrainPending : Result::RecoveryRequired);
    }
    if (!SameCommon(record_.identity, incoming) || record_.phase == Phase::Owned)
        return {};
    if (record_.phase == Phase::Terminal) {
        if (record_.backend_commit_id != message.backend_commit_id)
            return {};
    } else {
        if (!Closed())
            return Respond(Result::RecoveryRequired);
        Record next = record_;
        next.phase = Phase::Terminal;
        next.backend_commit_id = message.backend_commit_id;
        if (!Persist(next))
            return {Result::RecoveryRequired, {}};
    }
    return Respond(OpenTerminal() ? Result::Released : Result::RecoveryRequired);
}
}  // namespace provisions::output_fence
