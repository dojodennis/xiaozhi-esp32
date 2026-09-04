#ifndef PROVISIONS_TIMER_SNAPSHOT_H_
#define PROVISIONS_TIMER_SNAPSHOT_H_

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

struct cJSON;

namespace ProvisionsTimerSnapshot {

inline constexpr int kVersion = 1;
inline constexpr std::size_t kMaximumTimers = 64;
inline constexpr std::size_t kMaximumLabelScalars = 80;

enum class TimerStatus : uint8_t {
    kActive,
    kAttention,
};

struct Timer {
    std::string id;
    std::string label;
    int64_t deadline_ms = 0;
    TimerStatus status = TimerStatus::kActive;
};

struct Snapshot {
    std::string session_id;
    int64_t revision = 0;
    int64_t server_now_ms = 0;
    std::vector<Timer> timers;
};

struct Update {
    enum class Kind : uint8_t {
        kSnapshot,
        kReset,
    };

    Kind kind = Kind::kSnapshot;
    Snapshot snapshot;
};

enum class ApplyResult : uint8_t {
    kAccepted,
    kMalformed,
    kWebsocketSessionMismatch,
    kStaleRevision,
};

// Validates a complete timer_snapshot frame before changing revision state.
// Same-session revisions must increase; a new galley session starts a fresh
// revision sequence.
class Gate {
public:
    ApplyResult Apply(const cJSON* frame, std::string_view websocket_session_id,
                      Update& accepted_update);

private:
    std::mutex mutex_;
    bool has_snapshot_ = false;
    std::string session_id_;
    int64_t revision_ = 0;
};

}  // namespace ProvisionsTimerSnapshot

#endif  // PROVISIONS_TIMER_SNAPSHOT_H_
