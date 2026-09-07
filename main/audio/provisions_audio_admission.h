#ifndef PROVISIONS_AUDIO_ADMISSION_H_
#define PROVISIONS_AUDIO_ADMISSION_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace provisions::audio_admission {
using Uuid = std::array<uint8_t, 16>;
using Digest = std::array<uint8_t, 32>;
using Generation = uint64_t;

// Converted outside this primitive from the Core's validated canonical identity.
struct FenceIdentity {
    Uuid device_id{}, lease_id{}, playback_id{}, request_id{}, route_epoch{},
        device_connection_id{};
    Digest checkpoint_sha256{};
    uint64_t fence_epoch = 0, sequence = 0;
    uint32_t response_revision = 0;
};
enum class TimerKind { Preparation, Alarm };
struct TimerIdentity {
    TimerKind kind = TimerKind::Preparation;
    Uuid lease_id{}, playback_id{}, timer_id{};
    uint32_t timer_revision = 0, attempt = 0;
};
enum class Producer {
    Capture,
    InputPreparation,
    InputRead,
    Encode,
    Decode,
    Output,
    Notification,
    TimerPreparation,
    TimerRecovery,
    OrdinaryOutput,
    CaptureWork,
    CaptureUpload,
    Count
};
enum class Acknowledgement { Boot, Main, Notification, Recorder, Engine, Count };

class Gate;
// One-shot: pin this object through actual completion, including cancellation-resistant work.
// Destruction does NOT release a reservation. A lost token conservatively retains its slot.
class Reservation final {
public:
    Reservation() = default;
    ~Reservation() = default;
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    Reservation(Reservation&&) = delete;
    Reservation& operator=(Reservation&&) = delete;

private:
    friend class Gate;
    mutable std::mutex mutex_;
    const Gate* gate_ = nullptr;
    size_t slot_ = 0;
    uint64_t serial_ = 0;
};

struct ClosureSnapshot {
    Generation generation = 0;
    bool blocked = true, faulted = false, owner_matches = false;
    uint32_t acknowledgements = 0;
    size_t active = 0;
    std::array<size_t, static_cast<size_t>(Producer::Count)> active_by_producer{};
    bool timer_recovery_retained = false;
    // Metadata only. This NEVER asserts codec/DMA drain, storage or server authority.
    bool metadata_closed = false;
};

struct ParentSnapshot {
    bool owned = false, current = false, input_sealed = true;
    size_t children = 0;
    std::array<size_t, static_cast<size_t>(Producer::Count)> children_by_producer{};
};

class Gate final {
public:
    static constexpr size_t kCapacity = 32;
    static constexpr uint32_t kAllAcknowledgements =
        (1u << static_cast<unsigned>(Acknowledgement::Count)) - 1;
    // Starts closed with no implicit acknowledgements. Zero is invalid; exhaustion stays closed.
    explicit Gate(Generation initial_generation = 1);
    Gate(const Gate&) = delete;
    Gate& operator=(const Gate&) = delete;

    // Repeated requests while closed retain the same generation so closure ACKs can catch up.
    Generation BeginClose();
    // An explicit new boundary invalidates old publication and ACKs even when already closed.
    Generation Invalidate();
    bool Hold(const FenceIdentity& identity, Generation generation);
    bool Acknowledge(Acknowledgement acknowledgement, Generation generation);
    ClosureSnapshot Snapshot() const;
    ClosureSnapshot Snapshot(const FenceIdentity& expected_owner) const;
    // Caller invokes only after actual physical drain and a validated durable Core terminal.
    // Exact metadata CAS only: this method does not establish either of those external facts.
    bool OpenAfterTerminal(const FenceIdentity& identity, Generation generation);

    bool Reserve(Producer producer, Reservation& reservation);
    bool ReserveTimerMedia(const Reservation& parent, Producer producer, Reservation& reservation);
    bool ReserveCaptureMedia(const Reservation& parent, Producer producer,
                             Reservation& reservation);
    bool ReserveMedia(const Reservation& parent, Producer producer, Reservation& reservation);
    bool SealCaptureInput(const Reservation& parent);
    ParentSnapshot Snapshot(const Reservation& parent) const;
    bool AllowsPublication(const Reservation& reservation) const;
    bool AllowsPublication(const Reservation& reservation, Producer expected) const;
    bool AllowsCaptureInput(const Reservation& reservation, Producer expected) const;
    bool IsChild(const Reservation& parent, const Reservation& reservation) const;
    bool CompleteChild(const Reservation& parent, Reservation& reservation);
    bool Complete(Reservation& reservation);  // Frees the slot; the token object remains spent.

    // Only for a positively validated existing durable timer, before this generation's Boot ACK.
    // A fresh prepare must use Reserve(TimerPreparation), never this recovery-only registration.
    bool RegisterRetainedTimer(const TimerIdentity& identity, Generation generation);
    bool ReserveTimerRecovery(const TimerIdentity& identity, Reservation& reservation);
    // Call only after the existing timer's exact durable terminal ACK, not on task completion.
    bool RetireRetainedTimer(const TimerIdentity& identity, Generation generation);

private:
    struct Slot {
        const Reservation* token = nullptr;
        uint64_t serial = 0;
        Generation generation = 0;
        Producer producer = Producer::Capture;
        size_t parent_slot = kCapacity;
        uint64_t parent_serial = 0;
        bool input_sealed = false;
    };
    Generation AdvanceLocked();
    void FaultLocked();
    bool ReserveLocked(Producer producer, Reservation& reservation);
    bool MatchesLocked(const Reservation& reservation) const;
    bool AllowsLocked(const Reservation& reservation) const;
    bool ReserveMediaLocked(const Reservation& parent, Producer producer, Reservation& reservation);
    ClosureSnapshot SnapshotLocked(const FenceIdentity* expected_owner) const;

    mutable std::mutex mutex_;
    std::array<Slot, kCapacity> slots_{};
    Generation generation_;
    uint64_t next_serial_ = 1;
    bool blocked_ = true, faulted_ = false, has_owner_ = false, owner_terminal_ = false;
    uint32_t acknowledgements_ = 0;
    FenceIdentity owner_{};
    bool retained_timer_ = false;
    TimerIdentity timer_{};
};
}  // namespace provisions::audio_admission
#endif
