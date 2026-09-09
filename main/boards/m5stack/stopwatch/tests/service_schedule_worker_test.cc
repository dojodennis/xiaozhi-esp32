#include "service_schedule_worker.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

using namespace orbit::service_schedule;
using namespace orbit::service_schedule::storage;
using namespace std::chrono_literals;

namespace {
std::string Id(unsigned number) {
    char value[37];
    std::snprintf(value, sizeof(value), "00000000-0000-4000-8000-%012x", number);
    return value;
}
Scope Enrolled() { return {Id(1), Id(2)}; }
constexpr int64_t kNow = 1788883200000LL;
Snapshot SnapshotForTest(size_t count = 6, int64_t delay = 1000) {
    Snapshot snapshot;
    snapshot.scope = Enrolled();
    snapshot.service_occurrence_id = Id(3);
    snapshot.service_revision = snapshot.snapshot_revision = 1;
    snapshot.service_at_ms = kNow + 3600000;
    snapshot.server_now_ms = kNow;
    snapshot.timezone = "Europe/Brussels";
    for (size_t i = 0; i < count; ++i)
        snapshot.timers.push_back({Id(static_cast<unsigned>(10 + i)), 1, "Rice", kNow + delay});
    return snapshot;
}
Bytes EncodeState(const FacePersistentState& state) {
    Bytes bytes;
    assert(Encode(state, Enrolled(), bytes) == CodecResult::Accepted);
    return bytes;
}
FacePersistentState DecodeState(const Bytes& bytes) {
    FacePersistentState result;
    assert(Decode(bytes.data(), bytes.size(), Enrolled(), result) == CodecResult::Accepted);
    return result;
}
Bytes Record(const Snapshot& snapshot) {
    FaceModel model(Enrolled());
    assert(model.ApplyVerified(snapshot, 0) == ApplyResult::Applied);
    FacePersistentState state;
    assert(model.ExportState(state));
    return EncodeState(state);
}

enum class SaveFault {
    None,
    UncertainCandidate,
    UncertainPrior,
    WrongSuccess,
    IoError,
    Conflict,
    Corrupt
};
struct Disk {
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<Bytes> bytes;
    std::optional<LoadResult> load_fault;
    SaveFault save_fault = SaveFault::None;
    bool block_load = false, block_save = false, entered = false, release = false;
    size_t loads = 0, writes = 0;
    std::vector<std::thread::id> threads;
    std::thread::id destroyed;
    void WaitEntered() {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, 5s, [&] { return entered; }));
    }
    void Release() {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
        changed.notify_all();
    }
    size_t Writes() {
        std::lock_guard<std::mutex> lock(mutex);
        return writes;
    }
    Bytes BytesOnDisk() {
        std::lock_guard<std::mutex> lock(mutex);
        assert(bytes);
        return *bytes;
    }
};
class TestStore final : public WorkerStore {
public:
    explicit TestStore(std::shared_ptr<Disk> disk) : disk_(std::move(disk)) {}
    ~TestStore() override {
        std::lock_guard<std::mutex> lock(disk_->mutex);
        disk_->destroyed = std::this_thread::get_id();
    }
    LoadResult Load(FacePersistentState& state, Bytes& encoded) override {
        std::unique_lock<std::mutex> lock(disk_->mutex);
        disk_->threads.push_back(std::this_thread::get_id());
        ++disk_->loads;
        Gate(lock, disk_->block_load);
        if (disk_->load_fault)
            return *disk_->load_fault;
        if (!disk_->bytes)
            return LoadResult::Absent;
        FacePersistentState parsed;
        if (Decode(disk_->bytes->data(), disk_->bytes->size(), Enrolled(), parsed) !=
            CodecResult::Accepted)
            return LoadResult::Corrupt;
        state = std::move(parsed);
        encoded = *disk_->bytes;
        return LoadResult::Present;
    }
    SaveResult Transition(const Bytes* expected, const FacePersistentState& state,
                          Bytes& verified) override {
        std::unique_lock<std::mutex> lock(disk_->mutex);
        disk_->threads.push_back(std::this_thread::get_id());
        ++disk_->writes;
        assert(expected ? disk_->bytes && *expected == *disk_->bytes : !disk_->bytes);
        auto bytes = EncodeState(state);
        SaveResult result = SaveResult::Saved;
        switch (disk_->save_fault) {
            case SaveFault::None:
                disk_->bytes = bytes;
                verified = bytes;
                break;
            case SaveFault::UncertainCandidate:
                disk_->bytes = bytes;
                result = SaveResult::Uncertain;
                break;
            case SaveFault::UncertainPrior:
                result = SaveResult::Uncertain;
                break;
            case SaveFault::WrongSuccess:
                disk_->bytes = bytes;
                verified = {1, 2, 3};
                break;
            case SaveFault::IoError:
                result = SaveResult::IoError;
                break;
            case SaveFault::Conflict:
                result = SaveResult::Conflict;
                break;
            case SaveFault::Corrupt:
                result = SaveResult::Corrupt;
                break;
        }
        // Delay completion AFTER the write: publication must still await exact
        // storage verification, and admission must remain bounded during it.
        Gate(lock, disk_->block_save);
        return result;
    }

private:
    std::shared_ptr<Disk> disk_;
    void Gate(std::unique_lock<std::mutex>& lock, bool& enabled) {
        if (!enabled)
            return;
        disk_->entered = true;
        disk_->changed.notify_all();
        assert(disk_->changed.wait_for(lock, 5s, [&] { return disk_->release; }));
        enabled = false;
        disk_->entered = disk_->release = false;
    }
};
using Publication = std::shared_ptr<const WorkerPublication>;
Publication Take(ServiceScheduleWorker& worker) {
    const auto limit = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < limit) {
        if (auto result = worker.TryTakePublication())
            return result;
        std::this_thread::sleep_for(1ms);
    }
    assert(false && "worker publication timed out");
    return {};
}
WorkerCommand Command(WorkerCommandKind kind, const Publication& current) {
    WorkerCommand command;
    command.kind = kind;
    command.token = current->token;
    return command;
}
Publication Send(ServiceScheduleWorker& worker, WorkerCommand command) {
    // A concurrent publication retrieval can briefly contend with release of the
    // worker's lock. Retry only bounded admission pressure, never a storage save.
    const auto limit = std::chrono::steady_clock::now() + 5s;
    WorkerAdmission accepted;
    do {
        accepted = worker.Submit(command);
        if (accepted != WorkerAdmission::Busy)
            break;
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < limit);
    assert(accepted == WorkerAdmission::Accepted);
    return Take(worker);
}
Publication Apply(ServiceScheduleWorker& worker, const Publication& current,
                  const Snapshot& snapshot, int64_t monotonic = 0) {
    auto command = Command(WorkerCommandKind::Snapshot, current);
    command.snapshot = snapshot;
    command.monotonic_ms = monotonic;
    return Send(worker, command);
}
Publication Tick(ServiceScheduleWorker& worker, const Publication& current, int64_t monotonic) {
    auto command = Command(WorkerCommandKind::Tick, current);
    command.monotonic_ms = monotonic;
    return Send(worker, command);
}
std::unique_ptr<WorkerStore> Store(const std::shared_ptr<Disk>& disk) {
    return std::make_unique<TestStore>(disk);
}

