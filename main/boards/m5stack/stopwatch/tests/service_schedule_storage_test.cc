#include "service_schedule_nvs_store.h"

#include <nvs.h>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace orbit::service_schedule;
using namespace orbit::service_schedule::storage;

namespace {
std::string Id(unsigned value) {
    char id[37];
    std::snprintf(id, sizeof(id), "00000000-0000-4000-8000-%012x", value);
    return id;
}
Scope ScopeForTest() { return {Id(1), Id(2)}; }
constexpr int64_t kNow = 1788883200000LL;

FacePersistentState State(size_t cues = 2, size_t timers = 4) {
    FacePersistentState value;
    auto& snapshot = value.schedule.snapshot;
    snapshot.scope = ScopeForTest();
    snapshot.service_occurrence_id = Id(3);
    snapshot.service_revision = 7;
    snapshot.snapshot_revision = 9;
    snapshot.service_at_ms = kNow + 3600000;
    snapshot.server_now_ms = kNow;
    snapshot.timezone = "Europe/Brussels";
    value.schedule.last_known_epoch_ms = kNow;
    for (size_t i = 0; i < cues; ++i) {
        const auto kind = i % 2 ? CueKind::Fixed : CueKind::ServiceOffset;
        const auto deadline = kind == CueKind::Fixed ? kNow : snapshot.service_at_ms - 3600000;
        snapshot.cues.push_back({Id(static_cast<unsigned>(10 + i)), 2, "Setup", kind, deadline,
                                 kind == CueKind::Fixed ? 0 : -3600000});
        value.schedule.items.push_back(
            {{snapshot.scope, ItemKind::Cue, snapshot.service_occurrence_id,
              snapshot.cues.back().id, 2},
             true,
             false});
    }
    for (size_t i = 0; i < timers; ++i) {
        snapshot.timers.push_back({Id(static_cast<unsigned>(100 + i)), 3, "Rice", kNow});
        value.schedule.items.push_back(
            {{snapshot.scope, ItemKind::Timer, {}, snapshot.timers.back().id, 3}, true, false});
    }
    return value;
}
Bytes Encoded(const FacePersistentState& state) {
    Bytes bytes;
    assert(Encode(state, ScopeForTest(), bytes) == CodecResult::Accepted);
    return bytes;
}
FacePersistentState Decoded(const Bytes& bytes) {
    FacePersistentState value;
    assert(Decode(bytes.data(), bytes.size(), ScopeForTest(), value) == CodecResult::Accepted);
    return value;
}

// Independent test checksum helper also exercises corruption with a repaired
// checksum: malformed payloads must be rejected by their actual typed rules.
void Checksum(Bytes& bytes) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i + 4 < bytes.size(); ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = crc & 1 ? (crc >> 1) ^ 0xedb88320u : crc >> 1;
    }
    crc = ~crc;
    for (size_t i = 0; i < 4; ++i)
        bytes[bytes.size() - 4 + i] = static_cast<uint8_t>(crc >> (8 * i));
}
void SetNumber(Bytes& bytes, size_t offset, uint64_t value, size_t count) {
    for (size_t i = 0; i < count; ++i)
        bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
void Reject(const Bytes& bytes, CodecResult expected) {
    auto output = State(0, 1);
    const auto before = Encoded(output);
    assert(Decode(bytes.data(), bytes.size(), ScopeForTest(), output) == expected);
    assert(Encoded(output) == before);
}

void Roundtrip() {
    auto state = State();
    state.schedule.items[0].acknowledged = true;
    state.pending.push_back(state.schedule.items[0].key);
    state.schedule.retired_ids = {Id(200), Id(201)};
    const auto bytes = Encoded(state);
    assert(bytes.size() < kMaximumRecordBytes);
    auto decoded = Decoded(bytes);
    assert(Encoded(decoded) == bytes);
    assert(decoded.pending[0].service_occurrence_id == Id(3));
    assert(decoded.schedule.snapshot.cues[0].offset_ms == -3600000);
    assert(decoded.schedule.snapshot.cues[1].kind == CueKind::Fixed);
    assert(decoded.schedule.retired_ids == state.schedule.retired_ids);
}

void WorstCase() {
    auto state = State(6, 0);
    auto& s = state.schedule.snapshot;
    s.timezone = std::string(64, 'A');  // Typed zone bound; IANA admission is upstream.
    s.service_revision = s.snapshot_revision = kMaximumRevision;
    s.service_at_ms = s.server_now_ms = state.schedule.last_known_epoch_ms = kMaximumEpochMs;
    for (size_t i = 0; i < s.cues.size(); ++i) {
        auto& cue = s.cues[i];
        cue.kind = CueKind::Fixed;
        cue.offset_ms = 0;
        cue.deadline_ms = kMaximumEpochMs;
        cue.revision = kMaximumRevision;
        cue.label.clear();
        for (size_t scalar = 0; scalar < 80; ++scalar)
            cue.label += "\xf0\x9f\x8d\x9a";
        state.schedule.items[i].key.revision = kMaximumRevision;
        state.schedule.items[i].acknowledged = true;
        state.pending.push_back(state.schedule.items[i].key);
    }
    for (unsigned i = 0; i < 64; ++i)
        state.schedule.retired_ids.push_back(Id(200 + i));
    const auto bytes = Encoded(state);
    assert(bytes.size() == kWorstCaseRecordBytes && bytes.size() == 3567);
    const auto restored = Decoded(bytes);
    assert(Encoded(restored) == bytes);
    assert(restored.schedule.snapshot.service_revision == kMaximumRevision);
    assert(restored.schedule.snapshot.snapshot_revision == kMaximumRevision);
    assert(restored.schedule.snapshot.service_at_ms == kMaximumEpochMs);
    assert(restored.schedule.snapshot.server_now_ms == kMaximumEpochMs);
    assert(restored.schedule.last_known_epoch_ms == kMaximumEpochMs);
    assert(restored.schedule.snapshot.cues[5].label.size() == 320);
    assert(restored.schedule.snapshot.cues[5].deadline_ms == kMaximumEpochMs);
    assert(restored.schedule.snapshot.cues[5].revision == kMaximumRevision);
    assert(restored.pending[5].revision == kMaximumRevision);
}

void HistoricalPending() {
    auto state = State(1, 1);
    // Older ACK for a replaced timer revision and a removed occurrence's cue.
    state.pending.push_back({ScopeForTest(), ItemKind::Timer, {}, Id(100), 1});
    state.schedule.retired_ids.push_back(Id(210));
    state.pending.push_back({ScopeForTest(), ItemKind::Cue, Id(211), Id(210), 8});
    const auto bytes = Encoded(state);
    auto restored = Decoded(bytes);
    assert(restored.pending.size() == 2 && restored.pending[0].revision == 1);
    assert(restored.pending[1].id == Id(210) &&
           restored.pending[1].service_occurrence_id == Id(211));
    assert(Encoded(restored) == bytes);
}

void ObservableReboot() {
    auto initial = State(0, 6).schedule.snapshot;
    FaceModel before(ScopeForTest());
    assert(before.ApplyVerified(initial, 0) == ApplyResult::Applied);
    before.SetConnected(true);
    assert(before.due().size() == 6 && before.alarm_active());
    assert(before.AcknowledgeNext());
    FacePersistentState saved;
    assert(before.ExportState(saved));
    auto disk = Encoded(saved);
    FaceModel rebooted(ScopeForTest());
    assert(rebooted.RestoreState(Decoded(disk)));
    assert(!rebooted.scheduler().connected() && !rebooted.receipt_confirmed());
    assert(rebooted.scheduler().clock_state() == ClockState::AwaitingFreshTime);
    assert(rebooted.due().size() == 5 && rebooted.pending().size() == 1);
    assert(!rebooted.AcceptReceipt(rebooted.pending().front()));
    rebooted.Tick(99999999);
    assert(rebooted.scheduler().clock_state() == ClockState::AwaitingFreshTime);
    assert(rebooted.due().size() == 5);
    assert(rebooted.ApplyVerified(initial, 99999999) == ApplyResult::Replay);
    assert(rebooted.scheduler().clock_state() == ClockState::AwaitingFreshTime);
    initial.snapshot_revision++;
    initial.server_now_ms++;
    assert(rebooted.ApplyVerified(initial, 100000000) == ApplyResult::Applied);
    assert(rebooted.scheduler().clock_state() == ClockState::Trusted);
    assert(rebooted.pending().size() == 1 && rebooted.due().size() == 5);
    rebooted.SetConnected(true);
    const auto key = rebooted.pending().front();
    auto wrong = key;
    wrong.revision++;
    assert(!rebooted.AcceptReceipt(wrong) && rebooted.pending().size() == 1);
    assert(rebooted.AcceptReceipt(key) && rebooted.pending().empty());
    assert(rebooted.ExportState(saved));
    FaceModel again(ScopeForTest());
    assert(again.RestoreState(Decoded(Encoded(saved))));
    assert(!again.receipt_confirmed() && again.pending().empty() && again.due().size() == 5);
}

void ExportedPendingAfterEdits() {
    FaceModel model(ScopeForTest());
    auto snapshot = State(1, 1).schedule.snapshot;
    assert(model.ApplyVerified(snapshot, 0) == ApplyResult::Applied);
    assert(model.AcknowledgeNext() && model.AcknowledgeNext());
    const auto old_pending = model.pending();
    snapshot.cues.clear();
    snapshot.timers[0].revision++;
    snapshot.timers[0].deadline_ms += 60000;
    snapshot.snapshot_revision++;
    snapshot.server_now_ms++;
    assert(model.ApplyVerified(snapshot, 1) == ApplyResult::Applied);
    FacePersistentState saved;
    assert(model.ExportState(saved));
    auto restored = Decoded(Encoded(saved));
    assert(restored.pending.size() == 2);
    for (size_t i = 0; i < old_pending.size(); ++i) {
        assert(restored.pending[i].id == old_pending[i].id);
        assert(restored.pending[i].revision == old_pending[i].revision);
        assert(restored.pending[i].service_occurrence_id == old_pending[i].service_occurrence_id);
    }
    assert(restored.schedule.retired_ids == std::vector<std::string>{old_pending[0].id});
    assert(restored.schedule.snapshot.timers[0].revision > restored.pending[1].revision);
}

void NumericEdges() {
    for (const int64_t offset : {-kMaximumOffsetMs, kMaximumOffsetMs}) {
        auto state = State(1, 0);
        auto& snapshot = state.schedule.snapshot;
        snapshot.service_at_ms = kNow + kMaximumOffsetMs;
        snapshot.cues[0].offset_ms = offset;
        snapshot.cues[0].deadline_ms = snapshot.service_at_ms + offset;
        state.schedule.last_known_epoch_ms = snapshot.cues[0].deadline_ms;
        const auto bytes = Encoded(state);
        assert(Decoded(bytes).schedule.snapshot.cues[0].offset_ms == offset);
        assert(Encoded(Decoded(bytes)) == bytes);
    }
    auto state = State(1, 0);
    auto& snapshot = state.schedule.snapshot;
    snapshot.service_at_ms = snapshot.server_now_ms = state.schedule.last_known_epoch_ms = 1;
    snapshot.cues[0].deadline_ms = 1;
    snapshot.cues[0].offset_ms = 0;
    snapshot.cues[0].revision = state.schedule.items[0].key.revision = 1;
    assert(Decoded(Encoded(state)).schedule.snapshot.cues[0].deadline_ms == 1);
}

void CountsAndBounds() {
    auto zero = State(0, 0);
    assert(Encoded(Decoded(Encoded(zero))) == Encoded(zero));
    for (size_t count = 1; count <= 6; ++count) {
        auto state = State(0, count);
        assert(Decoded(Encoded(state)).schedule.items.size() == count);
    }
    const std::vector<std::function<void(FacePersistentState&)>> invalid = {
        [](auto& s) { s = State(0, 7); },
        [](auto& s) { s = State(7, 0); },
        [](auto& s) { s.pending.resize(7); },
        [](auto& s) { s.schedule.retired_ids.resize(65); },
        [](auto& s) { s.schedule.items.resize(7); },
    };
    for (const auto& change : invalid) {
        auto state = State();
        change(state);
        Bytes output{7, 8};
        assert(Encode(state, ScopeForTest(), output) == CodecResult::LimitExceeded);
        assert((output == Bytes{7, 8}));
    }
    Reject(Bytes(kMaximumRecordBytes + 1), CodecResult::LimitExceeded);
}

void InvalidTypedState() {
    const std::vector<std::function<void(FacePersistentState&)>> invalid = {
        [](auto& s) { s.version++; },
        [](auto& s) { s.schedule.version++; },
        [](auto& s) { s.schedule.snapshot.version++; },
        [](auto& s) { s.schedule.snapshot.scope.device_id = Id(999); },
        [](auto& s) { s.schedule.snapshot.cues[0].id = std::string(36, '0'); },
        [](auto& s) { s.schedule.snapshot.cues[0].revision = kMaximumRevision + 1; },
        [](auto& s) { s.schedule.snapshot.cues[0].deadline_ms = kMaximumEpochMs + 1; },
        [](auto& s) { s.schedule.snapshot.cues[0].label = "\xc0\xaf"; },
        [](auto& s) { s.schedule.snapshot.cues[0].label = "\xed\xa0\x80"; },
        [](auto& s) { s.schedule.snapshot.cues[0].label = std::string("A\0B", 3); },
        [](auto& s) { s.schedule.snapshot.cues[0].label = std::string(81, 'A'); },
        [](auto& s) { s.schedule.snapshot.cues[0].label = std::string(321, 'A'); },
        [](auto& s) { s.schedule.snapshot.timezone = std::string(65, 'A'); },
        [](auto& s) { s.schedule.items[0].key.revision++; },
        [](auto& s) {
            s.schedule.items[0].acknowledged = true;
            s.schedule.items[0].due = false;
        },
        [](auto& s) { s.schedule.retired_ids.push_back(s.schedule.items[0].key.id); },
        [](auto& s) { s.pending.push_back(s.schedule.items[0].key); },
        [](auto& s) { s.pending.push_back({ScopeForTest(), ItemKind::Cue, Id(30), Id(31), 1}); },
    };
    for (const auto& change : invalid) {
        auto state = State();
        change(state);
        Bytes output{7, 8};
        assert(Encode(state, ScopeForTest(), output) == CodecResult::InvalidState);
        assert((output == Bytes{7, 8}));
    }
}

void CorruptionAndTruncation() {
    const auto original = Encoded(State(1, 1));
    for (size_t length = 0; length < original.size(); ++length)
        Reject(Bytes(original.begin(), original.begin() + length), CodecResult::Corrupt);
    for (size_t i = 0; i < original.size(); ++i) {
        auto bytes = original;
        bytes[i] ^= 1;
        Reject(bytes, i == 4 ? CodecResult::UnsupportedVersion : CodecResult::Corrupt);
    }
    auto bytes = original;
    bytes.insert(bytes.end() - 4, 0x55);
    SetNumber(bytes, 5, bytes.size(), 2);
    Checksum(bytes);
    Reject(bytes, CodecResult::Corrupt);  // Complete parse required even with valid CRC/length.
    FacePersistentState unchanged = State();
    assert(Decode(nullptr, 100, ScopeForTest(), unchanged) == CodecResult::Corrupt);
}

void PayloadValidation() {
    auto original = Encoded(State(1, 1));
    const size_t cue = 93 + std::strlen("Europe/Brussels");
    const std::vector<std::pair<size_t, uint8_t>> malformed = {
        {7, 1},
        {cue, 2},
        {cue + 36 + 5, 4},
    };
    for (const auto& [offset, value] : malformed) {
        auto bytes = original;
        bytes[offset] = value;
        Checksum(bytes);
        Reject(bytes, CodecResult::Corrupt);
    }
    for (const auto [offset, value] :
         std::vector<std::pair<size_t, uint8_t>>{{8, 7}, {9, 6}, {10, 65}, {11, 7}}) {
        auto bytes = original;
        bytes[offset] = value;
        Checksum(bytes);
        Reject(bytes, CodecResult::LimitExceeded);
    }
    for (const auto& change : std::vector<std::function<void(Bytes&)>>{
             [&](auto& b) { b[cue + 36] = 0; },
             [&](auto& b) { b[cue + 36] = 0xff; },
             [&](auto& b) { std::fill(b.begin() + 12, b.begin() + 28, 0); },
             [&](auto& b) { SetNumber(b, cue + 17, kMaximumRevision + 1, 7); },
             [&](auto& b) { SetNumber(b, 74, kMaximumEpochMs + 1, 6); },
         }) {
        auto bytes = original;
        change(bytes);
        Checksum(bytes);
        Reject(bytes, CodecResult::InvalidState);
    }
    auto bytes = original;
    SetNumber(bytes, cue + 34, 321, 2);
    Checksum(bytes);
    Reject(bytes, CodecResult::Corrupt);
    auto value = State();
    const auto old = Encoded(value);
    assert(Decode(original.data(), original.size(), {Id(999), Id(2)}, value) ==
           CodecResult::InvalidState);
    assert(Encoded(value) == old);
}

enum class Fault {
    None,
    OpenRead,
    OpenWrite,
    SizeRead,
    DataRead,
    SizeChange,
    NoSpace,
    SetBefore,
    SetAfter,
    CommitBefore,
    CommitAfter,
    AfterCommitRead,
    CorruptReadback,
    DifferentReadback
};
struct Handle {
    int mode;
    Bytes staged;
};
struct NvsFixture {
    std::string expected_namespace = "orbit_sched_v1";
    bool namespace_present = false;
    bool key_present = false;
    Bytes disk;
    std::map<unsigned, Handle> handles;
    unsigned next_handle = 1;
    Fault fault = Fault::None;
    int sets = 0, commits = 0, reads = 0, opens = 0;
    Bytes other_valid;
} nvs;
void ResetNvs() {
    assert(nvs.handles.empty());
    nvs = {};
}
void Seed(const FacePersistentState& state) {
    ResetNvs();
    nvs.namespace_present = nvs.key_present = true;
    nvs.disk = Encoded(state);
}

void NvsAbsentAndLoad() {
    ResetNvs();
    NvsStore store(ScopeForTest());
    auto value = State();
    const auto original = Encoded(value);
    Bytes bytes{42};
    assert(store.Load(value, bytes) == LoadResult::Absent);
    assert(Encoded(value) == original && bytes == Bytes{42});
    nvs.namespace_present = true;
    assert(store.Load(value, bytes) == LoadResult::Absent);
    nvs.key_present = true;
    assert(store.Load(value, bytes) == LoadResult::Corrupt);
    nvs.disk = Bytes(kMaximumRecordBytes + 1);
    assert(store.Load(value, bytes) == LoadResult::Corrupt);
    Seed(State());
    assert(store.Load(value, bytes) == LoadResult::Present && bytes == nvs.disk);
    assert(nvs.handles.empty());
    assert(Encoded(value) == bytes);
    nvs.disk[0] ^= 1;
    assert(store.Load(value, bytes) == LoadResult::Corrupt);
    assert(Encoded(value) == bytes);
}

void NvsSaveAndReboot() {
    ResetNvs();
    NvsStore store(ScopeForTest());
    const auto desired = State();
    Bytes prior;
    assert(store.Transition(nullptr, desired, prior) == SaveResult::Saved);
    assert(nvs.sets == 1 && nvs.commits == 1 && prior == nvs.disk);
    assert(store.Transition(&prior, desired, prior) == SaveResult::Unchanged);
    assert(nvs.sets == 1 && nvs.commits == 1);
    auto acked = desired;
    acked.schedule.items[0].acknowledged = true;
    acked.pending.push_back(acked.schedule.items[0].key);
    assert(store.Transition(&prior, acked, prior) == SaveResult::Saved);
    NvsStore after_reboot(ScopeForTest());
    FacePersistentState loaded;
    Bytes bytes;
    assert(after_reboot.Load(loaded, bytes) == LoadResult::Present && bytes == prior);
    FaceModel recovered(ScopeForTest());
    assert(recovered.RestoreState(loaded));
    assert(recovered.due().size() == 5 && recovered.pending().size() == 1);
    assert(recovered.scheduler().clock_state() == ClockState::AwaitingFreshTime);
    assert(!recovered.scheduler().connected());
}

void NvsBenchNamespace() {
    ResetNvs();
    // The NVS substitute rejects any open outside the selected namespace. The
    // default Live path is independently exercised by all other scenarios.
    nvs.expected_namespace = "orbit_bench_v1";
    NvsStore bench(ScopeForTest(), StoreDomain::Bench);
    Bytes verified;
    assert(bench.Transition(nullptr, State(), verified) == SaveResult::Saved);
    FacePersistentState restored;
    Bytes loaded;
    assert(bench.Load(restored, loaded) == LoadResult::Present && loaded == verified);
    const auto opened = nvs.opens;
    NvsStore invalid(ScopeForTest(), static_cast<StoreDomain>(99));
    assert(invalid.Load(restored, loaded) == LoadResult::IoError);
    assert(invalid.Transition(nullptr, State(), verified) == SaveResult::IoError);
    assert(nvs.opens == opened);
}

void NvsExpectedPrior() {
    auto state = State();
    Seed(state);
    NvsStore store(ScopeForTest());
    Bytes output{42};
    assert(store.Transition(nullptr, state, output) == SaveResult::Conflict);
    auto stale = nvs.disk;
    stale.back() ^= 1;
    assert(store.Transition(&stale, state, output) == SaveResult::Conflict);
    const auto exact = nvs.disk;
    state.schedule.snapshot.snapshot_revision++;
    assert(store.Transition(&exact, state, output) == SaveResult::Saved);
    const int written = nvs.sets;
    assert(store.Transition(&exact, state, output) == SaveResult::Conflict);
    assert(nvs.sets == written);
    state.version++;
    assert(store.Transition(&output, state, output) == SaveResult::InvalidState);
    assert(nvs.sets == written);
    Seed(State());
    nvs.disk[0] ^= 1;
    assert(store.Transition(nullptr, State(), output) == SaveResult::Corrupt);
    assert(nvs.sets == 0);
}

void NvsReadFailures() {
    for (const auto fault :
         {Fault::OpenRead, Fault::SizeRead, Fault::DataRead, Fault::SizeChange}) {
        Seed(State());
        nvs.fault = fault;
        NvsStore store(ScopeForTest());
        auto output = State(0, 1);
        const auto sentinel = Encoded(output);
        Bytes bytes{42};
        assert(store.Load(output, bytes) == LoadResult::IoError);
        assert(Encoded(output) == sentinel && bytes == Bytes{42});
        const auto prior = nvs.disk;
        assert(store.Transition(&prior, State(), bytes) == SaveResult::IoError);
        assert(nvs.sets == 0 && nvs.handles.empty());
    }
    Seed(State());
    NvsStore wrong({Id(999), Id(2)});
    FacePersistentState output;
    Bytes bytes;
    assert(wrong.Load(output, bytes) == LoadResult::Corrupt && nvs.sets == 0);
}

void NvsWriteFailures() {
    for (const auto fault : {Fault::OpenWrite, Fault::NoSpace, Fault::SetBefore, Fault::SetAfter,
                             Fault::CommitBefore, Fault::CommitAfter, Fault::AfterCommitRead,
                             Fault::CorruptReadback, Fault::DifferentReadback}) {
        auto desired = State();
        Seed(desired);
        const auto prior = nvs.disk;
        nvs.other_valid = prior;
        desired.schedule.items[0].acknowledged = true;
        desired.pending.push_back(desired.schedule.items[0].key);
        nvs.fault = fault;
        NvsStore store(ScopeForTest());
        Bytes verified{42};
        assert(store.Transition(&prior, desired, verified) ==
               (fault == Fault::OpenWrite ? SaveResult::IoError : SaveResult::Uncertain));
        assert(verified == Bytes{42} && nvs.handles.empty());
        const bool changed = fault != Fault::OpenWrite && fault != Fault::NoSpace &&
                             fault != Fault::SetBefore && fault != Fault::CommitBefore;
        assert(nvs.disk == (changed ? Encoded(desired) : prior));
        nvs.fault = Fault::None;
        FacePersistentState recovered;
        Bytes recovered_bytes;
        assert(store.Load(recovered, recovered_bytes) == LoadResult::Present);
        assert(recovered.pending.size() == (changed ? 1 : 0));
        if (changed) {
            assert(store.Transition(&prior, desired, verified) == SaveResult::Conflict);
            assert(store.Transition(&recovered_bytes, desired, verified) == SaveResult::Unchanged);
        }
    }
}
}  // namespace

