#include "service_schedule_hardware_bench.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

using namespace orbit::service_schedule;
using namespace orbit::service_schedule::storage;
using namespace std::chrono_literals;

namespace {
constexpr int64_t kNow = 1788883200000LL;
std::string Id(unsigned n) {
    char value[37];
    std::snprintf(value, sizeof(value), "00000000-0000-4000-8000-%012x", n);
    return value;
}
Bytes Encoded(const FacePersistentState& state) {
    Bytes bytes;
    assert(Encode(state, HardwareBenchScope(), bytes) == CodecResult::Accepted);
    return bytes;
}
FacePersistentState Decoded(const Bytes& bytes) {
    FacePersistentState state;
    assert(Decode(bytes.data(), bytes.size(), HardwareBenchScope(), state) ==
           CodecResult::Accepted);
    return state;
}
Bytes Saved(bool due) {
    Snapshot snapshot;
    snapshot.scope = HardwareBenchScope();
    snapshot.service_occurrence_id = Id(100);
    snapshot.snapshot_revision = snapshot.service_revision = 7;
    snapshot.service_at_ms = kNow + 3600000;
    snapshot.server_now_ms = kNow;
    snapshot.timezone = "Europe/Brussels";
    for (unsigned i = 0; i < 6; ++i)
        snapshot.timers.push_back({Id(110 + i), 1, "Existing", kNow + (due ? 0 : 15000)});
    FaceModel face(HardwareBenchScope());
    assert(face.ApplyVerified(snapshot, 0) == ApplyResult::Applied);
    FacePersistentState state;
    assert(face.ExportState(state));
    return Encoded(state);
}
struct Disk {
    std::mutex mutex;
    std::optional<Bytes> bytes;
    std::optional<LoadResult> load_error;
    bool uncertain = false, commit_candidate = false;
    size_t loads = 0, writes = 0;
    size_t Writes() {
        std::lock_guard<std::mutex> lock(mutex);
        return writes;
    }
    size_t Loads() {
        std::lock_guard<std::mutex> lock(mutex);
        return loads;
    }
    Bytes Read() {
        std::lock_guard<std::mutex> lock(mutex);
        assert(bytes);
        return *bytes;
    }
};
class Store final : public WorkerStore {
public:
    explicit Store(std::shared_ptr<Disk> disk) : disk_(std::move(disk)) {}
    LoadResult Load(FacePersistentState& output, Bytes& bytes) override {
        std::lock_guard<std::mutex> lock(disk_->mutex);
        ++disk_->loads;
        if (disk_->load_error)
            return *disk_->load_error;
        if (!disk_->bytes)
            return LoadResult::Absent;
        FacePersistentState state;
        if (Decode(disk_->bytes->data(), disk_->bytes->size(), HardwareBenchScope(), state) !=
            CodecResult::Accepted)
            return LoadResult::Corrupt;
        output = std::move(state);
        bytes = *disk_->bytes;
        return LoadResult::Present;
    }
    SaveResult Transition(const Bytes* expected, const FacePersistentState& state,
                          Bytes& verified) override {
        std::lock_guard<std::mutex> lock(disk_->mutex);
        assert(expected ? disk_->bytes && *expected == *disk_->bytes : !disk_->bytes);
        ++disk_->writes;
        const auto encoded = Encoded(state);
        if (!disk_->uncertain || disk_->commit_candidate)
            disk_->bytes = encoded;
        if (disk_->uncertain)
            return SaveResult::Uncertain;
        verified = encoded;
        return SaveResult::Saved;
    }

private:
    std::shared_ptr<Disk> disk_;
};
struct Outputs {
    unsigned starts = 0, cancels = 0;
    std::vector<bool> motor;
    bool allow_start = true;
    AlarmOutputHooks Hooks() {
        return {[this] {
                    ++starts;
                    return allow_start;
                },
                [this] { ++cancels; }, [] { return true; }, [] { return uint32_t{0}; },
                [this](bool value) { motor.push_back(value); }};
    }
};
using Publication = std::shared_ptr<const WorkerPublication>;
template <typename Predicate>
void Pump(HardwareBench& bench, int64_t now, Predicate until) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        bench.Poll(now);
        if (until())
            return;
        std::this_thread::sleep_for(1ms);
    }
    std::fprintf(stderr, "timed out with status %s\n", bench.Status().c_str());
    assert(false);
}
void Boot(HardwareBench& bench, int64_t now = 0) {
    Pump(bench, now, [&] { return bench.current() != nullptr; });
}
void Settle(HardwareBench& bench, int64_t now) {
    for (unsigned i = 0; i < 10; ++i) {
        bench.Poll(now);
        std::this_thread::sleep_for(1ms);
    }
}
void Seed(HardwareBench& bench) {
    Boot(bench);
    assert(bench.current()->status == WorkerStatus::BootAbsent);
    bench.Yellow(bench.Observed(), 0);
    Pump(bench, 0, [&] { return bench.current()->face->scheduler().snapshot() != nullptr; });
    Settle(bench, 0);
}
void SixDue(HardwareBench& bench) {
    Seed(bench);
    Pump(bench, 15000, [&] { return bench.current()->face->due().size() == 6; });
    Settle(bench, 15000);
}
void NoRemoteClaim(const HardwareBench& bench) {
    assert(!bench.current()->face->scheduler().connected());
    assert(!bench.current()->face->receipt_confirmed());
    assert(bench.current()->status != WorkerStatus::ReceiptAccepted);
    assert(bench.Status().find("SAVED") == std::string::npos);
}

