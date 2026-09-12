#ifndef PROVISIONS_TIMER_DIAL_LINK_H_
#define PROVISIONS_TIMER_DIAL_LINK_H_
#include <cstdint>
#include <string>
#include <vector>
#include "provisions_timer_snapshot.h"
#include "provisions_timers.h"
namespace provisions::timers {
// Feeds the crest timer dial from the ring's own timer player. Until this link
// existed the dial was driven only by the phone-style "timer_snapshot" frame,
// which no gateway sends, so ring-created timers reached a text line and nothing
// else. Presentation only: it never starts, acknowledges or cancels an alarm.
//
// Mapping: State::Active -> TimerStatus::kActive; State::Expired (ringing) ->
// TimerStatus::kAttention; a timer the gateway dropped from the snapshot
// (cancelled/acknowledged) is removed. Main task only.
class DialLink {
public:
    // Returns true when `update` must be delivered to the dial consumer. An
    // unchanged snapshot never repaints. Exactly one kReset is emitted when a
    // LIVE session reports no timers; losing the session, the negotiation or
    // the trusted clock leaves the dial exactly as it is, because a countdown
    // that survives a reconnect is worth more than one that blanks.
    bool Reconcile(const Snapshot& snapshot, const std::string& session, bool negotiated,
                   int64_t trusted_now_ms, ProvisionsTimerSnapshot::Update& update);

private:
    struct Key {
        std::string id;
        uint64_t revision = 0;
        int64_t deadline_ms = 0;
        State state = State::Active;
        bool operator==(const Key& other) const {
            return id == other.id && revision == other.revision &&
                   deadline_ms == other.deadline_ms && state == other.state;
        }
    };
    bool applied_ = false;
    int64_t revision_ = 0;
    std::string session_;
    std::vector<Key> keys_;
};
}  // namespace provisions::timers
#endif
