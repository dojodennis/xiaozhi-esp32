#include "provisions_audio_admission.h"

#include <limits>

namespace provisions::audio_admission {
namespace {
constexpr uint64_t kMaximumWireInteger = 9007199254740991ULL;
constexpr uint32_t kMaximumRevision = 2147483647;
bool Nonzero(const Uuid& id) {
    for (const auto value : id)
        if (value != 0)
            return true;
    return false;
}
bool Valid(const FenceIdentity& id) {
    return Nonzero(id.device_id) && Nonzero(id.lease_id) && Nonzero(id.playback_id) &&
           Nonzero(id.request_id) && Nonzero(id.route_epoch) && Nonzero(id.device_connection_id) &&
           id.fence_epoch > 0 && id.fence_epoch <= kMaximumWireInteger && id.sequence > 0 &&
           id.sequence <= kMaximumWireInteger && id.response_revision > 0 &&
           id.response_revision <= kMaximumRevision;
}
bool Same(const FenceIdentity& a, const FenceIdentity& b) {
    return a.device_id == b.device_id && a.lease_id == b.lease_id &&
           a.playback_id == b.playback_id && a.request_id == b.request_id &&
           a.route_epoch == b.route_epoch && a.device_connection_id == b.device_connection_id &&
           a.checkpoint_sha256 == b.checkpoint_sha256 && a.fence_epoch == b.fence_epoch &&
           a.sequence == b.sequence && a.response_revision == b.response_revision;
}
bool Valid(const TimerIdentity& id) {
    if (!Nonzero(id.lease_id))
        return false;
    if (id.kind == TimerKind::Preparation)
        return !Nonzero(id.playback_id) && !Nonzero(id.timer_id) && id.timer_revision == 0 &&
               id.attempt == 0;
    return id.kind == TimerKind::Alarm && Nonzero(id.playback_id) && Nonzero(id.timer_id) &&
           id.timer_revision > 0 && id.timer_revision <= kMaximumRevision && id.attempt > 0 &&
           id.attempt <= kMaximumRevision;
}
bool Same(const TimerIdentity& a, const TimerIdentity& b) {
    return a.kind == b.kind && a.lease_id == b.lease_id && a.playback_id == b.playback_id &&
           a.timer_id == b.timer_id && a.timer_revision == b.timer_revision &&
           a.attempt == b.attempt;
}
}  // namespace

Gate::Gate(Generation initial_generation) : generation_(initial_generation) {
    if (generation_ == 0)
        FaultLocked();
}
void Gate::FaultLocked() {
    blocked_ = faulted_ = true;
    acknowledgements_ = 0;
}
Generation Gate::AdvanceLocked() {
    blocked_ = true;
    acknowledgements_ = 0;
    if (faulted_ || generation_ == std::numeric_limits<Generation>::max()) {
        FaultLocked();
    } else {
        ++generation_;
    }
    return generation_;
}
Generation Gate::BeginClose() {
    std::lock_guard<std::mutex> lock(mutex_);
    return blocked_ ? generation_ : AdvanceLocked();
}
Generation Gate::Invalidate() {
    std::lock_guard<std::mutex> lock(mutex_);
    return AdvanceLocked();
}
bool Gate::Hold(const FenceIdentity& identity, Generation generation) {
    if (!Valid(identity))
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!blocked_ || faulted_ || generation != generation_)
        return false;
    if (has_owner_) {
        if (Same(owner_, identity))
            return true;
        if (!owner_terminal_ || owner_.device_id != identity.device_id ||
            identity.fence_epoch <= owner_.fence_epoch ||
            identity.checkpoint_sha256 == owner_.checkpoint_sha256 ||
            identity.playback_id == owner_.playback_id ||
            (identity.lease_id == owner_.lease_id && identity.sequence <= owner_.sequence))
            return false;
    }
    owner_ = identity;
    has_owner_ = true;
    owner_terminal_ = false;
    return true;
}
bool Gate::Acknowledge(Acknowledgement acknowledgement, Generation generation) {
    if (static_cast<unsigned>(acknowledgement) >= static_cast<unsigned>(Acknowledgement::Count))
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!blocked_ || faulted_ || generation != generation_)
        return false;
    acknowledgements_ |= 1u << static_cast<unsigned>(acknowledgement);
    return true;
}
ClosureSnapshot Gate::SnapshotLocked(const FenceIdentity* expected_owner) const {
    ClosureSnapshot snapshot;
    snapshot.generation = generation_;
    snapshot.blocked = blocked_;
    snapshot.faulted = faulted_;
    snapshot.owner_matches =
        has_owner_ && expected_owner != nullptr && Same(owner_, *expected_owner);
    snapshot.acknowledgements = acknowledgements_;
    snapshot.timer_recovery_retained = retained_timer_;
    for (const auto& slot : slots_) {
        if (slot.token != nullptr) {
            ++snapshot.active;
            ++snapshot.active_by_producer[static_cast<size_t>(slot.producer)];
        }
    }
    snapshot.metadata_closed = blocked_ && !faulted_ && snapshot.owner_matches &&
                               acknowledgements_ == kAllAcknowledgements && snapshot.active == 0 &&
                               !retained_timer_;
    return snapshot;
}
ClosureSnapshot Gate::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return SnapshotLocked(nullptr);
}
ClosureSnapshot Gate::Snapshot(const FenceIdentity& expected_owner) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return SnapshotLocked(&expected_owner);
}
bool Gate::OpenAfterTerminal(const FenceIdentity& identity, Generation generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation != generation_ || !SnapshotLocked(&identity).metadata_closed)
        return false;
    blocked_ = false;
    owner_terminal_ = true;
    return true;
}
bool Gate::MatchesLocked(const Reservation& reservation) const {
    return reservation.gate_ == this && reservation.slot_ < slots_.size() &&
           reservation.serial_ != 0 && slots_[reservation.slot_].token == &reservation &&
           slots_[reservation.slot_].serial == reservation.serial_;
}
bool Gate::ReserveLocked(Producer producer, Reservation& reservation) {
    if (reservation.gate_ != nullptr || reservation.serial_ != 0)
        return false;
    for (size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].token != nullptr)
            continue;
        if (next_serial_ == std::numeric_limits<uint64_t>::max()) {
            FaultLocked();
            return false;
        }
        slots_[i] = {&reservation, next_serial_++, generation_, producer};
        reservation.gate_ = this;
        reservation.slot_ = i;
        reservation.serial_ = slots_[i].serial;
        return true;
    }
    return false;
}
bool Gate::Reserve(Producer producer, Reservation& reservation) {
    if (static_cast<size_t>(producer) >= static_cast<size_t>(Producer::Count) ||
        producer == Producer::TimerRecovery)
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    std::lock_guard<std::mutex> token_lock(reservation.mutex_);
    const auto primary = [](Producer value) {
        return value == Producer::Capture || value == Producer::TimerPreparation ||
               value == Producer::OrdinaryOutput;
    };
    if (producer == Producer::CaptureWork || producer == Producer::CaptureUpload)
        return false;  // These stages require their actual capture parent.
    for (const auto& slot : slots_) {
        if (slot.token != nullptr && (primary(producer) || primary(slot.producer)))
            return false;
    }
    return !blocked_ && !faulted_ && !retained_timer_ && ReserveLocked(producer, reservation);
}
bool Gate::ReserveMediaLocked(const Reservation& parent, Producer producer,
                              Reservation& reservation) {
    if (blocked_ || faulted_ || retained_timer_ || !MatchesLocked(parent) ||
        slots_[parent.slot_].generation != generation_)
        return false;
    const auto& root = slots_[parent.slot_];
    const bool input = producer == Producer::InputPreparation || producer == Producer::InputRead;
    const bool capture = root.producer == Producer::Capture &&
                         ((input && !root.input_sealed) || producer == Producer::Encode ||
                          producer == Producer::CaptureWork || producer == Producer::CaptureUpload);
    const bool output = (root.producer == Producer::TimerPreparation ||
                         root.producer == Producer::OrdinaryOutput) &&
                        (producer == Producer::Decode || producer == Producer::Output);
    if ((!capture && !output) || !ReserveLocked(producer, reservation))
        return false;
    slots_[reservation.slot_].parent_slot = parent.slot_;
    slots_[reservation.slot_].parent_serial = parent.serial_;
    return true;
}
bool Gate::ReserveMedia(const Reservation& parent, Producer producer, Reservation& reservation) {
    if (&parent == &reservation)
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    std::lock_guard<std::mutex> parent_lock(parent.mutex_);
    std::lock_guard<std::mutex> token_lock(reservation.mutex_);
    return ReserveMediaLocked(parent, producer, reservation);
}
bool Gate::ReserveTimerMedia(const Reservation& parent, Producer producer,
                             Reservation& reservation) {
    if (&parent == &reservation)
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    std::lock_guard<std::mutex> parent_lock(parent.mutex_);
    std::lock_guard<std::mutex> token_lock(reservation.mutex_);
    return MatchesLocked(parent) && slots_[parent.slot_].producer == Producer::TimerPreparation &&
           ReserveMediaLocked(parent, producer, reservation);
}
bool Gate::ReserveCaptureMedia(const Reservation& parent, Producer producer,
                               Reservation& reservation) {
    if (&parent == &reservation)
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    std::lock_guard<std::mutex> parent_lock(parent.mutex_);
    std::lock_guard<std::mutex> token_lock(reservation.mutex_);
    return MatchesLocked(parent) && slots_[parent.slot_].producer == Producer::Capture &&
           ReserveMediaLocked(parent, producer, reservation);
}
bool Gate::SealCaptureInput(const Reservation& parent) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::lock_guard<std::mutex> parent_lock(parent.mutex_);
    if (!MatchesLocked(parent) || slots_[parent.slot_].producer != Producer::Capture)
        return false;
    slots_[parent.slot_].input_sealed = true;
    return true;
}
ParentSnapshot Gate::Snapshot(const Reservation& parent) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::lock_guard<std::mutex> parent_lock(parent.mutex_);
    ParentSnapshot result;
    if (!MatchesLocked(parent))
        return result;
    result.owned = true;
    result.current = AllowsLocked(parent);
    result.input_sealed = slots_[parent.slot_].input_sealed;
    for (const auto& slot : slots_) {
        if (slot.token && slot.parent_slot == parent.slot_ &&
            slot.parent_serial == parent.serial_) {
            ++result.children;
            ++result.children_by_producer[static_cast<size_t>(slot.producer)];
        }
    }
    return result;
}
bool Gate::AllowsLocked(const Reservation& reservation) const {
    if (blocked_ || faulted_ || !MatchesLocked(reservation))
        return false;
    const auto& slot = slots_[reservation.slot_];
    if (slot.parent_slot < slots_.size()) {
        const auto& parent = slots_[slot.parent_slot];
        if (parent.token == nullptr || parent.serial != slot.parent_serial ||
            parent.generation != generation_)
            return false;
        if (parent.producer == Producer::Capture && parent.input_sealed &&
            (slot.producer == Producer::InputPreparation || slot.producer == Producer::InputRead))
            return false;
    }
    return slot.generation == generation_ && slot.producer != Producer::TimerRecovery;
}
bool Gate::AllowsPublication(const Reservation& reservation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::lock_guard<std::mutex> token_lock(reservation.mutex_);
    return AllowsLocked(reservation);
}
bool Gate::AllowsPublication(const Reservation& reservation, Producer expected) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::lock_guard<std::mutex> token_lock(reservation.mutex_);
    return AllowsLocked(reservation) && slots_[reservation.slot_].producer == expected;
}
bool Gate::AllowsCaptureInput(const Reservation& reservation, Producer expected) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::lock_guard<std::mutex> token_lock(reservation.mutex_);
    if ((expected != Producer::InputRead && expected != Producer::InputPreparation) ||
        !AllowsLocked(reservation))
        return false;
    const auto& child = slots_[reservation.slot_];
    return child.producer == expected && child.parent_slot < slots_.size() &&
           slots_[child.parent_slot].producer == Producer::Capture;
}
bool Gate::IsChild(const Reservation& parent, const Reservation& reservation) const {
    if (&parent == &reservation)
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    std::lock_guard<std::mutex> parent_lock(parent.mutex_);
    std::lock_guard<std::mutex> token_lock(reservation.mutex_);
    return MatchesLocked(parent) && MatchesLocked(reservation) &&
           slots_[reservation.slot_].parent_slot == parent.slot_ &&
           slots_[reservation.slot_].parent_serial == parent.serial_;
}
bool Gate::CompleteChild(const Reservation& parent, Reservation& reservation) {
    if (&parent == &reservation)
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    std::lock_guard<std::mutex> parent_lock(parent.mutex_);
    std::lock_guard<std::mutex> token_lock(reservation.mutex_);
    if (!MatchesLocked(parent) || !MatchesLocked(reservation) ||
        slots_[reservation.slot_].parent_slot != parent.slot_ ||
        slots_[reservation.slot_].parent_serial != parent.serial_)
        return false;
    slots_[reservation.slot_] = {};
    return true;
}
bool Gate::Complete(Reservation& reservation) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::lock_guard<std::mutex> token_lock(reservation.mutex_);
    if (!MatchesLocked(reservation))
        return false;
    slots_[reservation.slot_] = {};
    // Do not recycle this token object: a delayed second completion must not
    // release a new producer that reused the same object after the first one.
    return true;
}
bool Gate::RegisterRetainedTimer(const TimerIdentity& identity, Generation generation) {
    if (!Valid(identity))
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!blocked_ || faulted_ || generation != generation_)
        return false;
    if (retained_timer_)
        return Same(timer_, identity);
    if ((acknowledgements_ & (1u << static_cast<unsigned>(Acknowledgement::Boot))) != 0)
        return false;
    timer_ = identity;
    retained_timer_ = true;
    return true;
}
bool Gate::ReserveTimerRecovery(const TimerIdentity& identity, Reservation& reservation) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::lock_guard<std::mutex> token_lock(reservation.mutex_);
    if (!blocked_ || faulted_ || !retained_timer_ || !Same(timer_, identity))
        return false;
    if (MatchesLocked(reservation))
        return slots_[reservation.slot_].producer == Producer::TimerRecovery;
    for (const auto& slot : slots_)
        if (slot.token != nullptr && slot.producer == Producer::TimerRecovery)
            return false;
    return ReserveLocked(Producer::TimerRecovery, reservation);
}
bool Gate::RetireRetainedTimer(const TimerIdentity& identity, Generation generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!blocked_ || faulted_ || generation != generation_ || !retained_timer_ ||
        !Same(timer_, identity))
        return false;
    for (const auto& slot : slots_)
        if (slot.token != nullptr && (slot.producer == Producer::TimerRecovery ||
                                      slot.producer == Producer::TimerPreparation))
            return false;
    retained_timer_ = false;
    timer_ = {};
    return true;
}
}  // namespace provisions::audio_admission