void PhysicalSeedOnlyAfterAbsent() {
    auto disk = std::make_shared<Disk>();
    Outputs outputs;
    HardwareBench bench(std::make_unique<Store>(disk), outputs.Hooks(), 301);
    bench.Yellow({}, 0);
    Boot(bench);
    Settle(bench, 1000000);
    assert(disk->Writes() == 0 && !bench.current()->face->scheduler().snapshot());
    assert(bench.Status() == "YELLOW STARTS SIX TEST TIMERS");
    bench.Blue(bench.Observed());
    Settle(bench, 1000000);
    assert(disk->Writes() == 0);
    bench.Yellow(bench.Observed(), 1000000);
    Pump(bench, 1000000, [&] { return bench.current()->face->scheduler().snapshot() != nullptr; });
    const auto& snapshot = *bench.current()->face->scheduler().snapshot();
    assert(snapshot.cues.empty() && snapshot.timers.size() == 6);
    assert(snapshot.scope.assignment_id == HardwareBenchScope().assignment_id);
    for (const auto& timer : snapshot.timers)
        assert(timer.deadline_ms - snapshot.server_now_ms == 15000);
    assert(snapshot.timers.front().label == "Rice" && snapshot.timers.back().label == "Fish");
    assert(disk->Writes() == 1 && outputs.starts == 0);
    NoRemoteClaim(bench);
}

void RestoreNeverSeedsAndPreservesDue() {
    for (const bool due : {false, true}) {
        auto disk = std::make_shared<Disk>();
        disk->bytes = Saved(due);
        const auto original = *disk->bytes;
        Outputs outputs;
        HardwareBench bench(std::make_unique<Store>(disk), outputs.Hooks(), 302);
        Boot(bench);
        assert(bench.current()->status == WorkerStatus::BootPresent);
        assert(bench.Status() == "NEEDS SYNC");
        assert(bench.current()->face->due().size() == (due ? 6 : 0));
        assert((outputs.starts > 0) == due);
        bench.Yellow(bench.Observed(), 0);
        Settle(bench, 2000);
        assert(disk->Writes() == 0 && disk->Read() == original);
        assert(bench.current()->face->scheduler().clock_state() == ClockState::AwaitingFreshTime);
        assert(bench.current()->face->scheduler().now_ms() == kNow);
        assert(bench.current()->face->due().size() == (due ? 6 : 0));
        NoRemoteClaim(bench);
    }
}

void CorruptAndIoNeverSeedOrRetryAutomatically() {
    for (const auto failure : {LoadResult::Corrupt, LoadResult::IoError}) {
        auto disk = std::make_shared<Disk>();
        disk->load_error = failure;
        Outputs outputs;
        HardwareBench bench(std::make_unique<Store>(disk), outputs.Hooks(), 303);
        Boot(bench);
        const auto status = bench.Status();
        assert(status.find(failure == LoadResult::Corrupt ? "CORRUPT" : "ERROR") !=
               std::string::npos);
        Settle(bench, 15000);
        assert(disk->Loads() == 1 && disk->Writes() == 0);
        bench.Yellow(bench.Observed(), 15000);
        Pump(bench, 15000, [&] { return disk->Loads() == 2; });
        Settle(bench, 15000);
        assert(bench.current()->recovery_required &&
               !bench.current()->face->scheduler().snapshot());
        assert(disk->Writes() == 0 && outputs.starts == 0);
        NoRemoteClaim(bench);
    }
}

