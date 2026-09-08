#include "service_schedule_hardware_bench.h"

#include <cstdio>

namespace orbit::service_schedule {
namespace {
std::string FixtureId(unsigned value) {
    char result[37];
    std::snprintf(result, sizeof(result), "00000000-0000-4000-8000-%012x", value);
    return result;
}
Snapshot Fixture() {
    Snapshot s;
    s.scope = HardwareBenchScope();
    s.service_occurrence_id = FixtureId(903);
    s.service_revision = s.snapshot_revision = 1;
    // Labeled synthetic clock. It is seeded only after proven absent bench
    // storage and a physical press, never on restore/reconcile/reconnect.
    s.server_now_ms = 1788883200000LL;
    s.service_at_ms = s.server_now_ms + 3600000;
    s.timezone = "Europe/Brussels";
    const char* names[] = {"Rice", "Sauce", "Bread", "Stock", "Pasta", "Fish"};
    for (unsigned i = 0; i < 6; ++i)
        s.timers.push_back({FixtureId(910 + i), 1, names[i], s.server_now_ms + 15000});
    return s;
}
}  // namespace
Scope HardwareBenchScope() { return {FixtureId(901), FixtureId(902)}; }

HardwareBench::HardwareBench(std::unique_ptr<WorkerStore> store, AlarmOutputHooks hooks,
                             uint64_t generation)
    : generation_(generation),
      worker_(HardwareBenchScope(), generation, std::move(store)),
      output_(std::move(hooks)) {}

std::shared_ptr<const WorkerPublication> HardwareBench::Observed() const {
    return std::atomic_load(&observed_);
}
bool HardwareBench::Queue(WorkerCommand command) {
    if (pending_ || command_in_flight_) {
        RejectGesture();
        return false;
    }
    pending_ = std::move(command);
    notice_ = "WORKING";
    return true;
}
void HardwareBench::RejectGesture() { notice_ = "BUSY - PRESS AGAIN"; }

void HardwareBench::Blue(std::shared_ptr<const WorkerPublication> observed) {
    if (!observed || observed->token.owner_generation != generation_ || !observed->face) {
        notice_ = "WAIT FOR BOARD";
        return;
    }
    for (const auto& item : observed->face->scheduler().items()) {
        if (!item.due || item.acknowledged)
            continue;
        WorkerCommand command;
        command.kind = WorkerCommandKind::Acknowledge;
        command.token = observed->token;
        command.key = item.key;
        Queue(std::move(command));
        return;
    }
    notice_ = "NO DUE ALERT";
}
void HardwareBench::Yellow(std::shared_ptr<const WorkerPublication> observed, int64_t now_ms) {
    if (!observed || !current_ || observed->token.owner_generation != generation_ ||
        observed->sequence != current_->sequence) {
        notice_ = "STATE CHANGED - PRESS AGAIN";
        return;
    }
    if (output_.fault()) {
        notice_ = output_.Retry(now_ms) ? "AUDIO RETRY" : "AUDIO FAULT";
        return;
    }
    WorkerCommand command;
    command.token = observed->token;
    if (observed->recovery_required) {
        command.kind = WorkerCommandKind::Reconcile;
        Queue(std::move(command));
    } else if (can_seed_ && !observed->face->scheduler().snapshot()) {
        command.kind = WorkerCommandKind::Snapshot;
        command.monotonic_ms = now_ms;
        command.snapshot = Fixture();
        if (Queue(std::move(command)))
            can_seed_ = false;
    } else {
        notice_ = "NO BENCH RESET";
    }
}
void HardwareBench::Poll(int64_t now_ms) {
    if (auto next = worker_.TryTakePublication()) {
        if (next->token.owner_generation == generation_ &&
            (!current_ || next->sequence > current_->sequence)) {
            current_ = std::move(next);
            std::atomic_store(&observed_, current_);
            if (current_->status == WorkerStatus::BootAbsent ||
                (current_->status == WorkerStatus::ReconciledPrior &&
                 !current_->face->scheduler().snapshot()))
                can_seed_ = true;
            command_in_flight_ = false;
            if (current_->status != WorkerStatus::Projected)
                notice_.clear();
            if (current_->status == WorkerStatus::RejectedKey ||
                current_->status == WorkerStatus::LimitExceeded ||
                current_->status == WorkerStatus::InvalidState)
                notice_ = "ACK NOT STORED - CHECK BOARD";
        }
    }
    if (pending_) {
        const auto result = worker_.Submit(*pending_);
        if (result == WorkerAdmission::Accepted) {
            pending_.reset();
            command_in_flight_ = true;
        } else if (result != WorkerAdmission::Busy) {
            pending_.reset();
            notice_ = result == WorkerAdmission::RecoveryRequired ? "STORAGE CHECK - YELLOW"
                                                                  : "STATE CHANGED - PRESS AGAIN";
        }
    } else if (current_ && !current_->recovery_required && !command_in_flight_ &&
               current_->face->scheduler().clock_state() == ClockState::Trusted &&
               now_ms >= next_tick_ms_) {
        WorkerCommand tick;
        tick.kind = WorkerCommandKind::Tick;
        tick.token = current_->token;
        tick.monotonic_ms = now_ms;
        if (worker_.Submit(tick) == WorkerAdmission::Accepted)
            next_tick_ms_ = now_ms + 250;
    }
    output_.SetWanted(current_ && current_->face && !current_->face->due().empty(), now_ms);
    output_.Poll(now_ms);
}
std::string HardwareBench::Status() const {
    if (output_.fault())
        return "AUDIO FAULT - YELLOW RETRIES";
    if (!current_)
        return "READING STORAGE";
    if (current_->recovery_required) {
        switch (current_->status) {
            case WorkerStatus::BootCorrupt:
            case WorkerStatus::StorageCorrupt:
                return "STORAGE CORRUPT - NO RESET";
            case WorkerStatus::BootIoError:
            case WorkerStatus::StorageIoError:
                return "STORAGE ERROR - YELLOW CHECKS";
            default:
                return "STORAGE UNCERTAIN - YELLOW CHECKS";
        }
    }
    if (current_->face->scheduler().clock_state() == ClockState::AwaitingFreshTime)
        return "NEEDS SYNC";
    if (current_->face->scheduler().clock_state() == ClockState::Invalid)
        return "CHECK TIME";
    if (!notice_.empty())
        return notice_;
    if (!current_->face->scheduler().snapshot())
        return "YELLOW STARTS SIX TEST TIMERS";
    if (current_->status == WorkerStatus::RejectedKey ||
        current_->status == WorkerStatus::LimitExceeded)
        return "ACK NOT STORED - CHECK BOARD";
    if (pending_ || command_in_flight_)
        return "WORKING";
    if (!current_->face->pending().empty())
        return "ACK STORED HERE - SYNC PENDING";
    return "SYNTHETIC CLOCK - NO NETWORK";
}
}  // namespace orbit::service_schedule