// The sole substituted layer is the platform NVS I/O. Any namespace/key access
// beyond the new adapter's exact pair fails; no erase/init functions are supplied.
int nvs_open(const char* name, int mode, nvs_handle_t* handle) {
    assert(std::string(name) == nvs.expected_namespace);
    ++nvs.opens;
    if ((mode == NVS_READONLY && nvs.fault == Fault::OpenRead) ||
        (mode == NVS_READWRITE && nvs.fault == Fault::OpenWrite))
        return -1;
    if (!nvs.namespace_present && mode == NVS_READONLY)
        return ESP_ERR_NVS_NOT_FOUND;
    if (mode == NVS_READWRITE)
        nvs.namespace_present = true;
    *handle = nvs.next_handle++;
    nvs.handles[*handle] = {mode, nvs.disk};
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(nvs.handles.erase(handle) == 1); }
int nvs_get_blob(nvs_handle_t handle, const char* key, void* out, size_t* size) {
    assert(nvs.handles.count(handle) == 1 && std::string(key) == "state");
    ++nvs.reads;
    if (nvs.fault == Fault::AfterCommitRead && nvs.commits)
        return -1;
    if ((!out && nvs.fault == Fault::SizeRead) || (out && nvs.fault == Fault::DataRead))
        return -1;
    if (!nvs.key_present)
        return ESP_ERR_NVS_NOT_FOUND;
    const auto& source =
        nvs.fault == Fault::DifferentReadback && nvs.commits ? nvs.other_valid : nvs.disk;
    if (!out) {
        *size = source.size();
        return ESP_OK;
    }
    assert(*size >= source.size());
    std::memcpy(out, source.data(), source.size());
    *size = source.size();
    if (nvs.fault == Fault::SizeChange)
        --*size;
    if (nvs.fault == Fault::CorruptReadback && nvs.commits)
        static_cast<uint8_t*>(out)[0] ^= 1;
    return ESP_OK;
}
int nvs_set_blob(nvs_handle_t handle, const char* key, const void* input, size_t size) {
    assert(nvs.handles.at(handle).mode == NVS_READWRITE && std::string(key) == "state");
    assert(size <= kMaximumRecordBytes);
    ++nvs.sets;
    if (nvs.fault == Fault::NoSpace)
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    if (nvs.fault == Fault::SetBefore)
        return -1;
    const auto* bytes = static_cast<const uint8_t*>(input);
    nvs.handles.at(handle).staged.assign(bytes, bytes + size);
    if (nvs.fault == Fault::SetAfter) {
        nvs.disk = nvs.handles.at(handle).staged;
        nvs.key_present = true;
        return -1;
    }
    return ESP_OK;
}
int nvs_commit(nvs_handle_t handle) {
    assert(nvs.handles.at(handle).mode == NVS_READWRITE);
    ++nvs.commits;
    if (nvs.fault == Fault::CommitBefore)
        return -1;
    nvs.disk = nvs.handles.at(handle).staged;
    nvs.key_present = true;
    return nvs.fault == Fault::CommitAfter ? -1 : ESP_OK;
}

