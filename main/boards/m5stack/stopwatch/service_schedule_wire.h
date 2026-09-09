#ifndef PROVISIONS_STOPWATCH_SERVICE_SCHEDULE_WIRE_H_
#define PROVISIONS_STOPWATCH_SERVICE_SCHEDULE_WIRE_H_

#include "service_schedule.h"

#include <string_view>

namespace orbit::service_schedule::wire {

inline constexpr size_t kMaximumFrameBytes = 32 * 1024;
inline constexpr size_t kMaximumJsonDepth = 16;

struct Context {
    std::string_view websocket_session_id;
    Scope enrolled_scope;
    // Empty means positively authorized absence (v2 only), never unknown.
    std::string_view service_occurrence_id;
};

struct Validators {
    // Mandatory policy validators: complete Unicode control-category rejection,
    // no surrounding Unicode whitespace, and IANA timezone membership. Missing
    // callbacks fail closed. The decoder independently checks UTF-8, scalar/byte
    // bounds, C0/C1, surrounding ASCII spaces and zone syntax.
    bool (*label)(std::string_view) = nullptr;
    bool (*timezone)(std::string_view) = nullptr;
};

enum class Result {
    Accepted,
    Malformed,
    MissingValidators,
    SessionMismatch,
    ScopeMismatch,
    OccurrenceMismatch,
};

// Strict complete raw-frame decoder for the opt-in v1/v2 service schedule envelopes.
// Output changes only on success. No capability advertisement or network hooks.
//
// Context MUST come from the already authenticated current transport/enrollment
// and authorized occurrence, never from this payload. Matching supplied IDs does
// not authenticate a sender. The caller must establish fresh authenticated server
// time before passing a v1 snapshot to Scheduler::Apply. V2 snapshots carry no
// clock sample and require a separately owned clock exchange. Decoding a cached
// frame, reconnecting, or observing server_now_ms alone cannot establish freshness.
// Scheduler still owns revision, retirement, edit and monotonic-clock state checks.
//
// The raw bytes must be supplied before any JSON parse/normalization: cJSON's
// numeric representation cannot distinguish 1 from 1.0/1e0, and C strings cannot
// preserve escaped NUL. This decoder rejects those lexical forms before parsing.
Result Decode(std::string_view bytes, const Context& context, const Validators& validators,
              Snapshot& output);

// Decode only. Context binds the authenticated socket and enrolled scope; its
// occurrence field is unused here. Scheduler consumes the locally owned nonce,
// revision and measured <=2000 ms response window. Parsing does not prove freshness.
Result DecodeClock(std::string_view bytes, const Context& context, ClockResponse& output);
bool EncodeClockRequest(const ClockRequest& request, std::string& output);

}  // namespace orbit::service_schedule::wire
#endif  // PROVISIONS_STOPWATCH_SERVICE_SCHEDULE_WIRE_H_
