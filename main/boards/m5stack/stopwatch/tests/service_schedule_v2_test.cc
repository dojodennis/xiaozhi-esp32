#include "service_schedule_storage.h"
#include "service_schedule_v2_fixture.h"
#include "service_schedule_wire.h"

#include <cassert>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <limits>

using namespace orbit::service_schedule;
namespace {
std::string Id(unsigned number) {
    char text[37];
    std::snprintf(text, sizeof(text), "00000000-0000-4000-8000-%012x", number);
    return text;
}
const Scope scope{Id(1), Id(2)};
constexpr int64_t epoch = 1788883300000LL;
bool Label(std::string_view s) { return !s.empty(); }
bool Zone(std::string_view s) { return s == "Europe/Brussels"; }
wire::Context Context(std::string_view occurrence = "") { return {kSession, scope, occurrence}; }
Snapshot Frame(size_t i) {
    Snapshot s;
    assert(wire::Decode(kFrames[i], Context(kOccurrences[i]), {Label, Zone}, s) ==
           wire::Result::Accepted);
    return s;
}
ClockRequest Request(uint64_t revision = 21, unsigned nonce = 0x99) {
    return {scope, kSession, Id(nonce), revision};
}
void Connect(Scheduler& s) {
    assert(s.SetClockSession(kSession));
    s.SetConnected(true);
}
void Connect(FaceModel& s) {
    assert(s.SetClockSession(kSession));
    s.SetConnected(true);
}
Scheduler Absent() {
    Scheduler s(scope);
    assert(s.Apply(Frame(1), 0) == ApplyResult::Applied);
    Connect(s);
    return s;
}
void Trust(Scheduler& s, int64_t now = epoch, int64_t mono = 1000, unsigned nonce = 0x99) {
    const auto request = Request(s.snapshot()->snapshot_revision, nonce);
    assert(s.BeginClockRequest(request, mono));
    assert(s.AcceptClock({request, now}, mono) == ClockResult::Accepted);
}
FacePersistentState Export(const FaceModel& model) {
    FacePersistentState state;
    assert(model.ExportState(state));
    return state;
}
storage::Bytes Encode(const FacePersistentState& state) {
    storage::Bytes bytes;
    assert(storage::Encode(state, scope, bytes) == storage::CodecResult::Accepted);
    return bytes;
}
FacePersistentState Decode(const storage::Bytes& bytes) {
    FacePersistentState state;
    assert(storage::Decode(bytes.data(), bytes.size(), scope, state) ==
           storage::CodecResult::Accepted);
    return state;
}
void GoldenExchange() {
    auto s = Absent();
    ClockResponse response;
    assert(wire::DecodeClock(kClockResponse, Context(), response) == wire::Result::Accepted);
    std::string request;
    assert(wire::EncodeClockRequest(response.request, request));
    assert(request == kClockRequest);
    assert(s.BeginClockRequest(response.request, 1000));
    assert(s.AcceptClock(response, 3000) == ClockResult::Accepted);
    assert(s.now_ms() == epoch && s.items()[0].due && !s.items()[1].due);
    assert(s.AcceptClock(response, 3001) == ClockResult::Rejected);
    assert(s.Tick(23000) == ClockState::Trusted && s.items()[1].due);
}
void NoSnapshotTime() {
    auto s = Absent();
    assert(s.clock_state() == ClockState::AwaitingFreshTime && s.now_ms() == 0);
    s.Tick(999999999);
    assert(!s.items()[0].due);
    assert(s.Apply(Frame(1), 1) == ApplyResult::Replay);
    auto invalid = Frame(1);
    invalid.snapshot_revision++;
    invalid.server_now_ms = epoch;
    assert(s.Apply(invalid, 2) == ApplyResult::Malformed);
    auto stale = Frame(0);
    assert(s.Apply(stale, 3) == ApplyResult::StaleRevision);
    assert(s.clock_state() == ClockState::AwaitingFreshTime && s.now_ms() == 0);
}
void PresenceAndHistory() {
    FaceModel model(scope);
    assert(model.ApplyVerified(Frame(0), 0) == ApplyResult::Applied);
    Connect(model);
    const auto request = Request(20);
    assert(model.BeginClockRequest(request, 100));
    assert(model.AcceptClock({request, epoch}, 100) == ClockResult::Accepted);
    const auto cue = model.scheduler().items()[0].key;
    const auto timer = model.scheduler().items()[2].key;
    assert(model.Acknowledge(cue) && model.Acknowledge(timer));
    assert(model.ApplyVerified(Frame(1), 200) == ApplyResult::Applied);
    assert(model.pending().size() == 2 && model.pending()[0].id == cue.id);
    assert(model.scheduler().items()[0].acknowledged);
    auto bytes = Encode(Export(model));
    assert(bytes[4] == 2);
    FaceModel restored(scope);
    assert(restored.RestoreState(Decode(bytes)));
    assert(restored.pending().size() == 2 && !restored.scheduler().connected());
    assert(restored.scheduler().clock_state() == ClockState::AwaitingFreshTime);
    assert(restored.ApplyVerified(Frame(2), 1) == ApplyResult::Applied);
    assert(restored.scheduler().items()[2].acknowledged && restored.pending().size() == 2);
    auto resurrection = Frame(0);
    resurrection.snapshot_revision = 23;
    assert(restored.ApplyVerified(resurrection, 2) == ApplyResult::RetiredIdentity);
    assert(Export(restored).schedule.retired_ids.size() == 2);
    assert(restored.pending()[0].service_occurrence_id == Id(3));
}
void ZeroClockStorage() {
    FaceModel model(scope);
    assert(model.ApplyVerified(Frame(1), 0) == ApplyResult::Applied);
    const auto state = Export(model);
    auto bytes = Encode(state);
    assert(Encode(Decode(bytes)) == bytes);
    FaceModel restored(scope);
    assert(restored.RestoreState(Decode(bytes)));
    auto bad = state;
    bad.schedule.items[0].due = true;
    storage::Bytes sentinel{9};
    assert(storage::Encode(bad, scope, sentinel) == storage::CodecResult::InvalidState &&
           sentinel[0] == 9);
    bad = state;
    bad.pending.push_back(bad.schedule.items[0].key);
    assert(storage::Encode(bad, scope, sentinel) == storage::CodecResult::InvalidState);
    // The current decoder rejects unknown encoding versions instead of guessing.
    bytes[4] = 3;
    FacePersistentState out;
    assert(storage::Decode(bytes.data(), bytes.size(), scope, out) ==
           storage::CodecResult::UnsupportedVersion);
}
void V2MaximumStorageAndCorruption() {
    auto snapshot = Frame(1);
    snapshot.timers.clear();
    for (unsigned i = 0; i < 6; ++i) {
        std::string label;
        for (unsigned n = 0; n < 80; ++n)
            label += "🍳";
        snapshot.timers.push_back({Id(100 + i), kMaximumRevision, label, epoch});
    }
    FaceModel model(scope);
    assert(model.ApplyVerified(snapshot, 0) == ApplyResult::Applied);
    Connect(model);
    const auto req = Request();
    assert(model.BeginClockRequest(req, 0));
    assert(model.AcceptClock({req, epoch}, 0) == ClockResult::Accepted);
    for (const auto& item : model.scheduler().items())
        assert(model.Acknowledge(item.key));
    auto state = Export(model);
    for (unsigned i = 0; i < 64; ++i)
        state.schedule.retired_ids.push_back(Id(200 + i));
    const auto bytes = Encode(state);
    auto restored = Decode(bytes);
    assert(restored.pending.size() == 6 && restored.schedule.retired_ids.size() == 64);
    assert(Encode(restored) == bytes && bytes.size() <= storage::kWorstCaseRecordBytes);
    for (size_t i = 0; i < bytes.size(); ++i) {
        auto damaged = bytes;
        damaged[i] ^= 0x80;
        FacePersistentState output = state;
        assert(storage::Decode(damaged.data(), damaged.size(), scope, output) !=
               storage::CodecResult::Accepted);
        assert(Encode(output) == bytes);
        assert(storage::Decode(bytes.data(), i, scope, output) != storage::CodecResult::Accepted);
    }
    state.schedule.retired_ids.push_back(Id(300));
    storage::Bytes output{7};
    assert(storage::Encode(state, scope, output) == storage::CodecResult::LimitExceeded &&
           output[0] == 7);
}
void UpgradeOnly() {
    Snapshot legacy;
    assert(wire::Decode(kV1Frame, Context(Id(3)), {Label, Zone}, legacy) == wire::Result::Accepted);
    FaceModel old(scope);
    assert(old.ApplyVerified(legacy, 0) == ApplyResult::Applied);
    assert(Encode(Export(old)) == storage::Bytes(std::begin(kV1Bytes), std::end(kV1Bytes)));
    auto upgraded = Frame(0);
    upgraded.snapshot_revision = legacy.snapshot_revision;
    assert(old.ApplyVerified(upgraded, 0) == ApplyResult::ConflictingRevision);
    upgraded.snapshot_revision++;
    assert(old.ApplyVerified(upgraded, 5) == ApplyResult::Applied);
    assert(old.scheduler().now_ms() == legacy.server_now_ms + 5);
    legacy.snapshot_revision += 100;
    assert(old.ApplyVerified(legacy, 6) == ApplyResult::ConflictingRevision);
    FaceModel restored(scope);
    assert(restored.RestoreState(Decode(Encode(Export(old)))));
    assert(restored.ApplyVerified(legacy, 0) == ApplyResult::ConflictingRevision);
}
void TimeoutRecovery() {
    auto s = Absent();
    const auto first = Request(), second = Request(21, 0x98);
    assert(s.BeginClockRequest(first, 1000));
    assert(!s.BeginClockRequest(second, 999));
    assert(!s.BeginClockRequest(second, 3000));
    assert(!s.BeginClockRequest(first, 3001));
    assert(s.BeginClockRequest(second, 3001));
    assert(s.AcceptClock({first, epoch}, 3002) == ClockResult::Rejected);
    assert(s.AcceptClock({second, epoch}, 5001) == ClockResult::Accepted);
    s = Absent();
    assert(s.BeginClockRequest(first, 1000));
    assert(s.AcceptClock({first, epoch}, 3001) == ClockResult::Rejected);
    assert(s.AcceptClock({first, epoch}, 3000) == ClockResult::Rejected);
    assert(s.clock_state() == ClockState::AwaitingFreshTime);
}
void RequestIdentity() {
    auto s = Absent();
    const auto request = Request();
    for (int i = 0; i < 4; ++i) {
        auto wrong = request;
        if (i == 0)
            wrong.scope.device_id = Id(55);
        if (i == 1)
            wrong.session_id = Id(55);
        if (i == 2)
            wrong.snapshot_revision = 20;
        if (i == 3)
            wrong.request_id = "";
        assert(!s.BeginClockRequest(wrong, 1000));
    }
    assert(!s.BeginClockRequest(request, -1));
    assert(s.BeginClockRequest(request, 1000));
    for (int i = 0; i < 4; ++i) {
        auto wrong = request;
        if (i == 0)
            wrong.scope.assignment_id = Id(55);
        if (i == 1)
            wrong.session_id = Id(55);
        if (i == 2)
            wrong.snapshot_revision = 20;
        if (i == 3)
            wrong.request_id = Id(55);
        assert(s.AcceptClock({wrong, epoch}, 1001) == ClockResult::Rejected);
    }
    assert(s.AcceptClock({request, epoch}, 1001) == ClockResult::Accepted);
}
void Invalidation() {
    for (int mode = 0; mode < 5; ++mode) {
        auto s = Absent();
        const auto request = Request();
        assert(s.BeginClockRequest(request, 1000));
        if (mode == 0)
            s.SetConnected(false);
        if (mode == 1)
            assert(s.SetClockSession(Id(5)));
        if (mode == 2)
            assert(s.Apply(Frame(2), 1100) == ApplyResult::Applied);
        if (mode == 3) {
            PersistentState state;
            assert(s.ExportState(state));
            Scheduler restored(scope);
            assert(restored.Restore(state));
            s = restored;
            Connect(s);
        }
        if (mode == 4)
            s.CancelClockRequest();
        assert(s.AcceptClock({request, epoch}, 1200) == ClockResult::Rejected);
        assert(s.clock_state() != ClockState::Trusted);
    }
    auto s = Absent();
    auto request = Request();
    assert(s.BeginClockRequest(request, 1000));
    assert(s.Apply(Frame(1), 1100) == ApplyResult::Replay);
    auto bad = Frame(2);
    bad.server_now_ms = 1;
    assert(s.Apply(bad, 1100) == ApplyResult::Malformed);
    assert(s.AcceptClock({request, epoch}, 1200) == ClockResult::Accepted);
}
void ClockBounds() {
    for (const auto bad : {int64_t{0}, int64_t{-1}, kMaximumEpochMs + 1}) {
        auto s = Absent();
        auto req = Request();
        assert(s.BeginClockRequest(req, 1000));
        assert(s.AcceptClock({req, bad}, 1000) == ClockResult::Rejected);
    }
    for (const auto bad : {int64_t{-1}, int64_t{999}}) {
        auto s = Absent();
        auto req = Request();
        assert(s.BeginClockRequest(req, 1000));
        assert(s.AcceptClock({req, epoch}, bad) == ClockResult::InvalidClock);
    }
    auto s = Absent();
    Trust(s, kMaximumEpochMs - 1);
    auto req = Request(21, 0x97);
    assert(s.BeginClockRequest(req, 1000));
    assert(s.AcceptClock({req, kMaximumEpochMs}, 1002) == ClockResult::InvalidClock);
    s = Absent();
    Trust(s, kMaximumEpochMs - 1);
    assert(s.BeginClockRequest(req, 1000));
    assert(s.Apply(Frame(2), 1002) == ApplyResult::InvalidClock);
    assert(s.AcceptClock({req, kMaximumEpochMs}, 1002) == ClockResult::Rejected);
    s = Absent();
    Trust(s);
    assert(s.BeginClockRequest(req, 1000));
    assert(s.Tick(999) == ClockState::Invalid);
    assert(s.AcceptClock({req, epoch}, 1001) == ClockResult::Rejected);
}
void NoRewindAndOffline() {
    auto s = Absent();
    Trust(s);
    s.SetConnected(false);
    assert(s.Tick(2000) == ClockState::Trusted && s.now_ms() == epoch + 1000);
    Connect(s);
    auto req = Request(21, 0x97);
    assert(s.BeginClockRequest(req, 2000));
    assert(s.AcceptClock({req, epoch}, 3000) == ClockResult::Accepted);
    assert(s.now_ms() == epoch + 2000);
    PersistentState state;
    assert(s.ExportState(state));
    Scheduler restored(scope);
    assert(restored.Restore(state));
    Connect(restored);
    req = Request(21, 0x96);
    assert(restored.BeginClockRequest(req, 0));
    assert(restored.AcceptClock({req, epoch}, 1) == ClockResult::Rejected);
    assert(restored.clock_state() == ClockState::AwaitingFreshTime);
    req.request_id = Id(0x95);
    assert(restored.BeginClockRequest(req, 2));
    assert(restored.AcceptClock({req, epoch + 2000}, 3) == ClockResult::Accepted);
}
}  // namespace
int main(int argc, char** argv) {
    if (argc > 1) {
        const std::string bytes(std::istreambuf_iterator<char>(std::cin), {});
        const std::string mode = argv[1];
        auto context = Context(mode == "present" ? kOccurrences[0] : "");
        if (mode == "wrong_session")
            context.websocket_session_id = kOccurrences[0];
        if (mode == "wrong_scope")
            context.enrolled_scope.device_id = Id(44);
        if (mode == "clock" || mode == "wrong_session" || mode == "wrong_scope") {
            ClockResponse out;
            out.server_now_ms = 7;
            const auto result = wire::DecodeClock(bytes, context, out);
            if (result != wire::Result::Accepted)
                assert(out.server_now_ms == 7 && out.request.request_id.empty());
            std::cout << static_cast<int>(result) << '\n';
        } else {
            Snapshot out;
            out.snapshot_revision = 777;
            const auto result = wire::Decode(bytes, context, {Label, Zone}, out);
            if (result != wire::Result::Accepted)
                assert(out.snapshot_revision == 777 && out.timers.empty());
            std::cout << static_cast<int>(result) << '\n';
        }
        return 0;
    }
    GoldenExchange();
    NoSnapshotTime();
    PresenceAndHistory();
    ZeroClockStorage();
    UpgradeOnly();
    V2MaximumStorageAndCorruption();
    TimeoutRecovery();
    RequestIdentity();
    Invalidation();
    ClockBounds();
    NoRewindAndOffline();
    std::puts("11 v2 model/codec/clock scenarios passed");
}