void ExactObservedBlueAndRapidDuplicate() {
    auto disk = std::make_shared<Disk>();
    Outputs outputs;
    HardwareBench bench(std::make_unique<Store>(disk), outputs.Hooks(), 304);
    SixDue(bench);
    const auto observed = bench.Observed();
    const auto selected = observed->face->scheduler().items().front().key;
    const auto writes = disk->Writes();
    bench.Blue(observed);
    bench.Blue(observed);
    Pump(bench, 15000, [&] { return bench.current()->face->pending().size() == 1; });
    Settle(bench, 15000);
    assert(bench.current()->face->pending().front().id == selected.id);
    assert(bench.current()->face->due().size() == 5 && disk->Writes() == writes + 1);
    assert(outputs.cancels == 0 && outputs.starts > 0);
    NoRemoteClaim(bench);
    // A callback queued from the old view carries the original exact key. It
    // must reject that already-ACKed item instead of silently ACKing the next.
    bench.Blue(observed);
    Pump(bench, 15000, [&] { return bench.current()->status == WorkerStatus::RejectedKey; });
    assert(bench.current()->face->pending().size() == 1 && disk->Writes() == writes + 1);
    assert(bench.Status() == "ACK NOT STORED - CHECK BOARD");
    const auto sequence = bench.current()->sequence;
    Pump(bench, 15250, [&] { return bench.current()->sequence > sequence; });
    assert(bench.current()->status == WorkerStatus::Projected);
    // The rejected user gesture must remain visible across routine clock frames.
    assert(bench.Status() == "ACK NOT STORED - CHECK BOARD");
}

void PendingGestureWinsOverBusyTicks() {
    auto disk = std::make_shared<Disk>();
    Outputs outputs;
    HardwareBench bench(std::make_unique<Store>(disk), outputs.Hooks(), 305);
    SixDue(bench);
    const auto writes = disk->Writes();
    for (size_t ack_count = 1; ack_count <= 6; ++ack_count) {
        const auto now = 15000 + static_cast<int64_t>(ack_count) * 250;
        bench.Poll(now);  // Admit a periodic tick before the physical gesture.
        bench.Blue(bench.Observed());
        Pump(bench, now, [&] { return bench.current()->face->pending().size() == ack_count; });
        assert(disk->Writes() == writes + ack_count);
        assert(bench.current()->face->due().size() == 6 - ack_count);
        NoRemoteClaim(bench);
    }
    assert(bench.current()->face->due().empty());
    assert(!outputs.motor.empty() && !outputs.motor.back());
}

void SaveFailureKeepsRingingAndExplicitReconcile(bool candidate_committed) {
    auto disk = std::make_shared<Disk>();
    Outputs outputs;
    HardwareBench bench(std::make_unique<Store>(disk), outputs.Hooks(), 306);
    SixDue(bench);
    const auto prior = disk->Read();
    const auto writes = disk->Writes();
    {
        std::lock_guard<std::mutex> lock(disk->mutex);
        disk->uncertain = true;
        disk->commit_candidate = candidate_committed;
    }
    bench.Blue(bench.Observed());
    Pump(bench, 15000, [&] { return bench.current()->recovery_required; });
    assert(bench.current()->status == WorkerStatus::Uncertain);
    assert(bench.current()->face->pending().empty() && bench.current()->face->due().size() == 6);
    assert(bench.Status() == "STORAGE UNCERTAIN - YELLOW CHECKS");
    assert(outputs.starts > 0 && outputs.cancels == 0);
    NoRemoteClaim(bench);
    Settle(bench, 17000);
    assert(disk->Writes() == writes + 1);
    assert(candidate_committed ? Decoded(disk->Read()).pending.size() == 1 : disk->Read() == prior);
    bench.Yellow(bench.Observed(), 17000);
    Pump(bench, 17000, [&] { return !bench.current()->recovery_required; });
    assert(bench.current()->status == (candidate_committed ? WorkerStatus::ReconciledCandidate
                                                           : WorkerStatus::ReconciledPrior));
    assert(bench.Status() == "NEEDS SYNC");
    assert(bench.current()->face->due().size() == (candidate_committed ? 5 : 6));
    const auto clock = bench.current()->face->scheduler().now_ms();
    const auto record = disk->Read();
    bench.Yellow(bench.Observed(), 17000);
    Settle(bench, 18000);
    assert(bench.current()->face->scheduler().clock_state() == ClockState::AwaitingFreshTime);
    assert(bench.current()->face->scheduler().now_ms() == clock);
    assert(disk->Read() == record && disk->Writes() == writes + 1);
    NoRemoteClaim(bench);
}

