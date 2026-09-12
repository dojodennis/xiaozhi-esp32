#include "provisions_timer_dial_link.h"
namespace provisions::timers {
bool DialLink::Reconcile(const Snapshot& snapshot, const std::string& session, bool negotiated,
                         int64_t trusted_now_ms, ProvisionsTimerSnapshot::Update& update) {
    // A live, negotiated session is the only authority that may empty the dial.
    // Losing the socket used to clear it, so a six-second reconnect blanked
    // three running timers mid-countdown (Dennis, 12 Sept). The ring rings from
    // its own list, so the countdown stays on screen while the link is down and
    // corrects within a second of reconnecting.
    const bool live = negotiated && !session.empty() && snapshot.session_id == session &&
                      trusted_now_ms > 0;
    if (!live) {
        return false;
    }
    if (snapshot.timers.empty()) {
        if (!applied_)
            return false;
        applied_ = false;
        session_.clear();
        keys_.clear();
        update = {};
        update.kind = ProvisionsTimerSnapshot::Update::Kind::kReset;
        return true;
    }
    std::vector<Key> keys;
    keys.reserve(snapshot.timers.size());
    for (const auto& timer : snapshot.timers)
        keys.push_back({timer.id, timer.revision, timer.deadline_ms, timer.state});
    if (applied_ && session_ == session && keys_ == keys)
        return false;
    if (session_ != session)
        revision_ = 0;
    update = {};
    update.kind = ProvisionsTimerSnapshot::Update::Kind::kSnapshot;
    update.snapshot.session_id = session;
    update.snapshot.revision = ++revision_;
    update.snapshot.server_now_ms = trusted_now_ms;
    for (const auto& timer : snapshot.timers) {
        ProvisionsTimerSnapshot::Timer entry;
        entry.id = timer.id;
        entry.label = timer.label;
        entry.deadline_ms = timer.deadline_ms;
        entry.status = timer.state == State::Expired
                           ? ProvisionsTimerSnapshot::TimerStatus::kAttention
                           : ProvisionsTimerSnapshot::TimerStatus::kActive;
        update.snapshot.timers.push_back(std::move(entry));
    }
    applied_ = true;
    session_ = session;
    keys_ = std::move(keys);
    return true;
}
}  // namespace provisions::timers
