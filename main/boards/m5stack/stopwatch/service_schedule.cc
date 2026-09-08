#include "service_schedule.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace orbit::service_schedule {
namespace {
bool Same(const Scope& a, const Scope& b) {
    return a.assignment_id == b.assignment_id && a.device_id == b.device_id;
}
bool Same(const AlarmKey& a, const AlarmKey& b) {
    return Same(a.scope, b.scope) && a.kind == b.kind &&
           a.service_occurrence_id == b.service_occurrence_id && a.id == b.id &&
           a.revision == b.revision;
}
bool Same(const Cue& a, const Cue& b) {
    return a.id == b.id && a.revision == b.revision && a.label == b.label && a.kind == b.kind &&
           a.deadline_ms == b.deadline_ms && a.offset_ms == b.offset_ms;
}
bool Same(const Timer& a, const Timer& b) {
    return a.id == b.id && a.revision == b.revision && a.label == b.label &&
           a.deadline_ms == b.deadline_ms;
}
template <typename T>
bool SameItems(const std::vector<T>& a, const std::vector<T>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
                                              [](const T& x, const T& y) { return Same(x, y); });
}
bool Same(const Snapshot& a, const Snapshot& b) {
    return a.version == b.version && Same(a.scope, b.scope) &&
           a.service_occurrence_id == b.service_occurrence_id &&
           a.service_revision == b.service_revision && a.snapshot_revision == b.snapshot_revision &&
           a.service_at_ms == b.service_at_ms && a.timezone == b.timezone &&
           a.server_now_ms == b.server_now_ms && SameItems(a.cues, b.cues) &&
           SameItems(a.timers, b.timers);
}
bool Uuid(std::string_view s) {
    if (s.size() != 36)
        return false;
    bool nonzero = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-')
                return false;
        } else {
            if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
                return false;
            nonzero = nonzero || s[i] != '0';
        }
    }
    return nonzero;
}
bool Valid(const Scope& s) { return Uuid(s.assignment_id) && Uuid(s.device_id); }
bool Revision(uint64_t revision) { return revision > 0 && revision <= kMaximumRevision; }
bool Epoch(int64_t epoch) { return epoch > 0 && epoch <= kMaximumEpochMs; }
bool Label(std::string_view s) {
    if (s.empty() || s.size() > 320 || s.find_first_not_of(' ') == std::string_view::npos)
        return false;
    size_t count = 0;
    for (size_t i = 0; i < s.size();) {
        const auto lead = static_cast<unsigned char>(s[i++]);
        uint32_t cp = lead;
        size_t tail = 0;
        uint32_t minimum = 0;
        if (lead >= 0xc2 && lead <= 0xdf) {
            tail = 1;
            cp = lead & 0x1f;
            minimum = 0x80;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            tail = 2;
            cp = lead & 0x0f;
            minimum = 0x800;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            tail = 3;
            cp = lead & 7;
            minimum = 0x10000;
        } else if (lead >= 0x80)
            return false;
        if (s.size() - i < tail)
            return false;
        while (tail--) {
            const auto byte = static_cast<unsigned char>(s[i++]);
            if ((byte & 0xc0) != 0x80)
                return false;
            cp = (cp << 6) | (byte & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || cp < 32 ||
            (cp >= 0x7f && cp <= 0x9f) || ++count > 80)
            return false;
    }
    return true;
}
bool Zone(std::string_view s) {
    if (s.empty() || s.size() > 128)
        return false;
    for (unsigned char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '/' || c == '_' || c == '-' || c == '+'))
            return false;
    return true;  // IANA membership is validated upstream; no tz database on this device.
}
bool Valid(const Snapshot& s) {
    if (s.version != 1 || !Valid(s.scope) || !Uuid(s.service_occurrence_id) ||
        !Revision(s.service_revision) || !Revision(s.snapshot_revision) ||
        !Epoch(s.service_at_ms) || !Epoch(s.server_now_ms) || !Zone(s.timezone) ||
        s.cues.size() > kMaximumItems || s.timers.size() > kMaximumItems - s.cues.size())
        return false;
    std::vector<std::string_view> ids;
    ids.reserve(s.cues.size() + s.timers.size());
    auto item = [&](const auto& entry) {
        if (!Uuid(entry.id) || !Revision(entry.revision) || !Label(entry.label) ||
            !Epoch(entry.deadline_ms) || std::find(ids.begin(), ids.end(), entry.id) != ids.end())
            return false;
        ids.push_back(entry.id);
        return true;
    };
    for (const auto& cue : s.cues) {
        if (!item(cue))
            return false;
        if (cue.kind == CueKind::ServiceOffset) {
            if (cue.offset_ms < -kMaximumOffsetMs || cue.offset_ms > kMaximumOffsetMs ||
                cue.deadline_ms != s.service_at_ms + cue.offset_ms)
                return false;
        } else if (cue.kind != CueKind::Fixed || cue.offset_ms != 0)
            return false;
    }
    for (const auto& timer : s.timers)
        if (!item(timer))
            return false;
    return true;
}
std::vector<ItemState> NewItems(const Snapshot& s) {
    std::vector<ItemState> out;
    out.reserve(s.cues.size() + s.timers.size());
    for (const auto& cue : s.cues)
        out.push_back({{s.scope, ItemKind::Cue, s.service_occurrence_id, cue.id, cue.revision}});
    for (const auto& timer : s.timers)
        out.push_back({{s.scope, ItemKind::Timer, {}, timer.id, timer.revision}});
    return out;
}
int64_t Deadline(const Snapshot& s, size_t index) {
    return index < s.cues.size() ? s.cues[index].deadline_ms
                                 : s.timers[index - s.cues.size()].deadline_ms;
}
template <typename T>
ApplyResult CheckItems(const std::vector<T>& old_items, const std::vector<T>& next_items) {
    for (const auto& next : next_items) {
        const auto previous = std::find_if(old_items.begin(), old_items.end(),
                                           [&](const T& item) { return item.id == next.id; });
        if (previous == old_items.end())
            continue;
        if (next.revision < previous->revision)
            return ApplyResult::StaleRevision;
        if (next.revision == previous->revision && !Same(next, *previous))
            return ApplyResult::ConflictingRevision;
    }
    return ApplyResult::Applied;
}
}  // namespace

