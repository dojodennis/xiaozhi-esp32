#include "provisions_timers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string_view>

#include "provisions_tts_text.h"

namespace provisions::timers {
namespace {
bool Keys(const cJSON* object, std::initializer_list<const char*> keys) {
    if (!cJSON_IsObject(object) || cJSON_GetArraySize(object) != static_cast<int>(keys.size()))
        return false;
    for (const char* key : keys) {
        unsigned matches = 0;
        const cJSON* item;
        cJSON_ArrayForEach (item, object)
            if (item->string && std::strcmp(item->string, key) == 0)
                ++matches;
        if (matches != 1)
            return false;
    }
    return true;
}
const cJSON* Field(const cJSON* object, const char* key) {
    return cJSON_GetObjectItemCaseSensitive(object, key);
}
bool Text(const cJSON* value, const char* expected) {
    return cJSON_IsString(value) && std::strcmp(value->valuestring, expected) == 0;
}
bool Id(const cJSON* value, std::string& output) {
    if (!cJSON_IsString(value) || std::strlen(value->valuestring) != 36)
        return false;
    bool nonzero = false;
    for (size_t i = 0; i < 36; ++i) {
        const char c = value->valuestring[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-')
                return false;
        } else {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                return false;
            nonzero = nonzero || c != '0';
        }
    }
    if (!nonzero)
        return false;
    if (value->valuestring[14] < '1' || value->valuestring[14] > '8')
        return false;
    const char variant = value->valuestring[19];
    if (variant != '8' && variant != '9' && variant != 'a' && variant != 'b')
        return false;
    output = value->valuestring;
    return true;
}
bool Number(const cJSON* value, uint64_t maximum, uint64_t& output) {
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) || value->valuedouble < 1 ||
        value->valuedouble > static_cast<double>(maximum) ||
        std::floor(value->valuedouble) != value->valuedouble)
        return false;
    output = static_cast<uint64_t>(value->valuedouble);
    return true;
}
bool Label(const cJSON* value, std::string& output) {
    if (!cJSON_IsString(value))
        return false;
    const std::string_view text(value->valuestring);
    if (text.size() > 320 || !ProvisionsTtsText::IsValid(text))
        return false;
    size_t scalars = 0;
    for (unsigned char c : text)
        if ((c & 0xc0) != 0x80)
            ++scalars;
    if (scalars > 80 || text.find_first_not_of(' ') == std::string_view::npos)
        return false;
    output = text;
    return true;
}
bool Deadline(const cJSON* value, int64_t& output) {
    if (!cJSON_IsString(value))
        return false;
    const std::string_view text(value->valuestring);
    if (text.size() < 20 || text.size() > 32 || text[4] != '-' || text[7] != '-' ||
        (text[10] != 'T' && text[10] != 't') || text[13] != ':' || text[16] != ':')
        return false;
    auto digits = [&](size_t at, size_t count) {
        int number = 0;
        for (size_t i = 0; i < count; ++i) {
            const char c = text[at + i];
            if (c < '0' || c > '9')
                return -1;
            number = number * 10 + c - '0';
        }
        return number;
    };
    int year = digits(0, 4), month = digits(5, 2), day = digits(8, 2);
    const int hour = digits(11, 2), minute = digits(14, 2), second = digits(17, 2);
    if (year < 1970 || month < 1 || month > 12 || day < 1 || hour < 0 || hour > 23 || minute < 0 ||
        minute > 59 || second < 0 || second > 59)
        return false;
    constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (day > days[month - 1] + (month == 2 && leap ? 1 : 0))
        return false;
    size_t at = 19;
    int milliseconds = 0;
    if (text[at] == '.') {
        const size_t start = ++at;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
            if (at - start < 3)
                milliseconds = milliseconds * 10 + text[at] - '0';
            ++at;
        }
        const size_t length = at - start;
        if (length == 0 || length > 6)
            return false;
        for (size_t i = length; i < 3; ++i)
            milliseconds *= 10;
    }
    int offset = 0;
    if (text.substr(at) != "Z" && text.substr(at) != "z") {
        if (text.size() - at != 6 || (text[at] != '+' && text[at] != '-') || text[at + 3] != ':')
            return false;
        const int hours = digits(at + 1, 2), minutes = digits(at + 4, 2);
        if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59)
            return false;
        offset = (hours * 60 + minutes) * (text[at] == '+' ? 1 : -1);
    }
    year -= month <= 2;
    const int era = year / 400;
    const unsigned yoe = year - era * 400;
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int64_t epoch_days = era * 146097LL + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    output = ((epoch_days * 24 + hour) * 60 * 60 + (minute - offset) * 60 + second) * 1000 +
             milliseconds;
    return true;
}
}  // namespace
bool ParseTimestamp(const cJSON* value, int64_t& unix_ms) { return Deadline(value, unix_ms); }