void AsyncAdmissionAndVerifiedPublication() {
    auto disk = std::make_shared<Disk>();
    disk->block_load = true;
    ServiceScheduleWorker worker(Enrolled(), 101, Store(disk));
    disk->WaitEntered();
    WorkerCommand command;
    command.token = {101, 0};
    assert(worker.Submit(command) == WorkerAdmission::Busy);
    assert(!worker.TryTakePublication());
    disk->Release();
    auto initial = Take(worker);
    assert(initial->status == WorkerStatus::BootAbsent && !initial->face->scheduler().snapshot());
    {
        std::lock_guard<std::mutex> lock(disk->mutex);
        disk->block_save = true;
    }
    command = Command(WorkerCommandKind::Snapshot, initial);
    command.snapshot = SnapshotForTest();
    assert(worker.Submit(command) == WorkerAdmission::Accepted);
    disk->WaitEntered();
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i)
        assert(worker.Submit(command) == WorkerAdmission::Busy);
    assert(std::chrono::steady_clock::now() - start < 500ms);
    assert(!worker.TryTakePublication());
    assert(DecodeState(disk->BytesOnDisk()).schedule.snapshot.snapshot_revision == 1);
    disk->Release();
    // A completed publication cannot be overwritten by the next command.
    std::this_thread::sleep_for(10ms);
    assert(worker.Submit(command) == WorkerAdmission::Busy);
    auto applied = Take(worker);
    assert(applied->status == WorkerStatus::Applied && applied->sequence > initial->sequence);
    assert(!initial->face->scheduler().snapshot());  // old publication is immutable
    assert(!applied->face->scheduler().connected());
    worker.RequestStop();
    worker.Join();
    for (const auto id : disk->threads)
        assert(id != std::this_thread::get_id() && id == disk->destroyed);
    assert(disk->writes == 1);
}