Scheduler::Scheduler(Scope enrolled_scope) : scope_(std::move(enrolled_scope)) {}

ApplyResult Scheduler::Apply(const Snapshot& next, int64_t monotonic_ms) {
    if (!Valid(next))
        return ApplyResult::Malformed;
    if (!Valid(scope_) || !Same(scope_, next.scope))
        return ApplyResult::ScopeMismatch;
    if (has_snapshot_) {
        if (next.snapshot_revision < snapshot_.snapshot_revision)
            return ApplyResult::StaleRevision;
        if (next.snapshot_revision == snapshot_.snapshot_revision)
            return Same(next, snapshot_) ? ApplyResult::Replay : ApplyResult::ConflictingRevision;
        if (next.service_occurrence_id == snapshot_.service_occurrence_id) {
            if (next.service_revision < snapshot_.service_revision)
                return ApplyResult::StaleRevision;
            if (next.service_revision == snapshot_.service_revision &&
                (next.service_at_ms != snapshot_.service_at_ms ||
                 next.timezone != snapshot_.timezone))
                return ApplyResult::ConflictingRevision;
            const auto cue_check = CheckItems(snapshot_.cues, next.cues);
            if (cue_check != ApplyResult::Applied)
                return cue_check;
        }
        const auto timer_check = CheckItems(snapshot_.timers, next.timers);
        if (timer_check != ApplyResult::Applied)
            return timer_check;
        // Do not allow an existing active ID to switch category and evade revisions.
        for (const auto& cue : next.cues)
            for (const auto& timer : snapshot_.timers)
                if (cue.id == timer.id)
                    return ApplyResult::ConflictingRevision;
        for (const auto& timer : next.timers)
            for (const auto& cue : snapshot_.cues)
                if (timer.id == cue.id)
                    return ApplyResult::ConflictingRevision;
    }
    auto next_items = NewItems(next);
    auto next_retired = retired_ids_;
    for (const auto& item : next_items)
        if (std::find(retired_ids_.begin(), retired_ids_.end(), item.key.id) != retired_ids_.end())
            return ApplyResult::RetiredIdentity;
    for (const auto& old : items_) {
        if (std::none_of(next_items.begin(), next_items.end(),
                         [&](const ItemState& item) { return item.key.id == old.key.id; })) {
            if (next_retired.size() == kMaximumRetiredIds)
                return ApplyResult::HistoryCapacity;
            next_retired.push_back(old.key.id);
        }
    }
    int64_t projected_now = last_known_epoch_ms_;
    if (monotonic_ms < 0 ||
        (clock_state_ == ClockState::Trusted && monotonic_ms < last_monotonic_ms_) ||
        (has_snapshot_ && next.server_now_ms < snapshot_.server_now_ms) ||
        (clock_state_ != ClockState::Trusted && has_snapshot_ &&
         next.server_now_ms < last_known_epoch_ms_)) {
        clock_state_ = ClockState::Invalid;
        return ApplyResult::InvalidClock;
    }
    if (clock_state_ == ClockState::Trusted) {
        const auto elapsed = monotonic_ms - last_monotonic_ms_;
        if (elapsed > kMaximumEpochMs - projected_now) {
            clock_state_ = ClockState::Invalid;
            return ApplyResult::InvalidClock;
        }
        projected_now += elapsed;
    }
    for (auto& item : next_items) {
        const auto old = std::find_if(items_.begin(), items_.end(), [&](const ItemState& prior) {
            return Same(item.key, prior.key);
        });
        if (old != items_.end())
            item = *old;
    }
    snapshot_ = next;
    items_ = std::move(next_items);
    retired_ids_ = std::move(next_retired);
    has_snapshot_ = true;
    last_monotonic_ms_ = monotonic_ms;
    // Variable delivery delay must never wind a running countdown backwards.
    last_known_epoch_ms_ = std::max(projected_now, next.server_now_ms);
    clock_state_ = ClockState::Trusted;
    LatchDue();
    return ApplyResult::Applied;
}

