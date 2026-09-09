#ifndef ORBIT_SERVICE_SCHEDULE_WORKER_H_
#define ORBIT_SERVICE_SCHEDULE_WORKER_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "service_schedule_nvs_store.h"

namespace orbit::service_schedule {

// Implement with the reviewed NvsStore for the explicitly selected live OR bench
// namespace. There is deliberately no default store or namespace. All calls and
// destruction happen on the storage thread. No other owner may write that store.
class WorkerStore {
public:
    virtual ~WorkerStore() = default;
    virtual storage::LoadResult Load(FacePersistentState&, storage::Bytes&) = 0;
    virtual storage::SaveResult Transition(const storage::Bytes* expected,
                                           const FacePersistentState&,
                                           storage::Bytes& verified) = 0;
};

struct WorkerToken {
    uint64_t owner_generation = 0;
    uint64_t snapshot_revision = 0;
};

enum class WorkerCommandKind {
    Snapshot,
    Tick,
    Acknowledge,
    Connection,
    Receipt,
    Reconcile,
    BeginClockRequest,
    AcceptClock
};
struct WorkerCommand {
    WorkerCommandKind kind = WorkerCommandKind::Tick;
    WorkerToken token;
    Snapshot snapshot;
    AlarmKey key;
    int64_t monotonic_ms = 0;
    bool connected = false;
    std::string clock_session_id;
    ClockRequest clock_request;
    ClockResponse clock_response;
};

enum class WorkerAdmission { Accepted, Busy, Stale, Invalid, RecoveryRequired, Stopped };
enum class WorkerStatus {
    BootAbsent,
    BootPresent,
    BootCorrupt,
    BootIoError,
    InvalidConfiguration,
    Applied,
    Replay,
    RejectedSnapshot,
    Projected,
    InvalidClock,
    Acknowledged,
    RejectedKey,
    ConnectionChanged,
    ReceiptAccepted,
    LimitExceeded,
    InvalidState,
    StorageConflict,
    StorageCorrupt,
    StorageIoError,
    Uncertain,
    ReconciledCandidate,
    ReconciledPrior,
    ReconciliationMismatch,
    ClockRequested,
    ClockAccepted,
    ClockRejected,
};

struct WorkerPublication {
    WorkerToken token;
    uint64_t sequence = 0;
    WorkerStatus status = WorkerStatus::InvalidConfiguration;
    ApplyResult apply_result = ApplyResult::Malformed;
    bool recovery_required = false;
    // Successful local ACK means durable outbox entry, NOT a remote SAVED receipt.
    // ReceiptAccepted requires the caller's exact authenticated receipt, persisted
    // before publication. No connected/authenticated status is inferred at boot.
    std::shared_ptr<const FaceModel> face;
};

// Executable asynchronous single-owner worker. Storage I/O never runs in Submit
// or TryTakePublication. One command total may be queued/in flight; the one-entry
// publication mailbox must be consumed before another command is admitted. Busy
// is explicit backpressure, never silent replacement or an unbounded retry queue.
//
// Caller validates transport/session/assignment/occurrence, IANA/Unicode policy
// before Snapshot; v1 additionally needs verified fresh server time. V2 clock
// samples use BeginClockRequest/AcceptClock only. Connection(true) for v2 requires
// the current authenticated session ID; missing IDs disconnect and fail closed.
// Use a never-reused unpredictable request UUID and actual monotonic send/receive
// times. Begin admission precedes socket send; its timestamp conservatively starts
// the response window before any worker wait. ClockRequested authorizes sending
// that exact request only for the still-current socket/owner. No transport is here.
// Caller fences each socket callback before Submit; payload IDs are not authority.
// Connection and Receipt are explicit authenticated caller events. Tokens bind
// every command to an enrolled owner and schedule revision. An ACK also binds the
// exact key, allowing other alarms to become due without ACKing a different one.
// Caller must use a fresh nonzero generation after replacement and fence queued
// Application::Schedule callbacks with BOTH generation and publication sequence.
// Do not admit an older publication after a newer one or use this as timer authority.
//
// Construct off audio callbacks after configuring the platform pthread stack /
// priority for this dedicated task. Poll the bounded mailbox on the application
// owner. RequestStop is nonblocking and prevents all further publication; it does
// not cancel an in-flight flash operation. The idle wait checks stop at least
// every 100 ms so a stop/wait race cannot strand the thread. Join/destruction can wait for storage:
// invoke only from a lifecycle/background owner, never main/audio/callback tasks.
// Join before replacing the worker for the same store. No reset/erase/scope switch.
class ServiceScheduleWorker {
public:
    ServiceScheduleWorker(Scope enrolled_scope, uint64_t owner_generation,
                          std::unique_ptr<WorkerStore> store);
    ~ServiceScheduleWorker();
    ServiceScheduleWorker(const ServiceScheduleWorker&) = delete;
    ServiceScheduleWorker& operator=(const ServiceScheduleWorker&) = delete;
    WorkerAdmission Submit(const WorkerCommand& command);
    std::shared_ptr<const WorkerPublication> TryTakePublication();
    void RequestStop();
    void Join();

private:
    struct PendingWrite;
    Scope scope_;
    uint64_t generation_;
    std::unique_ptr<WorkerStore> store_;
    FaceModel model_;
    std::optional<storage::Bytes> durable_;
    std::unique_ptr<PendingWrite> uncertain_;
    bool boot_known_ = false;
    uint64_t sequence_ = 0;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<WorkerCommand> command_;
    std::shared_ptr<const WorkerPublication> publication_;
    WorkerToken admission_token_;
    bool busy_ = true;
    bool recovery_required_ = true;
    std::atomic<bool> stopping_{false};
    std::thread thread_;

    void Run();
    void Boot();
    void Process(const WorkerCommand& command);
    void Persist(FaceModel candidate, WorkerStatus success);
    void Reconcile();
    void Publish(WorkerStatus status, bool recovery = false,
                 ApplyResult apply = ApplyResult::Malformed);
    bool RestoreChecked(const FacePersistentState& state, const storage::Bytes& bytes);
};

}  // namespace orbit::service_schedule
#endif
