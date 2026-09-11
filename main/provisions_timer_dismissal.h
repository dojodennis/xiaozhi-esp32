#ifndef PROVISIONS_TIMER_DISMISSAL_H_
#define PROVISIONS_TIMER_DISMISSAL_H_
#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include "provisions_timers.h"

struct cJSON;

namespace provisions::timers {
// Reports a physical dismissal of ring-local alarms to the gateway.
//
// Contract (device -> gateway), one frame per due timer being dismissed:
//   {"session_id":"<current session>","type":"timer","action":"dismiss",
//    "timer_id":"<uuid from the snapshot>","revision":<int from the snapshot>,
//    "request_id":"<new uuid4>"}
// Gateway -> device:
//   {"type":"timer","action":"dismiss_ack","session_id":...,"timer_id":...,
//    "request_id":...,"status":"settled"|"unavailable"} then a fresh snapshot.
//
// A frame without an ack after 5 s is resent unchanged (same request_id), at
// most twice; after that the server's two-minute overdue sweep settles it.
// Nothing here blocks the UI: the takeover clears locally at the gesture.
// A dismissed timer stays hidden until a snapshot of the current session no
// longer contains it or carries a higher revision, so a stale snapshot in
// flight cannot resurrect the takeover.
class Dismissals {
public:
    static constexpr int64_t kAckTimeoutUs = 5LL * 1000 * 1000;
    static constexpr uint32_t kMaximumSends = 3;  // First send plus two retries.
    static constexpr size_t kMaximumEntries = kMaximumTimers;
    using RequestId = std::function<std::string()>;
    using Send = std::function<bool(const std::string&)>;

    // Records every due timer in `snapshot` as dismissed. Only when timers are
    // negotiated and the snapshot belongs to the current session; otherwise it
    // does nothing and returns 0. Returns the number of timers dismissed.
    size_t Dismiss(const Snapshot& snapshot, const std::string& session, bool negotiated,
                   int64_t trusted_now_ms, int64_t now_us, const RequestId& request_id);
    // Sends first attempts and retries that are due. Main task.
    void Service(const std::string& session, bool negotiated, int64_t now_us, const Send& send);
    // Network task. Returns false only for a malformed dismiss_ack frame; a
    // well-formed ack for an unknown or abandoned request is accepted silently.
    bool OnAck(const cJSON* root, const std::string& transport_session);
    // Removes suppressed timers from a snapshot and releases suppressions the
    // snapshot proves obsolete. An empty (disconnected) snapshot proves nothing.
    void Filter(Snapshot& snapshot);
    size_t PendingCount() const;

    static std::string FrameJson(const std::string& session, const std::string& timer_id,
                                 uint64_t revision, const std::string& request_id);
    static std::string FormatUuidV4(std::array<uint8_t, 16> bytes);

private:
    struct Pending {
        std::string timer_id;
        uint64_t revision = 0;
        std::string request_id;
        uint32_t sends = 0;
        int64_t last_send_us = 0;
    };
    struct Suppressed {
        std::string timer_id;
        uint64_t revision = 0;
    };
    mutable std::mutex mutex_;
    std::vector<Pending> pending_;
    std::vector<Suppressed> suppressed_;
};
}  // namespace provisions::timers
#endif