bool WithinJsonBudget(std::string_view text) {
    if (text.empty() || text.size() > 32768)
        return false;
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (const char c : text) {
        if (c == '\0')
            return false;
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
        } else if (c == '"')
            quoted = true;
        else if (c == '{' || c == '[') {
            if (++depth > 16)
                return false;
        } else if (c == '}' || c == ']') {
            if (depth == 0)
                return false;
            --depth;
        }
    }
    return !quoted && depth == 0;
}

bool ParseSnapshot(const cJSON* root, Snapshot& output) {
    Snapshot parsed;
    if (!Keys(root, {"type", "action", "session_id", "request_id", "timers"}) ||
        !Text(Field(root, "type"), "timer") || !Text(Field(root, "action"), "snapshot") ||
        !Id(Field(root, "session_id"), parsed.session_id) ||
        !Id(Field(root, "request_id"), parsed.request_id))
        return false;
    auto list = Field(root, "timers");
    if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) > static_cast<int>(kMaximumTimers))
        return false;
    const cJSON* item;
    cJSON_ArrayForEach (item, list) {
        Timer timer;
        uint64_t number;
        if (!Keys(item, {"id", "spoken_number", "label", "deadline_at", "revision", "state"}) ||
            !Id(Field(item, "id"), timer.id) || !Label(Field(item, "label"), timer.label) ||
            !Number(Field(item, "spoken_number"), 99, number) ||
            !Number(Field(item, "revision"), kMaximumRevision, timer.revision) ||
            !Deadline(Field(item, "deadline_at"), timer.deadline_ms))
            return false;
        timer.spoken_number = static_cast<uint32_t>(number);
        const auto state = Field(item, "state");
        if (Text(state, "active"))
            timer.state = State::Active;
        else if (Text(state, "expired"))
            timer.state = State::Expired;
        else
            return false;
        for (const auto& prior : parsed.timers)
            if (prior.id == timer.id || prior.spoken_number == timer.spoken_number)
                return false;
        parsed.timers.push_back(std::move(timer));
    }
    output = std::move(parsed);
    return true;
}

