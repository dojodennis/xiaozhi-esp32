#ifndef PROVISIONS_STOPWATCH_SERVICE_SCHEDULE_H_
#define PROVISIONS_STOPWATCH_SERVICE_SCHEDULE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace orbit::service_schedule {

inline constexpr size_t kMaximumItems = 64;
inline constexpr size_t kMaximumRetiredIds = 64;
inline constexpr uint64_t kMaximumRevision = 9007199254740991ULL;
inline constexpr int64_t kMaximumEpochMs = 253402300799999LL;
inline constexpr int64_t kMaximumOffsetMs = 7 * 24 * 60 * 60 * 1000LL;

struct Scope {
    std::string assignment_id;
    std::string device_id;
};

enum class CueKind { ServiceOffset, Fixed };
struct Cue {
    std::string id;
    uint64_t revision = 0;
    std::string label;
    CueKind kind = CueKind::Fixed;
    int64_t deadline_ms = 0;
    // Fixed cues must leave this zero; the JSON adapter must reject an offset field.
    int64_t offset_ms = 0;
};
struct Timer {
    std::string id;
    uint64_t revision = 0;
    std::string label;
    int64_t deadline_ms = 0;
};
struct Snapshot {
    int version = 1;
    Scope scope;
    std::string service_occurrence_id;
    uint64_t service_revision = 0;
    uint64_t snapshot_revision = 0;
    int64_t service_at_ms = 0;
    std::string timezone;
    int64_t server_now_ms = 0;
    std::vector<Cue> cues;
    std::vector<Timer> timers;
};

enum class ItemKind { Cue, Timer };
struct AlarmKey {
    Scope scope;
    ItemKind kind = ItemKind::Cue;
    // Cue acknowledgements are occurrence-bound; independent timers use empty.
    std::string service_occurrence_id;
    std::string id;
    uint64_t revision = 0;
};
struct ItemState {
    AlarmKey key;
    bool due = false;
    bool acknowledged = false;
};

// Typed persistence seam only. No NVS implementation or durable receipt is implied.
// The adapter must atomically save and read back the complete state, bind it to the
// enrolled scope, and reject corrupt/unknown versions. Never persist just the ACK.
struct PersistentState {
    int version = 1;
    Snapshot snapshot;
    std::vector<ItemState> items;
    int64_t last_known_epoch_ms = 0;
    std::vector<std::string> retired_ids;
};

enum class ApplyResult {
    Applied,
    Replay,
    Malformed,
    ScopeMismatch,
    StaleRevision,
    ConflictingRevision,
    InvalidClock,
    RetiredIdentity,
    HistoryCapacity,
};
enum class ClockState { AwaitingSnapshot, Trusted, AwaitingFreshTime, Invalid };
enum class AckResult { Acknowledged, AlreadyAcknowledged, NotDue, NotFound };

// Portable single-owner state machine, intentionally absent from the live board
// hooks. The caller must serialize calls on the application's owning task.
//
// BEFORE Apply: authenticate the current websocket/session and device assignment,
// enforce the opt-in v1 frame schema, reject unknown/duplicate JSON keys, validate
// the IANA zone and Unicode control categories, and bound allocations while parsing.
// The typed checks below are defense in depth, not authorization or a JSON parser.
// Scope comes from enrollment, never from an incoming snapshot. To change scope,
// an authorized owner must retire this instance and its persisted state explicitly.
//
// Full snapshots cancel omitted items. Retired IDs cannot be resurrected, even
// with a higher revision. At most 64 active identities and 64 retired IDs are kept.
// Exhausting that history rejects the candidate atomically. Recovery needs an
// explicitly authorized, reconciled reset/epoch adapter, which is NOT implemented;
// never reset on reconnect or a new occurrence. Snapshot revisions are durable across
// websocket sessions AND service occurrence changes within assignment/device scope.
class Scheduler {
public:
    explicit Scheduler(Scope enrolled_scope);

    ApplyResult Apply(const Snapshot& snapshot, int64_t monotonic_ms);
    ClockState Tick(int64_t monotonic_ms);
    void SetConnected(bool connected) { connected_ = connected; }
    bool connected() const { return connected_; }
    ClockState clock_state() const { return clock_state_; }
    int64_t now_ms() const { return last_known_epoch_ms_; }
    const Snapshot* snapshot() const { return has_snapshot_ ? &snapshot_ : nullptr; }
    const std::vector<ItemState>& items() const { return items_; }

    // Due is a latched observation, not permission to bypass the existing alarm
    // player/output owner. Existing timer takeover retains priority. An adapter
    // arbitrates visual/sound/haptic output and obtains physical acknowledgement.
    // Acknowledge changes RAM only; do not claim durable success before save/readback.
    AckResult Acknowledge(const AlarmKey& key);
    bool ExportState(PersistentState& output) const;
    // Fresh instance only. No alarms are newly latched after restore until Apply
    // accepts a NEWER authenticated snapshot with fresh server time. Replay cannot
    // reset the stale gate or manufacture a fresh clock. Already-due alarms survive.
    bool Restore(const PersistentState& state);

private:
    Scope scope_;
    bool has_snapshot_ = false;
    bool connected_ = false;
    Snapshot snapshot_;
    std::vector<ItemState> items_;
    std::vector<std::string> retired_ids_;
    ClockState clock_state_ = ClockState::AwaitingSnapshot;
    int64_t last_monotonic_ms_ = 0;
    int64_t last_known_epoch_ms_ = 0;
    void LatchDue();
};

}  // namespace orbit::service_schedule
#endif  // PROVISIONS_STOPWATCH_SERVICE_SCHEDULE_H_