void OfflineProjectionAndSixExactAcks() {
    auto disk = std::make_shared<Disk>();
    ServiceScheduleWorker worker(Enrolled(), 102, Store(disk));
    auto current = Apply(worker, Take(worker), SnapshotForTest());
    for (int64_t i = 1; i < 10; ++i)
        current = Tick(worker, current, i * 100);
    assert(disk->Writes() == 1 && current->face->due().empty());
    assert(current->face->scheduler().now_ms() == kNow + 900);
    current = Tick(worker, current, 1000);
    assert(disk->Writes() == 2 && current->face->due().size() == 6);
    const auto six_due = current;
    auto ack = Command(WorkerCommandKind::Acknowledge, current);
    ack.key = current->face->scheduler().items()[4].key;
    current = Send(worker, ack);
    assert(current->status == WorkerStatus::Acknowledged);
    assert(current->face->pending().size() == 1 && current->face->pending()[0].id == Id(14));
    assert(current->face->due().size() == 5 && current->face->alarm_active());
    assert(!current->face->receipt_confirmed() && six_due->face->due().size() == 6);
    assert(disk->Writes() == 3);
    const auto persisted = DecodeState(disk->BytesOnDisk());
    assert(persisted.pending[0].id == Id(14) && persisted.schedule.items[4].acknowledged);
    ack.token = current->token;
    current = Send(worker, ack);
    assert(current->status == WorkerStatus::RejectedKey && disk->Writes() == 3);
    for (int64_t i = 2; i < 20; ++i)
        current = Tick(worker, current, i * 1000);
    assert(disk->Writes() == 3);
    current = Tick(worker, current, 18000);
    assert(current->status == WorkerStatus::InvalidClock && current->face->due().size() == 5);
    assert(disk->Writes() == 3);
}

void ExactSelectionAcrossDueAndStaleSchedule() {
    auto disk = std::make_shared<Disk>();
    ServiceScheduleWorker worker(Enrolled(), 103, Store(disk));
    auto snapshot = SnapshotForTest(2, 2000);
    snapshot.timers[1].deadline_ms = kNow + 1000;
    auto current = Apply(worker, Take(worker), snapshot);
    auto one_due = Tick(worker, current, 1000);
    auto selected = Command(WorkerCommandKind::Acknowledge, one_due);
    selected.key = one_due->face->scheduler().items()[1].key;
    current = Tick(worker, one_due, 2000);
    assert(current->face->due().size() == 2);
    current = Send(worker, selected);
    assert(current->face->pending()[0].id == Id(11));
    assert(!current->face->scheduler().items()[0].acknowledged);
    auto next = snapshot;
    next.snapshot_revision = 2;
    next.server_now_ms = kNow + 2000;
    current = Apply(worker, current, next, 2000);
    assert(current->status == WorkerStatus::Applied);
    assert(worker.Submit(selected) == WorkerAdmission::Stale);
    selected.token = current->token;
    selected.token.owner_generation = 999;
    assert(worker.Submit(selected) == WorkerAdmission::Stale);
    selected.token = current->token;
    selected.key.revision = 999;
    current = Send(worker, selected);
    assert(current->status == WorkerStatus::RejectedKey && current->face->due().size() == 1);
}

void BootRestoreNeedsSyncAndReceipt() {
    auto disk = std::make_shared<Disk>();
    {
        ServiceScheduleWorker worker(Enrolled(), 104, Store(disk));
        auto current = Apply(worker, Take(worker), SnapshotForTest(6, 0));
        auto ack = Command(WorkerCommandKind::Acknowledge, current);
        ack.key = current->face->scheduler().items()[3].key;
        current = Send(worker, ack);
        assert(current->face->due().size() == 5);
    }
    ServiceScheduleWorker worker(Enrolled(), 105, Store(disk));
    auto current = Take(worker);
    assert(current->status == WorkerStatus::BootPresent && current->face->pending().size() == 1);
    assert(current->face->scheduler().clock_state() == ClockState::AwaitingFreshTime);
    assert(!current->face->scheduler().connected() && !current->face->receipt_confirmed());
    const auto count = disk->Writes();
    current = Tick(worker, current, 999000);
    assert(current->face->scheduler().now_ms() == kNow && disk->Writes() == count);
    current = Apply(worker, current, SnapshotForTest(6, 0));
    assert(current->status == WorkerStatus::Replay);
    assert(current->face->scheduler().clock_state() == ClockState::AwaitingFreshTime);
    auto receipt = Command(WorkerCommandKind::Receipt, current);
    receipt.key = current->face->pending()[0];
    current = Send(worker, receipt);
    assert(current->status == WorkerStatus::RejectedKey);
    auto connect = Command(WorkerCommandKind::Connection, current);
    connect.connected = true;
    current = Send(worker, connect);
    assert(current->face->scheduler().connected() && disk->Writes() == count);
    receipt.token = current->token;
    current = Send(worker, receipt);
    assert(current->status == WorkerStatus::ReceiptAccepted && current->face->receipt_confirmed());
    assert(current->face->pending().empty() && DecodeState(disk->BytesOnDisk()).pending.empty());
    assert(current->face->due().size() == 5 && disk->Writes() == count + 1);
}

