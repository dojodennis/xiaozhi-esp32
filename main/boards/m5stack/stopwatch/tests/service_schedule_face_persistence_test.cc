#include "service_schedule_face.h"

#include <cassert>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace orbit::service_schedule;
using ProvisionsStopwatchOrbit::AlarmOutputChange;

namespace {
std::string Id(int value) {
    char buffer[37];
    std::snprintf(buffer, sizeof(buffer), "11111111-1111-4111-8111-%012d", value);
    return buffer;
}
Scope Enrolled() { return {Id(1), Id(2)}; }
Snapshot Six() {
    Snapshot s;
    s.scope = Enrolled();
    s.service_occurrence_id = Id(3);
    s.service_revision = 1;
    s.snapshot_revision = 1;
    s.service_at_ms = 10'000'000;
    s.server_now_ms = 1'000'000;
    s.timezone = "Europe/Brussels";
    for (int i = 0; i < 6; ++i)
        s.timers.push_back({Id(10 + i), 1, "Timer " + std::to_string(i + 1), 1'001'000});
    return s;
}
FacePersistentState Saved(const FaceModel& model) {
    FacePersistentState saved;
    assert(model.ExportState(saved));
    return saved;
}
FaceModel DueSix() {
    FaceModel model(Enrolled());
    assert(model.ApplyVerified(Six(), 0) == ApplyResult::Applied);
    model.Tick(1'000);
    assert(model.due().size() == 6 && model.alarm_active());
    return model;
}
void RejectAtomically(const FacePersistentState& bad, const FacePersistentState& good) {
    FaceModel model(Enrolled());
    assert(!model.RestoreState(bad));
    assert(!model.scheduler().snapshot());
    assert(model.pending().empty() && model.due().empty() && !model.alarm_active());
    assert(!model.receipt_confirmed());
    assert(model.TakeOutputChange() == AlarmOutputChange::kNone);
    assert(model.RestoreState(good));
}

void RunningWaitsForFreshTime() {
    FaceModel model(Enrolled());
    auto snapshot = Six();
    assert(model.ApplyVerified(snapshot, 0) == ApplyResult::Applied);
    model.SetConnected(true);
    FaceModel recovered(Enrolled());
    // Even a caller that had marked an empty model connected cannot restore that flag.
    recovered.SetConnected(true);
    assert(recovered.RestoreState(Saved(model)));
    assert(!recovered.scheduler().connected());
    assert(recovered.scheduler().clock_state() == ClockState::AwaitingFreshTime);
    recovered.Tick(500'000);
    assert(recovered.scheduler().now_ms() == snapshot.server_now_ms);
    assert(recovered.due().empty() && !recovered.alarm_active());
    assert(recovered.ApplyVerified(snapshot, 100) == ApplyResult::Replay);
    assert(recovered.scheduler().clock_state() == ClockState::AwaitingFreshTime);
    ++snapshot.snapshot_revision;
    snapshot.server_now_ms += 1'000;
    assert(recovered.ApplyVerified(snapshot, 100) == ApplyResult::Applied);
    assert(recovered.scheduler().clock_state() == ClockState::Trusted);
    assert(recovered.due().size() == 6 && recovered.alarm_active());
}

void OneOfSixAckSurvivesRestart() {
    auto model = DueSix();
    assert(model.AcknowledgeNext());
    const auto key = model.pending().front();
    FaceModel recovered(Enrolled());
    assert(recovered.RestoreState(Saved(model)));
    assert(recovered.due().size() == 5 && recovered.pending().size() == 1);
    assert(recovered.alarm_active());
    assert(recovered.TakeOutputChange() == AlarmOutputChange::kStart);
    assert(recovered.TakeOutputChange() == AlarmOutputChange::kNone);
    assert(!recovered.AcceptReceipt(key));
    recovered.SetConnected(true);
    auto wrong = key;
    ++wrong.revision;
    assert(!recovered.AcceptReceipt(wrong));
    assert(recovered.pending().size() == 1);
    assert(recovered.AcceptReceipt(key));
    assert(recovered.pending().empty() && recovered.receipt_confirmed());
    assert(recovered.due().size() == 5 && recovered.alarm_active());
    FaceModel again(Enrolled());
    assert(again.RestoreState(Saved(recovered)));
    assert(again.pending().empty() && again.due().size() == 5);
    assert(!again.receipt_confirmed());
}

void AllAckedStaySilent() {
    auto model = DueSix();
    for (int i = 0; i < 6; ++i)
        assert(model.AcknowledgeNext());
    assert(!model.AcknowledgeNext());
    FaceModel recovered(Enrolled());
    assert(recovered.RestoreState(Saved(model)));
    assert(recovered.pending().size() == 6 && recovered.due().empty());
    assert(!recovered.alarm_active());
    assert(recovered.TakeOutputChange() == AlarmOutputChange::kNone);
    recovered.Tick(1'000'000);
    assert(recovered.pending().size() == 6 && recovered.due().empty());
    recovered.SetConnected(true);
    auto key = recovered.pending().front();
    assert(recovered.AcceptReceipt(key));
    assert(recovered.pending().size() == 5);
    assert(!recovered.AcceptReceipt(key));
}

void OldAckDoesNotSilenceNewRevision() {
    auto model = DueSix();
    assert(model.AcknowledgeNext());
    const auto old = model.pending().front();
    auto next = Six();
    ++next.snapshot_revision;
    ++next.timers.front().revision;
    next.timers.front().deadline_ms = 1'003'000;
    next.server_now_ms += 1'000;
    assert(model.ApplyVerified(next, 1'000) == ApplyResult::Applied);
    FaceModel recovered(Enrolled());
    assert(recovered.RestoreState(Saved(model)));
    assert(recovered.pending().size() == 1);
    recovered.SetConnected(true);
    assert(recovered.AcceptReceipt(old));
    ++next.snapshot_revision;
    next.server_now_ms += 2'000;
    assert(recovered.ApplyVerified(next, 0) == ApplyResult::Applied);
    assert(recovered.due().size() == 6);
    assert(!recovered.scheduler().items().front().acknowledged);
}

void RetiredAckSurvivesOmission() {
    auto model = DueSix();
    assert(model.AcknowledgeNext());
    const auto old = model.pending().front();
    auto next = Six();
    ++next.snapshot_revision;
    next.server_now_ms += 1'000;
    next.timers.erase(next.timers.begin());
    assert(model.ApplyVerified(next, 1'000) == ApplyResult::Applied);
    auto saved = Saved(model);
    assert(saved.schedule.retired_ids.size() == 1);
    FaceModel recovered(Enrolled());
    assert(recovered.RestoreState(saved));
    assert(recovered.pending().size() == 1 && recovered.due().size() == 5);
    recovered.SetConnected(true);
    assert(recovered.AcceptReceipt(old));
    assert(recovered.due().size() == 5);
}

void CueAckSurvivesServiceAndOccurrenceChange() {
    auto first = Six();
    first.timers.clear();
    first.cues.push_back({Id(20), 1, "Setup", CueKind::ServiceOffset, 1'001'000, -8'999'000});
    FaceModel model(Enrolled());
    assert(model.ApplyVerified(first, 0) == ApplyResult::Applied);
    model.Tick(1'000);
    assert(model.AcknowledgeNext());
    const auto old = model.pending().front();
    auto changed = first;
    ++changed.snapshot_revision;
    ++changed.service_revision;
    changed.server_now_ms += 1'000;
    changed.service_at_ms += 60'000;
    ++changed.cues.front().revision;
    changed.cues.front().deadline_ms += 60'000;
    assert(model.ApplyVerified(changed, 1'000) == ApplyResult::Applied);
    FaceModel service_recovered(Enrolled());
    assert(service_recovered.RestoreState(Saved(model)));
    assert(service_recovered.pending().front().revision == old.revision);
    ++changed.snapshot_revision;
    changed.service_occurrence_id = Id(30);
    changed.cues.front().id = Id(31);
    assert(model.ApplyVerified(changed, 1'000) == ApplyResult::Applied);
    FaceModel occurrence_recovered(Enrolled());
    assert(occurrence_recovered.RestoreState(Saved(model)));
    assert(occurrence_recovered.pending().front().service_occurrence_id ==
           old.service_occurrence_id);
    occurrence_recovered.SetConnected(true);
    assert(occurrence_recovered.AcceptReceipt(old));
}

void CorruptPendingRejectedAtomically() {
    auto model = DueSix();
    assert(model.AcknowledgeNext());
    const auto good = Saved(model);
    const std::vector<std::function<void(FacePersistentState&)>> corruptions = {
        [](auto& s) { s.version = 2; },
        [](auto& s) { s.schedule.version = 2; },
        [](auto& s) { s.pending.front().scope.assignment_id = Id(99); },
        [](auto& s) { s.pending.front().scope.device_id = Id(99); },
        [](auto& s) { s.pending.front().id = Id(99); },
        [](auto& s) { s.pending.front().id = "00000000-0000-0000-0000-000000000000"; },
        [](auto& s) { s.pending.front().revision = 0; },
        [](auto& s) { s.pending.front().revision = kMaximumRevision + 1; },
        [](auto& s) { ++s.pending.front().revision; },
        [](auto& s) { s.pending.front().kind = static_cast<ItemKind>(99); },
        [](auto& s) { s.pending.front().service_occurrence_id = Id(3); },
        [](auto& s) { s.pending.push_back(s.pending.front()); },
        [](auto& s) { s.pending.resize(kMaximumItems + 1, s.pending.front()); },
        [](auto& s) { s.schedule.items.front().acknowledged = false; },
        [](auto& s) { s.schedule.items.front().due = false; },
        [](auto& s) { s.schedule.last_known_epoch_ms = 1; },
    };
    for (const auto& corrupt : corruptions) {
        auto bad = good;
        corrupt(bad);
        RejectAtomically(bad, good);
    }
}

void CorruptCuePendingRejected() {
    auto first = Six();
    first.timers.clear();
    first.cues.push_back({Id(20), 1, "Reminder", CueKind::Fixed, 1'001'000, 0});
    FaceModel model(Enrolled());
    assert(model.ApplyVerified(first, 0) == ApplyResult::Applied);
    model.Tick(1'000);
    assert(model.AcknowledgeNext());
    const auto good = Saved(model);
    for (const std::string& occurrence : {std::string{}, Id(99), std::string(36, 'x')}) {
        auto bad = good;
        bad.pending.front().service_occurrence_id = occurrence;
        RejectAtomically(bad, good);
    }
    auto bad = good;
    bad.pending.front().kind = ItemKind::Timer;
    bad.pending.front().service_occurrence_id.clear();
    RejectAtomically(bad, good);
}

void ExistingModelCannotBeReplaced() {
    auto model = DueSix();
    const auto old = Saved(model);
    assert(model.AcknowledgeNext());
    assert(!model.RestoreState(old));
    assert(model.pending().size() == 1 && model.due().size() == 5);
    FacePersistentState sentinel;
    sentinel.version = 99;
    FaceModel empty(Enrolled());
    assert(!empty.ExportState(sentinel));
    assert(sentinel.version == 99);
    FaceModel wrong({Id(90), Id(91)});
    assert(!wrong.RestoreState(old));
    assert(!wrong.scheduler().snapshot());
}
}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::pair<std::string, void (*)()>> cases = {
        {"running_waits_for_fresh_time", RunningWaitsForFreshTime},
        {"one_of_six_ack_survives_restart", OneOfSixAckSurvivesRestart},
        {"all_acked_stay_silent", AllAckedStaySilent},
        {"old_ack_does_not_silence_new_revision", OldAckDoesNotSilenceNewRevision},
        {"retired_ack_survives_omission", RetiredAckSurvivesOmission},
        {"cue_ack_survives_service_and_occurrence_change",
         CueAckSurvivesServiceAndOccurrenceChange},
        {"corrupt_pending_rejected_atomically", CorruptPendingRejectedAtomically},
        {"corrupt_cue_pending_rejected", CorruptCuePendingRejected},
        {"existing_model_cannot_be_replaced", ExistingModelCannotBeReplaced},
    };
    assert(argc == 2);
    for (const auto& test : cases) {
        if (test.first == argv[1]) {
            test.second();
            return 0;
        }
    }
    return 2;
}