bool ParseAlarm(const cJSON* root, Alarm& output) {
    Alarm parsed;
    uint64_t attempt, number, count;
    if (!Keys(root,
              {"type", "action", "session_id", "playback_id", "timer_id", "timer_revision",
               "attempt", "spoken_number", "label", "lease_id", "audio_sha256", "packet_count"}) ||
        !Text(Field(root, "type"), "timer") || !Text(Field(root, "action"), "alarm") ||
        !Id(Field(root, "session_id"), parsed.session_id) ||
        !Id(Field(root, "lease_id"), parsed.lease_id) ||
        !Id(Field(root, "playback_id"), parsed.playback_id) ||
        !Id(Field(root, "timer_id"), parsed.timer_id) ||
        !Label(Field(root, "label"), parsed.label) ||
        !Number(Field(root, "timer_revision"), kMaximumRevision, parsed.timer_revision) ||
        !Number(Field(root, "attempt"), 2147483647, attempt) ||
        !Number(Field(root, "spoken_number"), 99, number) ||
        !Number(Field(root, "packet_count"), 500, count))
        return false;
    auto hash = Field(root, "audio_sha256");
    if (!cJSON_IsString(hash) || std::strlen(hash->valuestring) != 64)
        return false;
    for (size_t i = 0; i < 64; ++i) {
        const char c = hash->valuestring[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    parsed.audio_sha256 = hash->valuestring;
    parsed.packet_count = static_cast<uint32_t>(count);
    parsed.attempt = static_cast<uint32_t>(attempt);
    parsed.spoken_number = static_cast<uint32_t>(number);
    output = std::move(parsed);
    return true;
}

bool SameAttempt(const Alarm& a, const Alarm& b) {
    return a.session_id == b.session_id && a.lease_id == b.lease_id &&
           a.playback_id == b.playback_id && a.timer_id == b.timer_id &&
           a.timer_revision == b.timer_revision && a.attempt == b.attempt;
}

namespace {
bool Identity(const cJSON* root, const Alarm& alarm, const std::string& session, bool lease) {
    uint64_t revision, attempt;
    return Text(Field(root, "session_id"), session.c_str()) &&
           (!lease || Text(Field(root, "lease_id"), alarm.lease_id.c_str())) &&
           Text(Field(root, "playback_id"), alarm.playback_id.c_str()) &&
           Text(Field(root, "timer_id"), alarm.timer_id.c_str()) &&
           Number(Field(root, "timer_revision"), kMaximumRevision, revision) &&
           revision == alarm.timer_revision &&
           Number(Field(root, "attempt"), kMaximumRevision, attempt) && attempt == alarm.attempt;
}
const char* OutcomeText(Outcome outcome) {
    switch (outcome) {
        case Outcome::Completed:
            return "completed";
        case Outcome::Failed:
            return "failed";
        case Outcome::Interrupted:
            return "interrupted";
        default:
            return "unknown";
    }
}
std::string Print(cJSON* root) {
    if (!root)
        return {};
    char* text = cJSON_PrintUnformatted(root);
    std::string result = text ? text : "";
    cJSON_free(text);
    cJSON_Delete(root);
    return result;
}
void AddIdentity(cJSON* root, const Alarm& alarm, const std::string& session) {
    cJSON_AddStringToObject(root, "session_id", session.c_str());
    cJSON_AddStringToObject(root, "lease_id", alarm.lease_id.c_str());
    cJSON_AddStringToObject(root, "playback_id", alarm.playback_id.c_str());
    cJSON_AddStringToObject(root, "timer_id", alarm.timer_id.c_str());
    cJSON_AddNumberToObject(root, "timer_revision", alarm.timer_revision);
    cJSON_AddNumberToObject(root, "attempt", alarm.attempt);
}
}  // namespace
bool MatchesTts(const cJSON* root, const Alarm& alarm, std::string& state) {
    const auto value = Field(root, "state");
    const bool sentence = Text(value, "sentence_start");
    if (sentence) {
        if (!Keys(root, {"type", "state", "session_id", "playback_id", "timer_id", "timer_revision",
                         "attempt", "text"}))
            return false;
        const auto text = Field(root, "text");
        if (!cJSON_IsString(text) || !ProvisionsTtsText::IsValid(text->valuestring))
            return false;
    } else if (!Keys(root, {"type", "state", "session_id", "playback_id", "timer_id",
                            "timer_revision", "attempt"}) ||
               (!Text(value, "start") && !Text(value, "stop")))
        return false;
    if (!Text(Field(root, "type"), "tts") || !Identity(root, alarm, alarm.session_id, false))
        return false;
    state = value->valuestring;
    return true;
}
bool MatchesAck(const cJSON* root, const Alarm& alarm, const std::string& session) {
    return Keys(root, {"type", "action", "session_id", "lease_id", "playback_id", "timer_id",
                       "timer_revision", "attempt", "status"}) &&
           Text(Field(root, "type"), "timer") && Text(Field(root, "action"), "drain_ack") &&
           Text(Field(root, "status"), "accepted") && Identity(root, alarm, session, true);
}
std::string RecordJson(const Record& record) {
    auto root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "outcome", OutcomeText(record.outcome));
    auto alarm = cJSON_AddObjectToObject(root, "alarm");
    cJSON_AddStringToObject(alarm, "type", "timer");
    cJSON_AddStringToObject(alarm, "action", "alarm");
    AddIdentity(alarm, record.alarm, record.alarm.session_id);
    cJSON_AddStringToObject(alarm, "label", record.alarm.label.c_str());
    cJSON_AddStringToObject(alarm, "audio_sha256", record.alarm.audio_sha256.c_str());
    cJSON_AddNumberToObject(alarm, "packet_count", record.alarm.packet_count);
    cJSON_AddNumberToObject(alarm, "spoken_number", record.alarm.spoken_number);
    return Print(root);
}
bool ParseRecord(const std::string& text, Record& record) {
    if (!WithinJsonBudget(text) || text.size() > 2048 || text.find('\0') != std::string::npos ||
        text.find("\\u0000") != std::string::npos)
        return false;
    const char* end = nullptr;
    auto root = cJSON_ParseWithLengthOpts(text.c_str(), text.size() + 1, &end, true);
    Record parsed;
    bool valid = root && Keys(root, {"version", "alarm", "outcome"}) &&
                 cJSON_IsNumber(Field(root, "version")) &&
                 Field(root, "version")->valuedouble == 1 &&
                 ParseAlarm(Field(root, "alarm"), parsed.alarm);
    if (valid) {
        const auto outcome = Field(root, "outcome");
        if (Text(outcome, "unknown"))
            parsed.outcome = Outcome::Unknown;
        else if (Text(outcome, "completed"))
            parsed.outcome = Outcome::Completed;
        else if (Text(outcome, "failed"))
            parsed.outcome = Outcome::Failed;
        else if (Text(outcome, "interrupted"))
            parsed.outcome = Outcome::Interrupted;
        else
            valid = false;
    }
    cJSON_Delete(root);
    if (valid)
        record = std::move(parsed);
    return valid;
}

std::string DurableSlotJson(const DurableSlot& slot) {
    if (slot.state == DurableState::Empty && slot.lease_id.empty())
        return {};
    if (slot.state == DurableState::Alarm) {
        if (slot.record.alarm.lease_id != slot.lease_id)
            return {};
        return RecordJson(slot.record);
    }
    if ((slot.state != DurableState::Empty && slot.state != DurableState::Prepared &&
         slot.state != DurableState::NoStartPending) ||
        slot.lease_id.empty())
        return {};
    auto root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 2);
    cJSON_AddStringToObject(root, "state",
                            slot.state == DurableState::Empty      ? "empty"
                            : slot.state == DurableState::Prepared ? "prepared"
                                                                   : "no_start_pending");
    cJSON_AddStringToObject(root, "lease_id", slot.lease_id.c_str());
    return Print(root);
}