ClockState Scheduler::Tick(int64_t monotonic_ms) {
    if (clock_state_ != ClockState::Trusted)
        return clock_state_;
    if (monotonic_ms < last_monotonic_ms_ || monotonic_ms < 0 ||
        monotonic_ms - last_monotonic_ms_ > kMaximumEpochMs - last_known_epoch_ms_) {
        clock_state_ = ClockState::Invalid;
        return clock_state_;
    }
    last_known_epoch_ms_ += monotonic_ms - last_monotonic_ms_;
    last_monotonic_ms_ = monotonic_ms;
    LatchDue();
    return clock_state_;
}

void Scheduler::LatchDue() {
    for (size_t i = 0; i < items_.size(); ++i)
        if (Deadline(snapshot_, i) <= last_known_epoch_ms_)
            items_[i].due = true;
}

AckResult Scheduler::Acknowledge(const AlarmKey& key) {
    for (auto& item : items_) {
        if (!Same(key, item.key))
            continue;
        if (item.acknowledged)
            return AckResult::AlreadyAcknowledged;
        if (!item.due)
            return AckResult::NotDue;
        item.acknowledged = true;
        return AckResult::Acknowledged;
    }
    return AckResult::NotFound;
}

bool Scheduler::ExportState(PersistentState& output) const {
    if (!has_snapshot_)
        return false;
    output = {1, snapshot_, items_, last_known_epoch_ms_, retired_ids_};
    return true;
}

bool Scheduler::Restore(const PersistentState& saved) {
    if (has_snapshot_ || saved.version != 1 || !Valid(saved.snapshot) ||
        !Same(saved.snapshot.scope, scope_) || !Epoch(saved.last_known_epoch_ms) ||
        saved.last_known_epoch_ms < saved.snapshot.server_now_ms)
        return false;
    const auto expected = NewItems(saved.snapshot);
    if (expected.size() != saved.items.size())
        return false;
    if (saved.retired_ids.size() > kMaximumRetiredIds)
        return false;
    for (size_t i = 0; i < saved.retired_ids.size(); ++i) {
        const auto& id = saved.retired_ids[i];
        if (!Uuid(id) ||
            std::find(saved.retired_ids.begin(), saved.retired_ids.begin() + i, id) !=
                saved.retired_ids.begin() + i ||
            std::any_of(expected.begin(), expected.end(),
                        [&](const ItemState& item) { return item.key.id == id; }))
            return false;
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        const auto& item = saved.items[i];
        if (!Same(item.key, expected[i].key) || (item.acknowledged && !item.due) ||
            (item.due && Deadline(saved.snapshot, i) > saved.last_known_epoch_ms))
            return false;
    }
    snapshot_ = saved.snapshot;
    items_ = saved.items;
    retired_ids_ = saved.retired_ids;
    last_known_epoch_ms_ = saved.last_known_epoch_ms;
    has_snapshot_ = true;
    clock_state_ = ClockState::AwaitingFreshTime;
    return true;
}

}  // namespace orbit::service_schedule
