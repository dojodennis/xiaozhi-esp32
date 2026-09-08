#include "service_schedule_face.h"

#include <algorithm>
#include <string_view>

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
bool Uuid(std::string_view value) {
    if (value.size() != 36)
        return false;
    bool nonzero = false;
    for (size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-')
                return false;
        } else {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                return false;
            nonzero = nonzero || c != '0';
        }
    }
    return nonzero;
}

bool PendingKeyValid(const AlarmKey& key, const PersistentState& schedule) {
    if (key.scope.assignment_id != schedule.snapshot.scope.assignment_id ||
        key.scope.device_id != schedule.snapshot.scope.device_id || !Uuid(key.id) ||
        key.revision == 0 || key.revision > kMaximumRevision)
        return false;
    if (key.kind == ItemKind::Cue) {
        if (!Uuid(key.service_occurrence_id))
            return false;
    } else if (key.kind != ItemKind::Timer || !key.service_occurrence_id.empty()) {
        return false;
    }
    const auto current = std::find_if(schedule.items.begin(), schedule.items.end(),
                                      [&](const ItemState& item) { return item.key.id == key.id; });
    if (current != schedule.items.end()) {
        if (key.kind != current->key.kind ||
            key.service_occurrence_id != current->key.service_occurrence_id ||
            key.revision > current->key.revision)
            return false;
        // A newer version can still have an older local ACK awaiting reconciliation.
        // At the exact version, the persisted scheduler must carry the same ACK.
        return key.revision < current->key.revision || (current->due && current->acknowledged);
    }
    // Omission retires identities but must not discard their pending receipts.
    // The pending key is the retained history; the UUID tombstone cannot prove
    // its old type/revision independently. This is structural validation, not auth.
    return std::find(schedule.retired_ids.begin(), schedule.retired_ids.end(), key.id) !=
           schedule.retired_ids.end();
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

bool FaceModel::ExportState(FacePersistentState& output) const {
    FacePersistentState candidate;
    if (!scheduler_.ExportState(candidate.schedule))
        return false;
    candidate.pending = pending_;
    output = std::move(candidate);
    return true;
}

bool FaceModel::RestoreState(const FacePersistentState& saved) {
    if (scheduler_.snapshot() || saved.version != 1 || saved.pending.size() > kMaximumItems)
        return false;
    // Restore into a copy so an invalid pending outbox cannot partially install
    // a valid schedule. Scheduler::Restore owns all scope, snapshot and clock checks.
    Scheduler restored = scheduler_;
    restored.SetConnected(false);
    if (!restored.Restore(saved.schedule))
        return false;
    for (size_t i = 0; i < saved.pending.size(); ++i) {
        const auto& key = saved.pending[i];
        if (!PendingKeyValid(key, saved.schedule) ||
            std::any_of(saved.pending.begin(), saved.pending.begin() + i,
                        [&](const AlarmKey& previous) { return Same(key, previous); }))
            return false;
    }
    scheduler_ = std::move(restored);
    pending_ = saved.pending;
    receipt_confirmed_ = false;
    Refresh();
    return true;
}

ProvisionsStopwatchOrbit::AlarmOutputChange FaceModel::TakeOutputChange() {
    const auto result = output_;
    output_ = ProvisionsStopwatchOrbit::AlarmOutputChange::kNone;
    return result;
}
}  // namespace orbit::service_schedule