void FreshSnapshotAndReplay() {
    auto disk = std::make_shared<Disk>();
    disk->bytes = Record(SnapshotForTest(1));
    ServiceScheduleWorker worker(Enrolled(), 106, Store(disk));
    auto current = Take(worker);
    auto fresh = SnapshotForTest(1);
    fresh.snapshot_revision = 2;
    fresh.server_now_ms = kNow + 5000;
    current = Apply(worker, current, fresh, 7);
    assert(current->status == WorkerStatus::Applied && current->face->due().size() == 1);
    assert(current->face->scheduler().clock_state() == ClockState::Trusted);
    current = Tick(worker, current, 1007);
    assert(current->face->scheduler().now_ms() == kNow + 6000);
    const auto writes = disk->Writes();
    current = Apply(worker, current, fresh, 1007);
    assert(current->status == WorkerStatus::Replay && disk->Writes() == writes);
    assert(current->face->scheduler().now_ms() == kNow + 6000);
    fresh.snapshot_revision = 3;
    fresh.server_now_ms = kNow + 1;
    current = Apply(worker, current, fresh, 1007);
    assert(current->status == WorkerStatus::InvalidClock);
    assert(current->token.snapshot_revision == 2 && disk->Writes() == writes);
}

void LimitsAndAtomicRejection() {
    auto disk = std::make_shared<Disk>();
    ServiceScheduleWorker worker(Enrolled(), 107, Store(disk));
    auto current = Take(worker);
    auto bad = Command(WorkerCommandKind::Snapshot, current);
    bad.snapshot = SnapshotForTest(7);
    assert(worker.Submit(bad) == WorkerAdmission::Invalid && disk->Writes() == 0);
    bad.snapshot = SnapshotForTest();
    bad.snapshot.timers[0].label = std::string(321, 'x');
    assert(worker.Submit(bad) == WorkerAdmission::Invalid);
    bad.snapshot = SnapshotForTest();
    bad.snapshot.timers[0].label = "\xff";
    current = Send(worker, bad);
    assert(current->status == WorkerStatus::RejectedSnapshot && disk->Writes() == 0);
    current = Apply(worker, current, SnapshotForTest(6, 0));
    for (size_t i = 0; i < 6; ++i) {
        auto ack = Command(WorkerCommandKind::Acknowledge, current);
        ack.key = current->face->scheduler().items()[i].key;
        current = Send(worker, ack);
    }
    assert(current->face->pending().size() == 6 && current->face->due().empty());
    auto next = SnapshotForTest(1, 0);
    next.snapshot_revision = 2;
    next.timers[0].id = Id(99);
    current = Apply(worker, current, next);
    assert(current->status == WorkerStatus::Applied && current->face->pending().size() == 6);
    const auto before = disk->BytesOnDisk();
    auto ack = Command(WorkerCommandKind::Acknowledge, current);
    ack.key = current->face->scheduler().items()[0].key;
    current = Send(worker, ack);
    assert(current->status == WorkerStatus::LimitExceeded && current->face->due().size() == 1);
    assert(disk->BytesOnDisk() == before);
}

void UncertainCandidateAndPrior(bool candidate_on_disk, bool prior_present) {
    auto disk = std::make_shared<Disk>();
    if (prior_present)
        disk->bytes = Record(SnapshotForTest(1, 0));
    ServiceScheduleWorker worker(Enrolled(), 108, Store(disk));
    auto current = Take(worker);
    {
        std::lock_guard<std::mutex> lock(disk->mutex);
        disk->save_fault =
            candidate_on_disk ? SaveFault::UncertainCandidate : SaveFault::UncertainPrior;
    }
    if (prior_present) {
        auto ack = Command(WorkerCommandKind::Acknowledge, current);
        ack.key = current->face->scheduler().items()[0].key;
        current = Send(worker, ack);
    } else {
        current = Apply(worker, current, SnapshotForTest(1, 0));
    }
    assert(current->status == WorkerStatus::Uncertain && current->recovery_required);
    assert(current->face->pending().empty());
    const auto writes = disk->Writes();
    for (const auto kind :
         {WorkerCommandKind::Snapshot, WorkerCommandKind::Tick, WorkerCommandKind::Acknowledge,
          WorkerCommandKind::Connection, WorkerCommandKind::Receipt}) {
        auto command = Command(kind, current);
        assert(worker.Submit(command) == WorkerAdmission::RecoveryRequired);
    }
    std::this_thread::sleep_for(10ms);
    assert(disk->Writes() == writes);
    current = Send(worker, Command(WorkerCommandKind::Reconcile, current));
    assert(current->status ==
           (candidate_on_disk ? WorkerStatus::ReconciledCandidate : WorkerStatus::ReconciledPrior));
    assert(!current->recovery_required && !current->face->receipt_confirmed());
    assert(!current->face->scheduler().connected() && disk->Writes() == writes);
    if (candidate_on_disk || prior_present) {
        assert(current->face->scheduler().clock_state() == ClockState::AwaitingFreshTime);
        assert(current->face->pending().size() == (candidate_on_disk && prior_present ? 1 : 0));
    } else {
        assert(!current->face->scheduler().snapshot());
    }
}

