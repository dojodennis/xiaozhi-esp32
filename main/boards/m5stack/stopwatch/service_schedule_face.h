#ifndef ORBIT_SERVICE_SCHEDULE_FACE_H_
#define ORBIT_SERVICE_SCHEDULE_FACE_H_

#include <utility>
#include "orbit_dial.h"
#include "service_schedule.h"

namespace orbit::service_schedule {

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
