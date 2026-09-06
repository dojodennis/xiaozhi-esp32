#ifndef PROVISIONS_TIMERS_H_
#define PROVISIONS_TIMERS_H_

#include <cJSON.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace provisions::timers {

constexpr size_t kMaximumTimers = 32;
constexpr uint64_t kMaximumRevision = 2147483647;

enum class State { Active, Expired };
struct Timer {
    std::string id;
    uint32_t spoken_number = 0;
    std::string label;
    int64_t deadline_ms = 0;
    uint64_t revision = 0;
    State state = State::Active;
};
struct Snapshot {
    std::string session_id;
    std::string request_id;
    std::vector<Timer> timers;
};
struct Alarm {
    std::string session_id;
    std::string lease_id;
    std::string playback_id;
    std::string timer_id;
    uint64_t timer_revision = 0;
    uint32_t attempt = 0;
    uint32_t spoken_number = 0;
    std::string label;
    std::string audio_sha256;
    uint32_t packet_count = 0;
};

enum class Outcome { Unknown, Completed, Failed, Interrupted };
struct Record {
    Alarm alarm;
    Outcome outcome = Outcome::Unknown;
};

// Bound cJSON recursion as well as bytes before parsing the larger snapshot frame.
bool WithinJsonBudget(std::string_view text);
bool ParseTimestamp(const cJSON* value, int64_t& unix_ms);
bool ParseSnapshot(const cJSON* root, Snapshot& output);
bool ParseAlarm(const cJSON* root, Alarm& output);
bool MatchesTts(const cJSON* root, const Alarm& alarm, std::string& state);
bool MatchesAck(const cJSON* root, const Alarm& alarm, const std::string& transport_session);
std::string RecordJson(const Record& record);
bool ParseRecord(const std::string& text, Record& record);
std::string ReceiptJson(const Record& record, const std::string& transport_session);
bool IsSixtyMsOpus(const std::vector<uint8_t>& packet);
bool SameAttempt(const Alarm& a, const Alarm& b);
// Presentation only: passing a deadline never starts or acknowledges an alarm.
std::string DisplayText(const Snapshot& snapshot, int64_t trusted_now_ms);

}  // namespace provisions::timers
#endif
