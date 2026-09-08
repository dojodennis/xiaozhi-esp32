#include "service_schedule.h"
#include "service_schedule_fixture.h"

#include <cassert>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <vector>

using namespace orbit::service_schedule;

std::string Id(unsigned n) {
    char buffer[37];
    std::snprintf(buffer, sizeof(buffer), "00000000-0000-4000-8000-%012x", n);
    return buffer;
}
Snapshot First() { return Fixture0(); }
Snapshot Edit() { return Fixture1(); }
std::vector<std::string> Due(const Scheduler& s) {
    std::vector<std::string> ids;
    for (const auto& item : s.items())
        if (item.due && !item.acknowledged)
            ids.push_back(item.key.id);
    return ids;
}
Scheduler Started() {
    Scheduler s(First().scope);
    assert(s.Apply(First(), 0) == ApplyResult::Applied);
    return s;
}
Snapshot Later(const Scheduler& s) {
    auto n = *s.snapshot();
    ++n.snapshot_revision;
    n.server_now_ms = s.now_ms();
    return n;
}
void GoldenScenario() {
    auto s = Started();
    assert(s.items().size() == 5 && Due(s).empty());
    assert(s.Apply(Edit(), 30000) == ApplyResult::Applied);
    s.SetConnected(false);
    assert(s.Tick(60000) == ClockState::Trusted);
    assert(Due(s) == std::vector<std::string>{Id(0x11)});
    assert(s.Acknowledge(s.items()[1].key) == AckResult::Acknowledged);
    s.Tick(120000);
    assert(Due(s) == std::vector<std::string>{Id(0x20)});
    assert(s.Apply(First(), 121000) == ApplyResult::StaleRevision);
    s.Tick(1860000);
    assert(Due(s) == (std::vector<std::string>{Id(0x10), Id(0x20), Id(0x21), Id(0x22)}));
    assert(s.Tick(100) == ClockState::Invalid);
}
void ServiceEdit() {
    auto s = Started();
    assert(s.Apply(Edit(), 30000) == ApplyResult::Applied);
    assert(s.snapshot()->cues[0].deadline_ms == 1788885000000LL);
    assert(s.snapshot()->cues[1].deadline_ms == 1788883200000LL);
    assert(s.snapshot()->timers[0].deadline_ms == 1788883260000LL);
    assert(s.snapshot()->timers[1].deadline_ms == 1788883320000LL);
    assert(s.snapshot()->timers[2].deadline_ms == 1788883380000LL);
}
void ExactAcknowledgement() {
    auto s = Started();
    auto key = s.items()[2].key;
    assert(s.Acknowledge(key) == AckResult::NotDue);
    s.Tick(120000);
    for (int i = 0; i != 6; ++i) {
        auto wrong = key;
        if (i == 0)
            wrong.id = Id(999);
        if (i == 1)
            ++wrong.revision;
        if (i == 2)
            wrong.scope.assignment_id = Id(999);
        if (i == 3)
            wrong.scope.device_id = Id(999);
        if (i == 4)
            wrong.service_occurrence_id = First().service_occurrence_id;
        if (i == 5)
            wrong.kind = ItemKind::Cue;
        assert(s.Acknowledge(wrong) == AckResult::NotFound);
    }
    assert(s.Acknowledge(key) == AckResult::Acknowledged);
    assert(s.Acknowledge(key) == AckResult::AlreadyAcknowledged);
}
void OccurrenceSwitch() {
    auto s = Started();
    s.Tick(120000);
    const auto timer_key = s.items()[2].key;
    const auto cue_key = s.items()[1].key;
    s.Acknowledge(timer_key);
    s.Acknowledge(cue_key);
    auto n = Later(s);
    n.service_occurrence_id = Id(99);
    n.cues[0].id = Id(1001);
    n.cues[1].id = Id(1002);
    assert(s.Apply(n, 120000) == ApplyResult::Applied);
    assert(s.Acknowledge(timer_key) == AckResult::AlreadyAcknowledged);
    assert(s.Acknowledge(cue_key) == AckResult::NotFound);
    assert(!s.items()[1].acknowledged && s.items()[2].acknowledged);
    auto stale = First();
    stale.service_occurrence_id = Id(100);
    assert(s.Apply(stale, 120001) == ApplyResult::StaleRevision);
}
void OccurrenceCueCannotReplay() {
    auto s = Started();
    s.Tick(120000);
    const auto cue_key = s.items()[1].key;
    const auto timer_key = s.items()[2].key;
    assert(s.Acknowledge(cue_key) == AckResult::Acknowledged);
    assert(s.Acknowledge(timer_key) == AckResult::Acknowledged);
    auto next = Later(s);
    next.service_occurrence_id = Id(99);
    assert(s.Apply(next, 120000) == ApplyResult::ConflictingRevision);
    assert(s.items()[1].acknowledged && s.items()[2].acknowledged);
    next.cues[0].id = Id(1001);
    next.cues[1].id = Id(1002);
    assert(s.Apply(next, 120000) == ApplyResult::Applied);
    assert(s.Acknowledge(timer_key) == AckResult::AlreadyAcknowledged);
    next = Later(s);
    next.service_occurrence_id = First().service_occurrence_id;
    next.cues = First().cues;
    assert(s.Apply(next, 120000) == ApplyResult::RetiredIdentity);
    assert(s.snapshot()->service_occurrence_id == Id(99));
    assert(s.Acknowledge(timer_key) == AckResult::AlreadyAcknowledged);
}
void ReplayNoRearm() {
    auto s = Started();
    s.Tick(120000);
    s.Acknowledge(s.items()[2].key);
    const auto now = s.now_ms();
    assert(s.Apply(First(), 999999) == ApplyResult::Replay);
    assert(s.now_ms() == now && s.items()[2].acknowledged);
}
void ConflictingSnapshot() {
    auto s = Started();
    auto n = First();
    ++n.server_now_ms;
    assert(s.Apply(n, 1) == ApplyResult::ConflictingRevision);
    assert(s.now_ms() == First().server_now_ms);
}
void ConflictingItems() {
    for (int mode = 0; mode != 6; ++mode) {
        auto s = Started();
        auto n = Later(s);
        if (mode == 0)
            n.timers[0].label = "Wrong rice";
        if (mode == 1)
            ++n.timers[0].deadline_ms;
        if (mode == 2)
            n.cues[1].label = "Wrong reminder";
        if (mode == 3)
            ++n.cues[1].deadline_ms;
        if (mode == 4) {
            n = Edit();
            n.cues[0].revision = 1;
        }
        if (mode == 5) {
            n.service_occurrence_id = Id(99);
            n.cues[0].id = Id(1001);
            n.cues[1].id = Id(1002);
            n.timers[0].label = "Mutation across occurrence";
        }
        assert(s.Apply(n, 0) == ApplyResult::ConflictingRevision);
        assert(s.snapshot()->snapshot_revision == 10 && Due(s).empty());
    }
}
void LowerItemRevision() {
    auto s = Started();
    auto n = Later(s);
    n.timers[0].revision = 3;
    assert(s.Apply(n, 0) == ApplyResult::Applied);
    n = Later(s);
    n.timers[0].revision = 2;
    assert(s.Apply(n, 0) == ApplyResult::StaleRevision);
}
void ServiceRevision() {
    auto s = Started();
    auto n = Edit();
    n.service_revision = 1;
    assert(s.Apply(n, 0) == ApplyResult::ConflictingRevision);
    assert(s.Apply(Edit(), 30000) == ApplyResult::Applied);
    n = Later(s);
    n.service_revision = 1;
    assert(s.Apply(n, 30000) == ApplyResult::StaleRevision);
}
void AtomicMalformed() {
    auto s = Started();
    auto n = Edit();
    n.timers.back().label = "\n";
    assert(s.Apply(n, 30000) == ApplyResult::Malformed);
    s.Tick(60000);
    assert(Due(s) == (std::vector<std::string>{Id(0x10), Id(0x11)}));
}
void ScopeMismatch() {
    for (int mode = 0; mode != 2; ++mode) {
        auto s = Started();
        auto n = Later(s);
        if (mode == 0)
            n.scope.device_id = Id(99);
        else
            n.scope.assignment_id = Id(99);
        assert(s.Apply(n, 0) == ApplyResult::ScopeMismatch);
    }
    Scheduler unassigned({});
    assert(unassigned.Apply(First(), 0) == ApplyResult::ScopeMismatch);
}
void Capacity() {
    auto n = First();
    n.cues.clear();
    n.timers.clear();
    for (unsigned i = 0; i < 64; ++i)
        n.timers.push_back({Id(i + 100), 1, "Timer", n.server_now_ms});
    Scheduler s(n.scope);
    assert(s.Apply(n, 0) == ApplyResult::Applied && Due(s).size() == 64);
    ++n.snapshot_revision;
    n.timers.push_back({Id(999), 1, "Overflow", n.server_now_ms});
    assert(s.Apply(n, 0) == ApplyResult::Malformed && Due(s).size() == 64);
}
void RetiredCannotReturn() {
    auto s = Started();
    auto n = Later(s);
    n.timers.erase(n.timers.begin());
    assert(s.Apply(n, 0) == ApplyResult::Applied);
    n = Later(s);
    n.timers.push_back(First().timers[0]);
    assert(s.Apply(n, 0) == ApplyResult::RetiredIdentity);
    ++n.timers.back().revision;
    n.service_occurrence_id = Id(99);
    n.cues[0].id = Id(1001);
    n.cues[1].id = Id(1002);
    assert(s.Apply(n, 0) == ApplyResult::RetiredIdentity);
    assert(s.items().size() == 4);
}
void RetiredCapacity() {
    auto n = First();
    n.cues.clear();
    n.timers.clear();
    for (unsigned i = 0; i < 64; ++i)
        n.timers.push_back({Id(i + 100), 1, "Timer", n.server_now_ms});
    Scheduler s(n.scope);
    assert(s.Apply(n, 0) == ApplyResult::Applied);
    ++n.snapshot_revision;
    n.timers = {{Id(1000), 1, "Still running", n.server_now_ms + 1000}};
    assert(s.Apply(n, 0) == ApplyResult::Applied);
    ++n.snapshot_revision;
    n.timers.clear();
    assert(s.Apply(n, 0) == ApplyResult::HistoryCapacity);
    assert(s.items().size() == 1 && s.snapshot()->snapshot_revision == 11);
    s.Tick(1000);
    assert(Due(s) == std::vector<std::string>{Id(1000)});
}
void RestoreWaitsForFreshClock() {
    auto s = Started();
    s.Tick(120000);
    s.Acknowledge(s.items()[2].key);
    PersistentState p;
    assert(s.ExportState(p));
    Scheduler restored(p.snapshot.scope);
    assert(restored.Restore(p));
    assert(!restored.Restore(p));
    const auto before = Due(restored);
    restored.SetConnected(true);
    assert(restored.Tick(999999999) == ClockState::AwaitingFreshTime);
    assert(restored.Apply(p.snapshot, 0) == ApplyResult::Replay);
    assert(restored.clock_state() == ClockState::AwaitingFreshTime && Due(restored) == before);
    auto n = p.snapshot;
    ++n.snapshot_revision;
    n.server_now_ms = p.last_known_epoch_ms + 60000;
    assert(restored.Apply(n, 0) == ApplyResult::Applied);
    assert(restored.items()[2].acknowledged && restored.items()[3].due);
}
void RestoreRetiredHistory() {
    auto s = Started();
    auto n = Later(s);
    n.timers.erase(n.timers.begin());
    assert(s.Apply(n, 0) == ApplyResult::Applied);
    PersistentState p;
    s.ExportState(p);
    Scheduler restored(n.scope);
    assert(restored.Restore(p));
    n = Later(s);
    n.timers.push_back(First().timers[0]);
    assert(restored.Apply(n, 0) == ApplyResult::RetiredIdentity);
}
void CorruptRestore() {
    auto s = Started();
    PersistentState p;
    s.ExportState(p);
    for (int mode = 0; mode != 9; ++mode) {
        auto bad = p;
        if (mode == 0)
            bad.version = 2;
        if (mode == 1)
            bad.items.pop_back();
        if (mode == 2)
            bad.items[0].key.revision = 99;
        if (mode == 3)
            bad.items[0].acknowledged = true;
        if (mode == 4)
            bad.items[0].due = true;
        if (mode == 5)
            bad.last_known_epoch_ms = 0;
        if (mode == 6)
            bad.retired_ids = {Id(99), Id(99)};
        if (mode == 7)
            bad.retired_ids = {bad.items[0].key.id};
        if (mode == 8)
            bad.snapshot.scope.assignment_id = Id(99);
        Scheduler restored(p.snapshot.scope);
        assert(!restored.Restore(bad) && !restored.snapshot());
        assert(restored.Restore(p));
    }
}
void MonotonicRollback() {
    auto s = Started();
    s.Tick(60000);
    const auto due = Due(s);
    assert(s.Tick(10) == ClockState::Invalid);
    assert(s.Tick(9999999) == ClockState::Invalid && Due(s) == due);
    auto fresh = Later(s);
    fresh.server_now_ms += 60000;
    assert(s.Apply(fresh, 50) == ApplyResult::Applied);
    assert(s.items()[2].due);
}
void ServerClockRollback() {
    auto s = Started();
    auto n = Later(s);
    --n.server_now_ms;
    assert(s.Apply(n, 0) == ApplyResult::InvalidClock);
    assert(s.snapshot()->snapshot_revision == 10 && s.now_ms() == First().server_now_ms);
}
void DeliveryDelayNeverRewinds() {
    auto s = Started();
    s.Tick(30000);
    auto n = Later(s);
    n.server_now_ms = First().server_now_ms + 10000;
    assert(s.Apply(n, 40000) == ApplyResult::Applied);
    assert(s.now_ms() == First().server_now_ms + 40000);
}
void InvalidClockAndOverflow() {
    auto s = Started();
    auto n = Later(s);
    assert(s.Apply(n, -1) == ApplyResult::InvalidClock);
    assert(s.snapshot()->snapshot_revision == 10);
    assert(s.Apply(n, 0) == ApplyResult::Applied);
    assert(s.Tick(std::numeric_limits<int64_t>::max()) == ClockState::Invalid);
    auto p = First();
    p.server_now_ms = kMaximumEpochMs;
    Scheduler end(p.scope);
    assert(end.Apply(p, 0) == ApplyResult::Applied);
    assert(end.Tick(1) == ClockState::Invalid);
}
void LongOffline() {
    auto s = Started();
    s.SetConnected(false);
    assert(s.Tick(30LL * 24 * 60 * 60 * 1000) == ClockState::Trusted);
    assert(Due(s).size() == 5 && !s.connected());
}
void ValidUtf8AndLimits() {
    auto n = First();
    n.timers[0].label.clear();
    for (int i = 0; i < 80; ++i)
        n.timers[0].label += "🍳";
    Scheduler s(n.scope);
    assert(s.Apply(n, 0) == ApplyResult::Applied);
    ++n.snapshot_revision;
    ++n.timers[0].revision;
    n.timers[0].label += "é";
    assert(s.Apply(n, 0) == ApplyResult::Malformed);
}
void MalformedFields() {
    for (int mode = 0; mode != 19; ++mode) {
        auto n = First();
        if (mode == 0)
            n.version = 2;
        if (mode == 1)
            n.snapshot_revision = kMaximumRevision + 1;
        if (mode == 2)
            n.service_revision = 0;
        if (mode == 3)
            n.timers[0].revision = 0;
        if (mode == 4)
            n.timers[0].id = "00000000-0000-0000-0000-000000000000";
        if (mode == 5)
            n.timers[0].id = "AAAAAAAA-0000-4000-8000-000000000001";
        if (mode == 6)
            n.timers[0].id = n.cues[0].id;
        if (mode == 7)
            n.timers[0].deadline_ms = kMaximumEpochMs + 1;
        if (mode == 8)
            n.server_now_ms = 0;
        if (mode == 9)
            n.timezone = "";
        if (mode == 10)
            n.cues[0].offset_ms = kMaximumOffsetMs + 1;
        if (mode == 11)
            ++n.cues[0].deadline_ms;
        if (mode == 12)
            n.cues[1].offset_ms = 1;
        if (mode == 13)
            n.timers[0].label = "\xc0\x80";
        if (mode == 14)
            n.timers[0].label = "\xed\xa0\x80";
        if (mode == 15)
            n.timers[0].label = "\xf4\x90\x80\x80";
        if (mode == 16)
            n.timers[0].label = "\xc2\x80";
        if (mode == 17)
            n.timers[0].label = "   ";
        if (mode == 18)
            n.timezone = std::string(65, 'A');
        Scheduler s(First().scope);
        assert(s.Apply(n, 0) == ApplyResult::Malformed && !s.snapshot());
    }
}

