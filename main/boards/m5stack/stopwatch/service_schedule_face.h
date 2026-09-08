#ifndef ORBIT_SERVICE_SCHEDULE_FACE_H_
#define ORBIT_SERVICE_SCHEDULE_FACE_H_

#include <utility>
#include "orbit_dial.h"
#include "service_schedule.h"

namespace orbit::service_schedule {

// Persist the scheduler and exact local ACK outbox together. Receipt-confirmed
// UI state, connection state and a fresh clock are deliberately not restored.
// Pending keys are retained history after replacement/retirement. Structural
// validation cannot authenticate a rewritten history; byte integrity belongs to
// storage, and remote reconciliation still requires the exact authoritative key.
struct FacePersistentState {
    int version = 1;
    PersistentState schedule;
    std::vector<AlarmKey> pending;
};

// Single-owner bridge to the existing six-seat dial and alarm transition engine.
// It has no transport, credentials, storage or audio ownership. A local ACK is
// deliberately pending until an authenticated, exact authoritative receipt arrives.
class FaceModel {
public:
    explicit FaceModel(Scope scope) : scheduler_(std::move(scope)) {}
    ApplyResult ApplyVerified(const Snapshot& snapshot, int64_t monotonic_ms);
    void Tick(int64_t monotonic_ms);
    void SetConnected(bool connected) { scheduler_.SetConnected(connected); }
    bool AcknowledgeNext();
    bool AcceptReceipt(const AlarmKey& key);
    bool ExportState(FacePersistentState& output) const;
    // Fresh model only; validates the complete candidate before mutating this one.
    // The storage adapter must bind the enrolled scope and verify commit/readback
    // before publishing a durable outcome. This portable seam performs no I/O.
    bool RestoreState(const FacePersistentState& saved);
    const Scheduler& scheduler() const { return scheduler_; }
    const ProvisionsStopwatchOrbit::SlotBoard& dial() const { return dial_; }
    const std::vector<ProvisionsTimerSnapshot::Timer>& due() const { return due_; }
    const std::vector<AlarmKey>& pending() const { return pending_; }
    bool alarm_active() const { return alarm_.active(); }
    bool receipt_confirmed() const { return receipt_confirmed_; }
    ProvisionsStopwatchOrbit::AlarmOutputChange TakeOutputChange();

private:
    Scheduler scheduler_;
    ProvisionsStopwatchOrbit::SlotBoard dial_;
    ProvisionsStopwatchOrbit::AlarmState alarm_;
    std::vector<ProvisionsTimerSnapshot::Timer> due_;
    std::vector<AlarmKey> pending_;
    bool receipt_confirmed_ = false;
    ProvisionsStopwatchOrbit::AlarmOutputChange output_ =
        ProvisionsStopwatchOrbit::AlarmOutputChange::kNone;
    void Refresh();
};

}  // namespace orbit::service_schedule
#endif
