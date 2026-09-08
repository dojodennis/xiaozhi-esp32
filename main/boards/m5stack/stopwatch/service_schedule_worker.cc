#include "service_schedule_worker.h"

#include <algorithm>
#include <chrono>
#include <string_view>
#include <utility>

namespace orbit::service_schedule {
namespace {
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
            nonzero |= c != '0';
        }
    }
    return nonzero;
}
// Check sizes before copying into the bounded command slot. Full semantic and
// UTF-8 validation remains the real scheduler/codec's responsibility on the worker.
bool Bounded(const WorkerCommand& command) {
    const auto& s = command.snapshot;
    if (s.cues.size() > storage::kMaximumActiveItems ||
        s.timers.size() > storage::kMaximumActiveItems - s.cues.size() ||
        s.scope.assignment_id.size() > 36 || s.scope.device_id.size() > 36 ||
        s.service_occurrence_id.size() > 36 || s.timezone.size() > 64)
        return false;
    for (const auto& cue : s.cues)
        if (cue.id.size() > 36 || cue.label.size() > 320)
            return false;
    for (const auto& timer : s.timers)
        if (timer.id.size() > 36 || timer.label.size() > 320)
            return false;
    const auto& k = command.key;
    return k.scope.assignment_id.size() <= 36 && k.scope.device_id.size() <= 36 &&
           k.service_occurrence_id.size() <= 36 && k.id.size() <= 36 &&
           command.kind >= WorkerCommandKind::Snapshot &&
           command.kind <= WorkerCommandKind::Reconcile;
}
bool DueChanged(const FaceModel& before, const FaceModel& after) {
    const auto& a = before.scheduler().items();
    const auto& b = after.scheduler().items();
    if (a.size() != b.size())
        return true;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].due != b[i].due)
            return true;
    return false;
}
WorkerStatus Failure(storage::SaveResult result) {
    switch (result) {
        case storage::SaveResult::Conflict:
            return WorkerStatus::StorageConflict;
        case storage::SaveResult::Corrupt:
            return WorkerStatus::StorageCorrupt;
        case storage::SaveResult::IoError:
            return WorkerStatus::StorageIoError;
        case storage::SaveResult::InvalidState:
            return WorkerStatus::InvalidState;
        default:
            return WorkerStatus::Uncertain;
    }
}
}  // namespace

struct ServiceScheduleWorker::PendingWrite {
    FaceModel candidate;
    FacePersistentState state;
    storage::Bytes bytes;
    std::optional<storage::Bytes> expected;
};

ServiceScheduleWorker::ServiceScheduleWorker(Scope enrolled_scope, uint64_t owner_generation,
                                             std::unique_ptr<WorkerStore> store)
    : scope_(std::move(enrolled_scope)),
      generation_(owner_generation),
      store_(std::move(store)),
      model_(scope_),
      admission_token_{generation_, 0},
      thread_([this] { Run(); }) {}