int main(int argc, char** argv) {
    const std::map<std::string, std::function<void()>> cases = {
        {"golden_scenario", GoldenScenario},
        {"service_edit", ServiceEdit},
        {"exact_acknowledgement", ExactAcknowledgement},
        {"occurrence_switch", OccurrenceSwitch},
        {"occurrence_cue_cannot_replay", OccurrenceCueCannotReplay},
        {"replay_no_rearm", ReplayNoRearm},
        {"conflicting_snapshot", ConflictingSnapshot},
        {"conflicting_items", ConflictingItems},
        {"lower_item_revision", LowerItemRevision},
        {"service_revision", ServiceRevision},
        {"atomic_malformed", AtomicMalformed},
        {"scope_mismatch", ScopeMismatch},
        {"capacity", Capacity},
        {"retired_cannot_return", RetiredCannotReturn},
        {"retired_capacity", RetiredCapacity},
        {"restore_waits_for_fresh_clock", RestoreWaitsForFreshClock},
        {"restore_retired_history", RestoreRetiredHistory},
        {"corrupt_restore", CorruptRestore},
        {"monotonic_rollback", MonotonicRollback},
        {"server_clock_rollback", ServerClockRollback},
        {"delivery_delay_never_rewinds", DeliveryDelayNeverRewinds},
        {"invalid_clock_and_overflow", InvalidClockAndOverflow},
        {"long_offline", LongOffline},
        {"valid_utf8_and_limits", ValidUtf8AndLimits},
        {"malformed_fields", MalformedFields},
    };
    assert(argc == 2);
    if (std::string(argv[1]) == "--list") {
        for (const auto& [name, _] : cases)
            std::puts(name.c_str());
        return 0;
    }
    cases.at(argv[1])();
}