bool ParseDurableSlot(const std::string& text, DurableSlot& slot) {
    Record record;
    if (ParseRecord(text, record)) {
        slot = {DurableState::Alarm, record.alarm.lease_id, std::move(record)};
        return true;
    }
    if (!WithinJsonBudget(text) || text.size() > 2048 || text.find('\0') != std::string::npos ||
        text.find("\\u0000") != std::string::npos)
        return false;
    const char* end = nullptr;
    auto root = cJSON_ParseWithLengthOpts(text.c_str(), text.size() + 1, &end, true);
    DurableSlot parsed;
    const auto version = root ? Field(root, "version") : nullptr;
    const auto state = root ? Field(root, "state") : nullptr;
    bool valid = root && Keys(root, {"version", "state", "lease_id"}) && cJSON_IsNumber(version) &&
                 version->valuedouble == 2 && Id(Field(root, "lease_id"), parsed.lease_id);
    if (valid && Text(state, "empty"))
        parsed.state = DurableState::Empty;
    else if (valid && Text(state, "prepared"))
        parsed.state = DurableState::Prepared;
    else if (valid && Text(state, "no_start_pending"))
        parsed.state = DurableState::NoStartPending;
    else
        valid = false;
    cJSON_Delete(root);
    if (valid)
        slot = std::move(parsed);
    return valid;
}

bool SameDurableSlot(const DurableSlot& a, const DurableSlot& b) {
    return a.state == b.state && DurableSlotJson(a) == DurableSlotJson(b);
}

bool ParsePreparationRequest(const cJSON* root, const std::string& transport_session,
                             std::string& lease_id) {
    const auto version = Field(root, "version");
    std::string parsed_session;
    std::string parsed_lease;
    if (!Keys(root, {"type", "action", "version", "session_id", "lease_id"}) ||
        !Text(Field(root, "type"), "timer") ||
        !Text(Field(root, "action"), "prepare_alarm") || !cJSON_IsNumber(version) ||
        version->valuedouble != 1 || !Id(Field(root, "session_id"), parsed_session) ||
        parsed_session != transport_session || !Id(Field(root, "lease_id"), parsed_lease))
        return false;
    lease_id = std::move(parsed_lease);
    return true;
}

bool MatchesRecoveryRequest(const cJSON* root, const char* action, const std::string& lease_id,
                            const std::string& transport_session) {
    const auto version = Field(root, "version");
    return Keys(root, {"type", "action", "version", "session_id", "lease_id"}) &&
           Text(Field(root, "type"), "timer") && Text(Field(root, "action"), action) &&
           cJSON_IsNumber(version) && version->valuedouble == 1 &&
           Text(Field(root, "session_id"), transport_session.c_str()) &&
           Text(Field(root, "lease_id"), lease_id.c_str());
}