void WrongReadbackAndFaultFence() {
    for (const auto fault :
         {SaveFault::WrongSuccess, SaveFault::IoError, SaveFault::Conflict, SaveFault::Corrupt}) {
        auto disk = std::make_shared<Disk>();
        ServiceScheduleWorker worker(Enrolled(), 109, Store(disk));
        auto current = Take(worker);
        {
            std::lock_guard<std::mutex> lock(disk->mutex);
            disk->save_fault = fault;
        }
        current = Apply(worker, current, SnapshotForTest(1, 0));
        assert(current->recovery_required && !current->face->scheduler().snapshot());
        const auto candidate = Record(SnapshotForTest(1, 0));
        {
            std::lock_guard<std::mutex> lock(disk->mutex);
            auto different = SnapshotForTest(1, 0);
            different.snapshot_revision = 20;
            disk->bytes = Record(different);
        }
        current = Send(worker, Command(WorkerCommandKind::Reconcile, current));
        assert(current->status == WorkerStatus::ReconciliationMismatch &&
               current->recovery_required);
        {
            std::lock_guard<std::mutex> lock(disk->mutex);
            disk->bytes = Bytes{1, 2, 3};
        }
        current = Send(worker, Command(WorkerCommandKind::Reconcile, current));
        assert(current->status == WorkerStatus::StorageCorrupt && current->recovery_required);
        {
            std::lock_guard<std::mutex> lock(disk->mutex);
            disk->load_fault = LoadResult::IoError;
        }
        current = Send(worker, Command(WorkerCommandKind::Reconcile, current));
        assert(current->status == WorkerStatus::StorageIoError && current->recovery_required);
        {
            std::lock_guard<std::mutex> lock(disk->mutex);
            disk->load_fault.reset();
            disk->bytes = candidate;
        }
        current = Send(worker, Command(WorkerCommandKind::Reconcile, current));
        assert(current->status == WorkerStatus::ReconciledCandidate && !current->recovery_required);
        assert(disk->Writes() == 1);
    }
}

void DueWriteFailureAndReceiptUncertainty() {
    auto disk = std::make_shared<Disk>();
    ServiceScheduleWorker worker(Enrolled(), 113, Store(disk));
    auto current = Apply(worker, Take(worker), SnapshotForTest(2));
    {
        std::lock_guard<std::mutex> lock(disk->mutex);
        disk->save_fault = SaveFault::UncertainPrior;
    }
    current = Tick(worker, current, 1000);
    assert(current->status == WorkerStatus::Uncertain && current->face->due().empty());
    assert(current->recovery_required);  // Do not claim a durable due transition.
    assert(!DecodeState(disk->BytesOnDisk()).schedule.items[0].due);
    current = Send(worker, Command(WorkerCommandKind::Reconcile, current));
    assert(current->status == WorkerStatus::ReconciledPrior);
    assert(current->face->scheduler().clock_state() == ClockState::AwaitingFreshTime);
    {
        std::lock_guard<std::mutex> lock(disk->mutex);
        disk->save_fault = SaveFault::None;
    }
    auto fresh = SnapshotForTest(2);
    fresh.snapshot_revision = 2;
    fresh.server_now_ms = kNow + 1000;
    current = Apply(worker, current, fresh, 1000);
    assert(current->face->due().size() == 2);
    auto ack = Command(WorkerCommandKind::Acknowledge, current);
    ack.key = current->face->scheduler().items()[1].key;
    current = Send(worker, ack);
    auto connection = Command(WorkerCommandKind::Connection, current);
    connection.connected = true;
    current = Send(worker, connection);
    auto receipt = Command(WorkerCommandKind::Receipt, current);
    receipt.key = current->face->pending()[0];
    {
        std::lock_guard<std::mutex> lock(disk->mutex);
        disk->save_fault = SaveFault::UncertainCandidate;
    }
    current = Send(worker, receipt);
    assert(current->status == WorkerStatus::Uncertain && current->face->pending().size() == 1);
    assert(!current->face->receipt_confirmed());
    assert(DecodeState(disk->BytesOnDisk()).pending.empty());
    current = Send(worker, Command(WorkerCommandKind::Reconcile, current));
    assert(current->status == WorkerStatus::ReconciledCandidate &&
           current->face->pending().empty());
    assert(!current->face->receipt_confirmed() && !current->face->scheduler().connected());
    assert(current->face->due().size() == 1);  // Independent unacknowledged timer survives.
}