ServiceScheduleWorker::~ServiceScheduleWorker() {
    RequestStop();
    Join();
}
void ServiceScheduleWorker::RequestStop() {
    stopping_.store(true);
    wake_.notify_one();
}
void ServiceScheduleWorker::Join() {
    if (thread_.joinable())
        thread_.join();
}
WorkerAdmission ServiceScheduleWorker::Submit(const WorkerCommand& command) {
    if (stopping_.load())
        return WorkerAdmission::Stopped;
    if (!Bounded(command))
        return WorkerAdmission::Invalid;
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return WorkerAdmission::Busy;
    if (stopping_.load())
        return WorkerAdmission::Stopped;
    if (busy_ || publication_)
        return WorkerAdmission::Busy;
    if (command.token.owner_generation != admission_token_.owner_generation ||
        command.token.snapshot_revision != admission_token_.snapshot_revision)
        return WorkerAdmission::Stale;
    if (recovery_required_ && command.kind != WorkerCommandKind::Reconcile)
        return WorkerAdmission::RecoveryRequired;
    command_ = command;
    busy_ = true;
    lock.unlock();
    wake_.notify_one();
    return WorkerAdmission::Accepted;
}
std::shared_ptr<const WorkerPublication> ServiceScheduleWorker::TryTakePublication() {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || stopping_.load())
        return {};
    return std::exchange(publication_, {});
}
void ServiceScheduleWorker::Run() {
    Boot();
    while (!stopping_.load()) {
        std::optional<WorkerCommand> command;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // RequestStop deliberately never takes this mutex. Its notification
            // can race the predicate-to-wait boundary, so a bounded wait also
            // observes the atomic stop flag even if that notification was lost.
            wake_.wait_for(lock, std::chrono::milliseconds(100),
                           [&] { return stopping_.load() || command_.has_value(); });
            if (stopping_.load())
                break;
            if (!command_)
                continue;
            command = std::move(command_);
            command_.reset();
        }
        Process(*command);
    }
    // Own store destruction here as well: platform adapters may close handles.
    store_.reset();
}
void ServiceScheduleWorker::Publish(WorkerStatus status, bool recovery, ApplyResult apply) {
    if (stopping_.load())
        return;
    auto result = std::make_shared<WorkerPublication>();
    result->token = {generation_, model_.scheduler().snapshot()
                                      ? model_.scheduler().snapshot()->snapshot_revision
                                      : 0};
    result->sequence = ++sequence_;
    result->status = status;
    result->apply_result = apply;
    result->recovery_required = recovery;
    result->face = std::make_shared<const FaceModel>(model_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_.load())
        return;
    admission_token_ = result->token;
    recovery_required_ = recovery;
    publication_ = std::move(result);
    busy_ = false;
}
bool ServiceScheduleWorker::RestoreChecked(const FacePersistentState& state,
                                           const storage::Bytes& bytes) {
    storage::Bytes encoded;
    if (storage::Encode(state, scope_, encoded) != storage::CodecResult::Accepted ||
        encoded != bytes)
        return false;
    FaceModel restored(scope_);
    if (!restored.RestoreState(state))
        return false;
    model_ = std::move(restored);
    durable_ = bytes;
    boot_known_ = true;
    return true;
}
void ServiceScheduleWorker::Boot() {
    if (!store_ || generation_ == 0 || !Uuid(scope_.assignment_id) || !Uuid(scope_.device_id)) {
        Publish(WorkerStatus::InvalidConfiguration, true);
        return;
    }
    FacePersistentState state;
    storage::Bytes bytes;
    switch (store_->Load(state, bytes)) {
        case storage::LoadResult::Absent:
            boot_known_ = true;
            Publish(WorkerStatus::BootAbsent);
            return;
        case storage::LoadResult::Present:
            if (RestoreChecked(state, bytes)) {
                Publish(WorkerStatus::BootPresent);
                return;
            }
            Publish(WorkerStatus::BootCorrupt, true);
            return;
        case storage::LoadResult::Corrupt:
            Publish(WorkerStatus::BootCorrupt, true);
            return;
        case storage::LoadResult::IoError:
            Publish(WorkerStatus::BootIoError, true);
            return;
    }
}
void ServiceScheduleWorker::Persist(FaceModel candidate, WorkerStatus success) {
    FacePersistentState state;
    storage::Bytes bytes;
    if (!candidate.ExportState(state)) {
        Publish(WorkerStatus::InvalidState);
        return;
    }
    const auto encoded = storage::Encode(state, scope_, bytes);
    if (encoded != storage::CodecResult::Accepted) {
        Publish(encoded == storage::CodecResult::LimitExceeded ? WorkerStatus::LimitExceeded
                                                               : WorkerStatus::InvalidState);
        return;
    }
    auto pending = std::make_unique<PendingWrite>(
        PendingWrite{std::move(candidate), std::move(state), std::move(bytes), durable_});
    storage::Bytes verified;
    const auto saved = store_->Transition(pending->expected ? &*pending->expected : nullptr,
                                          pending->state, verified);
    if ((saved == storage::SaveResult::Saved || saved == storage::SaveResult::Unchanged) &&
        verified == pending->bytes) {
        model_ = std::move(pending->candidate);
        durable_ = std::move(verified);
        Publish(success, false,
                success == WorkerStatus::Applied ? ApplyResult::Applied : ApplyResult::Malformed);
        return;
    }
    // Even a claimed success with wrong bytes is uncertain. No further mutation,
    // clock update, connection event or automatic retry until explicit readback.
    uncertain_ = std::move(pending);
    Publish(Failure(saved), true);
}
void ServiceScheduleWorker::Process(const WorkerCommand& command) {
    if (command.kind == WorkerCommandKind::Reconcile) {
        Reconcile();
        return;
    }
    FaceModel candidate = model_;
    switch (command.kind) {
        case WorkerCommandKind::Snapshot: {
            const auto applied = candidate.ApplyVerified(command.snapshot, command.monotonic_ms);
            if (applied == ApplyResult::Applied) {
                Persist(std::move(candidate), WorkerStatus::Applied);
            } else if (applied == ApplyResult::InvalidClock) {
                model_ = std::move(candidate);
                Publish(WorkerStatus::InvalidClock, false, applied);
            } else {
                Publish(applied == ApplyResult::Replay ? WorkerStatus::Replay
                                                       : WorkerStatus::RejectedSnapshot,
                        false, applied);
            }
            return;
        }
        case WorkerCommandKind::Tick:
            candidate.Tick(command.monotonic_ms);
            if (DueChanged(model_, candidate)) {
                Persist(std::move(candidate), WorkerStatus::Projected);
            } else {
                model_ = std::move(candidate);
                Publish(model_.scheduler().clock_state() == ClockState::Invalid
                            ? WorkerStatus::InvalidClock
                            : WorkerStatus::Projected);
            }
            return;
        case WorkerCommandKind::Acknowledge:
            if (candidate.pending().size() >= storage::kMaximumPendingAcks) {
                Publish(WorkerStatus::LimitExceeded);
            } else if (candidate.Acknowledge(command.key)) {
                Persist(std::move(candidate), WorkerStatus::Acknowledged);
            } else {
                Publish(WorkerStatus::RejectedKey);
            }
            return;
        case WorkerCommandKind::Receipt:
            if (candidate.AcceptReceipt(command.key))
                Persist(std::move(candidate), WorkerStatus::ReceiptAccepted);
            else
                Publish(WorkerStatus::RejectedKey);
            return;
        case WorkerCommandKind::Connection:
            model_.SetConnected(command.connected);
            Publish(WorkerStatus::ConnectionChanged);
            return;
        case WorkerCommandKind::Reconcile:
            return;
    }
}
void ServiceScheduleWorker::Reconcile() {
    if (!store_ || generation_ == 0 || !Uuid(scope_.assignment_id) || !Uuid(scope_.device_id)) {
        Publish(WorkerStatus::InvalidConfiguration, true);
        return;
    }
    FacePersistentState state;
    storage::Bytes bytes;
    const auto loaded = store_->Load(state, bytes);
    if (loaded == storage::LoadResult::Corrupt || loaded == storage::LoadResult::IoError) {
        Publish(loaded == storage::LoadResult::Corrupt ? WorkerStatus::StorageCorrupt
                                                       : WorkerStatus::StorageIoError,
                true);
        return;
    }
    if (!boot_known_) {
        if (loaded == storage::LoadResult::Absent) {
            boot_known_ = true;
            Publish(WorkerStatus::BootAbsent);
        } else if (RestoreChecked(state, bytes)) {
            Publish(WorkerStatus::BootPresent);
        } else {
            Publish(WorkerStatus::StorageCorrupt, true);
        }
        return;
    }
    const auto& expected = uncertain_ ? uncertain_->expected : durable_;
    const bool matches_candidate =
        uncertain_ && loaded == storage::LoadResult::Present && bytes == uncertain_->bytes;
    const bool matches_prior = expected
                                   ? loaded == storage::LoadResult::Present && bytes == *expected
                                   : loaded == storage::LoadResult::Absent;
    if (!matches_candidate && !matches_prior) {
        Publish(WorkerStatus::ReconciliationMismatch, true);
        return;
    }
    if (loaded == storage::LoadResult::Present) {
        if (!RestoreChecked(state, bytes)) {
            Publish(WorkerStatus::StorageCorrupt, true);
            return;
        }
    } else {
        model_ = FaceModel(scope_);
        durable_.reset();
    }
    uncertain_.reset();
    // Readback proves bytes, not a fresh clock or an authenticated connection.
    Publish(matches_candidate ? WorkerStatus::ReconciledCandidate : WorkerStatus::ReconciledPrior);
}
}  // namespace orbit::service_schedule