int main() {
    const std::pair<const char*, void (*)()> cases[] = {
        {"deterministic_roundtrip", Roundtrip},
        {"worst_case_3567_bytes", WorstCase},
        {"historical_pending_keys", HistoricalPending},
        {"exported_pending_after_actual_edits", ExportedPendingAfterEdits},
        {"numeric_epoch_and_offset_edges", NumericEdges},
        {"observable_reboot_and_exact_ack", ObservableReboot},
        {"counts_and_limits", CountsAndBounds},
        {"invalid_typed_states", InvalidTypedState},
        {"corruption_and_every_truncation", CorruptionAndTruncation},
        {"payload_validation_with_valid_crc", PayloadValidation},
        {"nvs_absent_corrupt_present", NvsAbsentAndLoad},
        {"nvs_commit_readback_reboot", NvsSaveAndReboot},
        {"nvs_explicit_bench_namespace", NvsBenchNamespace},
        {"nvs_expected_prior_and_unchanged", NvsExpectedPrior},
        {"nvs_read_errors", NvsReadFailures},
        {"nvs_uncertain_writes_and_reconciliation", NvsWriteFailures},
    };
    for (const auto& [name, run] : cases) {
        run();
        assert(nvs.handles.empty());
        std::printf("PASS %s\n", name);
    }
    std::printf("%zu storage scenarios passed\n", std::size(cases));
}