void StopDuringBootAndMalformedRestoration() {
    auto disk = std::make_shared<Disk>();
    disk->block_load = true;
    {
        ServiceScheduleWorker worker(Enrolled(), 114, Store(disk));
        disk->WaitEntered();
        worker.RequestStop();
        disk->Release();
        worker.Join();
        assert(!worker.TryTakePublication() && disk->Writes() == 0);
    }
    // A lying adapter's Present result is independently checked against both
    // the enrolled scope and canonical bytes before the model can be published.
    class BadStore : public WorkerStore {
    public:
        LoadResult Load(FacePersistentState& state, Bytes& bytes) override {
            bytes = Record(SnapshotForTest(1));
            state = DecodeState(bytes);
            state.schedule.snapshot.scope.device_id = Id(99);
            return LoadResult::Present;
        }
        SaveResult Transition(const Bytes*, const FacePersistentState&, Bytes&) override {
            assert(false);
            return SaveResult::InvalidState;
        }
    };
    ServiceScheduleWorker bad(Enrolled(), 115, std::make_unique<BadStore>());
    const auto current = Take(bad);
    assert(current->status == WorkerStatus::BootCorrupt && current->recovery_required);
    assert(!current->face->scheduler().snapshot());
}

void BootFailureDistinction() {
    for (const auto fault : {LoadResult::Corrupt, LoadResult::IoError}) {
        auto disk = std::make_shared<Disk>();
        disk->load_fault = fault;
        ServiceScheduleWorker worker(Enrolled(), 110, Store(disk));
        auto current = Take(worker);
        assert(current->status == (fault == LoadResult::Corrupt ? WorkerStatus::BootCorrupt
                                                                : WorkerStatus::BootIoError));
        auto command = Command(WorkerCommandKind::Snapshot, current);
        command.snapshot = SnapshotForTest();
        assert(worker.Submit(command) == WorkerAdmission::RecoveryRequired && disk->Writes() == 0);
        {
            std::lock_guard<std::mutex> lock(disk->mutex);
            disk->load_fault.reset();
            disk->bytes = Record(SnapshotForTest(1));
        }
        current = Send(worker, Command(WorkerCommandKind::Reconcile, current));
        assert(current->status == WorkerStatus::BootPresent && !current->recovery_required);
        assert(current->face->scheduler().clock_state() == ClockState::AwaitingFreshTime);
    }
    ServiceScheduleWorker invalid(Enrolled(), 0, nullptr);
    auto result = Take(invalid);
    assert(result->status == WorkerStatus::InvalidConfiguration);
    result = Send(invalid, Command(WorkerCommandKind::Reconcile, result));
    assert(result->status == WorkerStatus::InvalidConfiguration);
}

void IdleStopWaitBoundary() {
    // Repeatedly request stop as boot publication hands ownership back, and
    // while the worker is already idle. Caller stop remains nonblocking; Join
    // must finish even if the stop notification races entry into the wait.
    for (unsigned i = 0; i < 128; ++i) {
        auto disk = std::make_shared<Disk>();
        ServiceScheduleWorker worker(Enrolled(), 200 + i, Store(disk));
        auto current = Take(worker);
        if (i % 2)
            std::this_thread::sleep_for(1ms);
        const auto start = std::chrono::steady_clock::now();
        worker.RequestStop();
        assert(worker.Submit(Command(WorkerCommandKind::Tick, current)) ==
               WorkerAdmission::Stopped);
        assert(std::chrono::steady_clock::now() - start < 500ms);
        worker.Join();
        assert(std::chrono::steady_clock::now() - start < 1000ms);
        assert(!worker.TryTakePublication() && disk->Writes() == 0);
    }
}