void FailedFirstSeedNeedsExplicitAbsenceProofAndNewPress() {
    for (const bool committed : {false, true}) {
        auto disk = std::make_shared<Disk>();
        disk->uncertain = true;
        disk->commit_candidate = committed;
        Outputs outputs;
        HardwareBench bench(std::make_unique<Store>(disk), outputs.Hooks(), 309);
        Boot(bench);
        bench.Yellow(bench.Observed(), 0);
        Pump(bench, 0, [&] { return bench.current()->recovery_required; });
        assert(disk->Writes() == 1 && !bench.current()->face->scheduler().snapshot());
        bench.Yellow(bench.Observed(), 0);
        Pump(bench, 0, [&] { return !bench.current()->recovery_required; });
        Settle(bench, 1000);
        assert(disk->Writes() == 1);  // Readback itself never seeds or writes.
        if (committed) {
            assert(bench.current()->status == WorkerStatus::ReconciledCandidate);
            assert(bench.Status() == "NEEDS SYNC");
            bench.Yellow(bench.Observed(), 1000);
            Settle(bench, 1000);
            assert(disk->Writes() == 1);
            assert(bench.current()->face->scheduler().clock_state() ==
                   ClockState::AwaitingFreshTime);
        } else {
            assert(bench.current()->status == WorkerStatus::ReconciledPrior);
            assert(!bench.current()->face->scheduler().snapshot());
            assert(bench.Status() == "YELLOW STARTS SIX TEST TIMERS");
            {
                std::lock_guard<std::mutex> lock(disk->mutex);
                disk->uncertain = false;
            }
            bench.Yellow(bench.Observed(), 1000);  // New, deliberate physical retry.
            Pump(bench, 1000,
                 [&] { return bench.current()->face->scheduler().snapshot() != nullptr; });
            assert(disk->Writes() == 2 &&
                   bench.current()->face->scheduler().snapshot()->timers.size() == 6);
        }
        NoRemoteClaim(bench);
    }
}

void StaleGenerationAndYellowPublication() {
    auto disk = std::make_shared<Disk>();
    Outputs outputs;
    HardwareBench bench(std::make_unique<Store>(disk), outputs.Hooks(), 307);
    Boot(bench);
    const auto boot = bench.Observed();
    auto other_disk = std::make_shared<Disk>();
    Outputs other_outputs;
    HardwareBench other(std::make_unique<Store>(other_disk), other_outputs.Hooks(), 308);
    Boot(other);
    bench.Yellow(other.Observed(), 0);
    bench.Blue(other.Observed());
    Settle(bench, 0);
    assert(disk->Writes() == 0);
    bench.Yellow(boot, 0);
    Pump(bench, 0, [&] { return bench.current()->face->scheduler().snapshot() != nullptr; });
    Settle(bench, 0);
    const auto writes = disk->Writes();
    bench.Yellow(boot, 0);
    assert(bench.Status() == "STATE CHANGED - PRESS AGAIN");
    Settle(bench, 1000);
    assert(disk->Writes() == writes);
    assert(bench.current()->token.owner_generation == 307);
    assert(bench.Observed()->sequence == bench.current()->sequence);
    NoRemoteClaim(bench);
}
}  // namespace

int main() {
    PhysicalSeedOnlyAfterAbsent();
    RestoreNeverSeedsAndPreservesDue();
    CorruptAndIoNeverSeedOrRetryAutomatically();
    ExactObservedBlueAndRapidDuplicate();
    PendingGestureWinsOverBusyTicks();
    SaveFailureKeepsRingingAndExplicitReconcile(false);
    SaveFailureKeepsRingingAndExplicitReconcile(true);
    StaleGenerationAndYellowPublication();
    FailedFirstSeedNeedsExplicitAbsenceProofAndNewPress();
    std::puts("9 hardware bench coordinator scenarios passed");
}
