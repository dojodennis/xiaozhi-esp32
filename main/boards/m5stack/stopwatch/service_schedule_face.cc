#include "service_schedule_face.h"

#include <algorithm>

namespace orbit::service_schedule {
namespace {
bool Same(const AlarmKey& a, const AlarmKey& b) {
    return a.scope.assignment_id == b.scope.assignment_id &&
           a.scope.device_id == b.scope.device_id &&
           a.service_occurrence_id == b.service_occurrence_id && a.kind == b.kind && a.id == b.id &&
           a.revision == b.revision;
}
std::string OutputIdentity(const AlarmKey& key) {
    return key.service_occurrence_id + "/" + key.id + "/" + std::to_string(key.revision);
}
}  // namespace

ApplyResult FaceModel::ApplyVerified(const Snapshot& snapshot, int64_t monotonic_ms) {
    // Caller owns authenticated delivery, occurrence admission and fresh-time proof.
    const auto result = scheduler_.Apply(snapshot, monotonic_ms);
    Refresh();
    return result;
}

void FaceModel::Tick(int64_t monotonic_ms) {
    scheduler_.Tick(monotonic_ms);
    Refresh();
}

void FaceModel::Refresh() {
    const auto* snapshot = scheduler_.snapshot();
    if (!snapshot)
        return;
    std::vector<ProvisionsTimerSnapshot::Timer> cooking;
    due_.clear();
    for (const auto& item : scheduler_.items()) {
        if (item.acknowledged)
            continue;
        ProvisionsTimerSnapshot::Timer projected;
        projected.id = OutputIdentity(item.key);
        projected.status = item.due ? ProvisionsTimerSnapshot::TimerStatus::kAttention
                                    : ProvisionsTimerSnapshot::TimerStatus::kActive;
        if (item.key.kind == ItemKind::Timer) {
            const auto source = std::find_if(snapshot->timers.begin(), snapshot->timers.end(),
                                             [&](const Timer& t) { return t.id == item.key.id; });
            if (source == snapshot->timers.end())
                continue;
            projected.label = source->label;
            projected.deadline_ms = source->deadline_ms;
            cooking.push_back(projected);
        } else {
            const auto source = std::find_if(snapshot->cues.begin(), snapshot->cues.end(),
                                             [&](const Cue& c) { return c.id == item.key.id; });
            if (source == snapshot->cues.end())
                continue;
            projected.label = source->label;
            projected.deadline_ms = source->deadline_ms;
        }
        if (item.due)
            due_.push_back(projected);
    }
    dial_.Update(cooking, scheduler_.now_ms());
    const auto change = alarm_.Update(due_);
    if (change != ProvisionsStopwatchOrbit::AlarmOutputChange::kNone)
        output_ = change;
}

bool FaceModel::AcknowledgeNext() {
    if (pending_.size() >= kMaximumItems)
        return false;
    for (const auto& item : scheduler_.items()) {
        if (!item.due || item.acknowledged)
            continue;
        const auto key = item.key;
        if (scheduler_.Acknowledge(key) != AckResult::Acknowledged)
            return false;
        pending_.push_back(key);
        receipt_confirmed_ = false;
        Refresh();
        return true;
    }
    return false;
}

bool FaceModel::AcceptReceipt(const AlarmKey& key) {
    if (!scheduler_.connected())
        return false;
    const auto item = std::find_if(pending_.begin(), pending_.end(),
                                   [&](const AlarmKey& pending) { return Same(key, pending); });
    if (item == pending_.end())
        return false;
    pending_.erase(item);
    receipt_confirmed_ = true;
    return true;
}

ProvisionsStopwatchOrbit::AlarmOutputChange FaceModel::TakeOutputChange() {
    const auto result = output_;
    output_ = ProvisionsStopwatchOrbit::AlarmOutputChange::kNone;
    return result;
}
}  // namespace orbit::service_schedule