void StopDuringDelayedCompletionAndNewOwner() {
    auto disk = std::make_shared<Disk>();
    ServiceScheduleWorker worker(Enrolled(), 111, Store(disk));
    auto initial = Take(worker);
    {
        std::lock_guard<std::mutex> lock(disk->mutex);
        disk->block_save = true;
    }
    auto command = Command(WorkerCommandKind::Snapshot, initial);
    command.snapshot = SnapshotForTest(1);
    assert(worker.Submit(command) == WorkerAdmission::Accepted);
    disk->WaitEntered();
    const auto start = std::chrono::steady_clock::now();
    worker.RequestStop();
    assert(std::chrono::steady_clock::now() - start < 500ms);
    assert(worker.Submit(command) == WorkerAdmission::Stopped);
    assert(!worker.TryTakePublication());
    disk->Release();
    worker.Join();
    assert(!worker.TryTakePublication() && !initial->face->scheduler().snapshot());
    ServiceScheduleWorker newer(Enrolled(), 112, Store(disk));
    auto current = Take(newer);
    assert(current->status == WorkerStatus::BootPresent && current->token.owner_generation == 112);
    command.token = {111, 1};
    assert(newer.Submit(command) == WorkerAdmission::Stale);
}
Snapshot V2Snapshot() {
    auto s = SnapshotForTest();
    s.version = 2;
    s.service_occurrence_id.clear();
    s.service_revision = 0;
    s.service_at_ms = s.server_now_ms = 0;
    s.timezone.clear();
    return s;
}
Publication V2Connect(ServiceScheduleWorker& worker, Publication current,
                      std::string session = Id(4)) {
    auto c = Command(WorkerCommandKind::Connection, current);
    c.connected = true;
    c.clock_session_id = session;
    return Send(worker, c);
}
ClockRequest V2Request(uint64_t revision = 1, unsigned nonce = 99) {
    return {Enrolled(), Id(4), Id(nonce), revision};
}
Publication V2Begin(ServiceScheduleWorker& worker, Publication current, const ClockRequest& request,
                    int64_t mono = 1000) {
    auto c = Command(WorkerCommandKind::BeginClockRequest, current);
    c.clock_request = request;
    c.monotonic_ms = mono;
    return Send(worker, c);
}
WorkerCommand V2Clock(Publication current, const ClockRequest& request, int64_t epoch,
                      int64_t mono = 1000) {
    auto c = Command(WorkerCommandKind::AcceptClock, current);
    c.clock_response = {request, epoch};
    c.monotonic_ms = mono;
    return c;
}
void V2OrdinaryTimeWithoutFlashAndReboot() {
    auto disk = std::make_shared<Disk>();
    {
        ServiceScheduleWorker worker(Enrolled(), 600, Store(disk));
        auto p = Apply(worker, Take(worker), V2Snapshot());
        assert(p->status == WorkerStatus::Applied && disk->Writes() == 1);
        assert(p->face->scheduler().clock_state() == ClockState::AwaitingFreshTime);
        p = V2Connect(worker, p);
        p = V2Begin(worker, p, V2Request());
        assert(p->status == WorkerStatus::ClockRequested);
        p = Send(worker, V2Clock(p, V2Request(), kNow));
        assert(p->status == WorkerStatus::ClockAccepted && disk->Writes() == 1);
        assert(p->face->scheduler().clock_state() == ClockState::Trusted && p->face->due().empty());
        p = Tick(worker, p, 1500);
        assert(p->face->scheduler().now_ms() == kNow + 500 && disk->Writes() == 1);
        assert(DecodeState(disk->BytesOnDisk()).schedule.last_known_epoch_ms == 0);
    }
    ServiceScheduleWorker worker(Enrolled(), 601, Store(disk));
    auto p = Take(worker);
    assert(p->status == WorkerStatus::BootPresent && !p->face->scheduler().connected());
    assert(p->face->scheduler().clock_state() == ClockState::AwaitingFreshTime);
    p = Send(worker, V2Clock(p, V2Request(), kNow + 1000));
    assert(p->status == WorkerStatus::ClockRejected && disk->Writes() == 1);
    auto downgrade = SnapshotForTest();
    downgrade.snapshot_revision = 2;
    p = Apply(worker, p, downgrade);
    assert(p->status == WorkerStatus::RejectedSnapshot && disk->Writes() == 1);
    p = V2Connect(worker, p);
    p = V2Begin(worker, p, V2Request(1, 100));
    p = Send(worker, V2Clock(p, V2Request(1, 100), kNow + 1000));
    assert(p->status == WorkerStatus::ClockAccepted && p->face->due().size() == 6 &&
           disk->Writes() == 2);
    assert(!p->face->receipt_confirmed());
}
void V2DueWaitsForVerifiedStorage() {
    auto disk = std::make_shared<Disk>();
    ServiceScheduleWorker worker(Enrolled(), 602, Store(disk));
    auto p = V2Connect(worker, Apply(worker, Take(worker), V2Snapshot()));
    p = V2Begin(worker, p, V2Request());
    {
        std::lock_guard<std::mutex> lock(disk->mutex);
        disk->block_save = true;
    }
    auto c = V2Clock(p, V2Request(), kNow + 1000);
    assert(worker.Submit(c) == WorkerAdmission::Accepted);
    disk->WaitEntered();
    assert(!worker.TryTakePublication() && p->face->due().empty());
    assert(worker.Submit(c) == WorkerAdmission::Busy);
    disk->Release();
    p = Take(worker);
    assert(p->status == WorkerStatus::ClockAccepted && p->face->due().size() == 6);
    auto selected = p->face->scheduler().items()[0].key;
    c = Command(WorkerCommandKind::Acknowledge, p);
    c.key = selected;
    p = Send(worker, c);
    assert(p->face->due().size() == 5 && p->face->pending().size() == 1);
    c.token = p->token;
    p = Send(worker, c);
    assert(p->status == WorkerStatus::RejectedKey && p->face->due().size() == 5);
    p = Send(worker, V2Clock(p, V2Request(), kNow + 1000));
    assert(p->status == WorkerStatus::ClockRejected && disk->Writes() == 3);
}
void V2FailedDueConsumesNonceAndUncertainRestoresUntrusted() {
    for (auto fault :
         {SaveFault::IoError, SaveFault::UncertainCandidate, SaveFault::UncertainPrior}) {
        auto disk = std::make_shared<Disk>();
        ServiceScheduleWorker worker(Enrolled(), 603, Store(disk));
        auto p = V2Connect(worker, Apply(worker, Take(worker), V2Snapshot()));
        p = V2Begin(worker, p, V2Request());
        {
            std::lock_guard<std::mutex> lock(disk->mutex);
            disk->save_fault = fault;
        }
        p = Send(worker, V2Clock(p, V2Request(), kNow + 1000));
        assert(p->face->due().empty() && !p->face->receipt_confirmed() && disk->Writes() == 2);
        assert(p->status == (fault == SaveFault::IoError ? WorkerStatus::StorageIoError
                                                         : WorkerStatus::Uncertain));
        assert(p->recovery_required);
        assert(worker.Submit(V2Clock(p, V2Request(), kNow + 1000)) ==
               WorkerAdmission::RecoveryRequired);
        p = Send(worker, Command(WorkerCommandKind::Reconcile, p));
        assert(p->status == (fault == SaveFault::UncertainCandidate
                                 ? WorkerStatus::ReconciledCandidate
                                 : WorkerStatus::ReconciledPrior));
        assert(p->face->scheduler().clock_state() == ClockState::AwaitingFreshTime &&
               !p->face->scheduler().connected());
        assert(p->face->due().size() == (fault == SaveFault::UncertainCandidate ? 6 : 0));
        p = V2Connect(worker, p);
        p = Send(worker, V2Clock(p, V2Request(), kNow + 1000));
        assert(p->status == WorkerStatus::ClockRejected && disk->Writes() == 2);
        {
            std::lock_guard<std::mutex> lock(disk->mutex);
            disk->save_fault = SaveFault::None;
        }
        p = V2Begin(worker, p, V2Request(1, 100), 2000);
        p = Send(worker, V2Clock(p, V2Request(1, 100), kNow + 1000, 2000));
        assert(p->status == WorkerStatus::ClockAccepted && p->face->due().size() == 6);
        assert(disk->Writes() == (fault == SaveFault::UncertainCandidate ? 2 : 3));
    }
}
void V2SessionAndRevisionFences() {
    auto disk = std::make_shared<Disk>();
    ServiceScheduleWorker worker(Enrolled(), 604, Store(disk));
    auto p = V2Connect(worker, Apply(worker, Take(worker), V2Snapshot()));
    p = V2Begin(worker, p, V2Request());
    p = V2Connect(worker, p, "");
    assert(p->status == WorkerStatus::ClockRejected && !p->face->scheduler().connected());
    p = Send(worker, V2Clock(p, V2Request(), kNow));
    assert(p->status == WorkerStatus::ClockRejected);
    p = V2Connect(worker, p);
    p = V2Begin(worker, p, V2Request(1, 100));
    auto old = V2Clock(p, V2Request(1, 100), kNow);
    auto next = V2Snapshot();
    next.snapshot_revision++;
    p = Apply(worker, p, next, 1500);
    assert(worker.Submit(old) == WorkerAdmission::Stale);
    old.token = p->token;
    p = Send(worker, old);
    assert(p->status == WorkerStatus::ClockRejected);
    p = V2Begin(worker, p, V2Request(2, 101), 2000);
    p = V2Begin(worker, p, V2Request(2, 102), 4000);
    assert(p->status == WorkerStatus::ClockRejected);
    p = V2Begin(worker, p, V2Request(2, 102), 4001);
    assert(p->status == WorkerStatus::ClockRequested);
    p = Send(worker, V2Clock(p, V2Request(2, 101), kNow, 4002));
    assert(p->status == WorkerStatus::ClockRejected);
    p = Send(worker, V2Clock(p, V2Request(2, 102), kNow, 6001));
    assert(p->status == WorkerStatus::ClockAccepted && disk->Writes() == 2);
}

}  // namespace

int main() {
    AsyncAdmissionAndVerifiedPublication();
    OfflineProjectionAndSixExactAcks();
    ExactSelectionAcrossDueAndStaleSchedule();
    BootRestoreNeedsSyncAndReceipt();
    FreshSnapshotAndReplay();
    LimitsAndAtomicRejection();
    for (const bool candidate : {false, true})
        for (const bool prior : {false, true})
            UncertainCandidateAndPrior(candidate, prior);
    WrongReadbackAndFaultFence();
    BootFailureDistinction();
    DueWriteFailureAndReceiptUncertainty();
    StopDuringBootAndMalformedRestoration();
    StopDuringDelayedCompletionAndNewOwner();
    IdleStopWaitBoundary();
    V2OrdinaryTimeWithoutFlashAndReboot();
    V2DueWaitsForVerifiedStorage();
    V2FailedDueConsumesNonceAndUncertainRestoresUntrusted();
    V2SessionAndRevisionFences();
    std::puts("20 threaded worker scenarios passed");
}