bool MatchesNoStartAck(const cJSON* root, const std::string& lease_id,
                       const std::string& transport_session) {
    const auto version = Field(root, "version");
    return Keys(root, {"type", "action", "version", "session_id", "lease_id", "status"}) &&
           Text(Field(root, "type"), "timer") && Text(Field(root, "action"), "no_start_ack") &&
           cJSON_IsNumber(version) && version->valuedouble == 1 &&
           Text(Field(root, "session_id"), transport_session.c_str()) &&
           Text(Field(root, "lease_id"), lease_id.c_str()) &&
           Text(Field(root, "status"), "accepted");
}

std::string RecoveryProofJson(DurableState state, const std::string& lease_id,
                              const std::string& transport_session) {
    if (state != DurableState::Prepared && state != DurableState::NoStartPending)
        return {};
    auto root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "timer");
    cJSON_AddStringToObject(root, "action",
                            state == DurableState::Prepared ? "prepared" : "no_start");
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "session_id", transport_session.c_str());
    cJSON_AddStringToObject(root, "lease_id", lease_id.c_str());
    return Print(root);
}

std::string ReceiptJson(const Record& record, const std::string& session) {
    if (record.outcome == Outcome::Unknown)
        return {};
    auto root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "timer");
    cJSON_AddStringToObject(root, "action",
                            session == record.alarm.session_id ? "drain" : "reconcile_drain");
    AddIdentity(root, record.alarm, session);
    cJSON_AddStringToObject(root, "outcome", OutcomeText(record.outcome));
    cJSON_AddBoolToObject(root, "output_drained", true);
    return Print(root);
}
bool IsSixtyMsOpus(const std::vector<uint8_t>& packet) {
    if (packet.empty() || packet.size() > 2048)
        return false;
    const unsigned toc = packet[0], config = toc >> 3, code = toc & 3;
    unsigned frames = code == 0 ? 1 : 2;
    if (code == 3) {
        if (packet.size() < 2)
            return false;
        frames = packet[1] & 63;
    }
    const unsigned units = config >= 16        ? (1u << (config & 3))
                           : config >= 12      ? (4u << (config & 1))
                           : (config & 3) == 3 ? 24
                                               : (4u << (config & 3));
    return frames * units == 24;
}

std::string DisplayText(const Snapshot& snapshot, int64_t trusted_now_ms) {
    std::vector<const Timer*> visible;
    for (const auto& timer : snapshot.timers)
        if (timer.state == State::Active || timer.state == State::Expired)
            visible.push_back(&timer);
    std::sort(visible.begin(), visible.end(), [](const Timer* a, const Timer* b) {
        return a->deadline_ms != b->deadline_ms ? a->deadline_ms < b->deadline_ms
                                                : a->spoken_number < b->spoken_number;
    });
    std::string text;
    for (size_t i = 0; i < std::min<size_t>(visible.size(), 2); ++i) {
        const auto& timer = *visible[i];
        char status[48];
        if (timer.state == State::Expired)
            std::snprintf(status, sizeof(status), "%u • Due", unsigned(timer.spoken_number));
        else if (trusted_now_ms <= 0)
            std::snprintf(status, sizeof(status), "%u • Syncing", unsigned(timer.spoken_number));
        else {
            const auto seconds =
                std::max<int64_t>(0, (timer.deadline_ms - trusted_now_ms + 999) / 1000);
            std::snprintf(status, sizeof(status), "%u • %lld:%02lld", unsigned(timer.spoken_number),
                          static_cast<long long>(seconds / 60),
                          static_cast<long long>(seconds % 60));
        }
        if (!text.empty())
            text += '\n';
        text += status;
        text += " ";
        // Do not split a UTF-8 scalar when shortening the label for the round face.
        size_t length = std::min<size_t>(timer.label.size(), 32);
        while (length < timer.label.size() &&
               (static_cast<unsigned char>(timer.label[length]) & 0xc0) == 0x80)
            --length;
        text += timer.label.substr(0, length);
        if (length < timer.label.size())
            text += "…";
    }
    if (visible.size() > 2)
        text += "\n+" + std::to_string(visible.size() - 2) + " timers";
    return text;
}
}  // namespace provisions::timers
