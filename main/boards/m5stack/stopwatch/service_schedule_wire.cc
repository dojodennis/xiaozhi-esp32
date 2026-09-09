#include "service_schedule_wire.h"

#include <cJSON.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

namespace orbit::service_schedule::wire {
namespace {
bool Space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool Digit(char c) { return c >= '0' && c <= '9'; }

// cJSON accepts some non-JSON number spellings and loses their original lexical
// form. This bounded scan rejects them, NUL and excessive nesting before allocation.
bool RawBudget(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kMaximumFrameBytes)
        return false;
    const auto first = bytes.find_first_not_of(" \t\r\n");
    const auto last = bytes.find_last_not_of(" \t\r\n");
    if (first == std::string_view::npos || bytes[first] != '{' || bytes[last] != '}')
        return false;
    size_t depth = 0;
    bool quoted = false;
    for (size_t i = 0; i < bytes.size(); ++i) {
        const char c = bytes[i];
        if (c == '\0')
            return false;
        if (quoted) {
            if (static_cast<unsigned char>(c) < 0x20)
                return false;
            if (c == '"') {
                quoted = false;
            } else if (c == '\\') {
                if (++i == bytes.size())
                    return false;
                const char escaped = bytes[i];
                if (escaped == 'u') {
                    if (bytes.size() - i < 5 || bytes.substr(i + 1, 4) == "0000")
                        return false;
                    for (size_t n = 1; n <= 4; ++n) {
                        const auto hex = bytes[i + n];
                        if (!(Digit(hex) || (hex >= 'a' && hex <= 'f') ||
                              (hex >= 'A' && hex <= 'F')))
                            return false;
                    }
                    i += 4;
                } else if (escaped != '"' && escaped != '\\' && escaped != '/' && escaped != 'b' &&
                           escaped != 'f' && escaped != 'n' && escaped != 'r' && escaped != 't') {
                    return false;
                }
            }
            continue;
        }
        if (c == '"') {
            quoted = true;
        } else if (c == '{' || c == '[') {
            if (++depth > kMaximumJsonDepth)
                return false;
        } else if (c == '}' || c == ']') {
            if (depth == 0)
                return false;
            --depth;
        } else if (c == '-' || Digit(c)) {
            size_t end = i;
            if (bytes[end] == '-') {
                if (++end == bytes.size() || !Digit(bytes[end]))
                    return false;
            }
            if (bytes[end] == '0') {
                ++end;
            } else {
                while (end < bytes.size() && Digit(bytes[end]))
                    ++end;
            }
            if (end < bytes.size() && !Space(bytes[end]) && bytes[end] != ',' &&
                bytes[end] != ']' && bytes[end] != '}')
                return false;
            i = end - 1;
        } else if (c == '+' || c == '.') {
            return false;
        } else if (static_cast<unsigned char>(c) < 0x20 && !Space(c)) {
            return false;
        }
    }
    return !quoted && depth == 0;
}

bool Keys(const cJSON* object, std::initializer_list<std::string_view> expected) {
    if (!cJSON_IsObject(object) || cJSON_GetArraySize(object) != static_cast<int>(expected.size()))
        return false;
    for (const auto name : expected) {
        size_t matches = 0;
        const cJSON* item = nullptr;
        cJSON_ArrayForEach (item, object)
            if (item->string != nullptr && name == item->string)
                ++matches;
        if (matches != 1)
            return false;
    }
    return true;
}
const cJSON* Field(const cJSON* object, const char* name) {
    return cJSON_GetObjectItemCaseSensitive(object, name);
}
bool IsText(const cJSON* item, std::string_view expected) {
    return cJSON_IsString(item) && item->valuestring != nullptr && item->valuestring == expected;
}
bool Uuid(std::string_view text) {
    if (text.size() != 36)
        return false;
    bool nonzero = false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-')
                return false;
        } else {
            if (!Digit(text[i]) && !(text[i] >= 'a' && text[i] <= 'f'))
                return false;
            nonzero = nonzero || text[i] != '0';
        }
    }
    return nonzero;
}
bool ParseUuid(const cJSON* item, std::string& out) {
    if (!cJSON_IsString(item) || item->valuestring == nullptr || !Uuid(item->valuestring))
        return false;
    out = item->valuestring;
    return true;
}
bool Integer(const cJSON* item, int64_t minimum, int64_t maximum, int64_t& out) {
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        item->valuedouble < static_cast<double>(minimum) ||
        item->valuedouble > static_cast<double>(maximum) ||
        std::trunc(item->valuedouble) != item->valuedouble)
        return false;
    out = static_cast<int64_t>(item->valuedouble);
    return true;
}
bool Revision(const cJSON* item, uint64_t& out) {
    int64_t value = 0;
    if (!Integer(item, 1, kMaximumRevision, value))
        return false;
    out = static_cast<uint64_t>(value);
    return true;
}
bool Label(const cJSON* item, const Validators& validators, std::string& out) {
    if (!cJSON_IsString(item) || item->valuestring == nullptr)
        return false;
    const std::string_view text(item->valuestring);
    if (text.empty() || text.size() > 320 || text.front() == ' ' || text.back() == ' ')
        return false;
    size_t count = 0;
    for (size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i++]);
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
        } else if (lead >= 0x80) {
            return false;
        }
        if (text.size() - i < tail)
            return false;
        while (tail--) {
            const auto byte = static_cast<unsigned char>(text[i++]);
            if ((byte & 0xc0) != 0x80)
                return false;
            cp = (cp << 6) | (byte & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || cp < 32 ||
            (cp >= 0x7f && cp <= 0x9f) || ++count > 80)
            return false;
    }
    if (!validators.label(text))
        return false;
    out = text;
    return true;
}
bool Timezone(const cJSON* item, const Validators& validators, std::string& out) {
    if (!cJSON_IsString(item) || item->valuestring == nullptr)
        return false;
    const std::string_view text(item->valuestring);
    if (text.empty() || text.size() > 64)
        return false;
    for (const char c : text)
        if (!Digit(c) && !(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z') && c != '/' &&
            c != '_' && c != '-' && c != '+')
            return false;
    if (!validators.timezone(text))
        return false;
    out = text;
    return true;
}
bool CueValue(const cJSON* item, const Validators& validators, const Snapshot& snapshot, Cue& cue) {
    const bool linked = IsText(Field(item, "kind"), "service_offset");
    if (linked) {
        if (!Keys(item, {"id", "revision", "label", "kind", "deadline_ms", "offset_ms"}))
            return false;
        cue.kind = CueKind::ServiceOffset;
        if (!Integer(Field(item, "offset_ms"), -kMaximumOffsetMs, kMaximumOffsetMs, cue.offset_ms))
            return false;
    } else if (!IsText(Field(item, "kind"), "fixed") ||
               !Keys(item, {"id", "revision", "label", "kind", "deadline_ms"})) {
        return false;
    }
    if (!ParseUuid(Field(item, "id"), cue.id) || !Revision(Field(item, "revision"), cue.revision) ||
        !Label(Field(item, "label"), validators, cue.label) ||
        !Integer(Field(item, "deadline_ms"), 1, kMaximumEpochMs, cue.deadline_ms))
        return false;
    return !linked || cue.deadline_ms == snapshot.service_at_ms + cue.offset_ms;
}
bool TimerValue(const cJSON* item, const Validators& validators, Timer& timer) {
    return Keys(item, {"id", "revision", "label", "deadline_ms"}) &&
           ParseUuid(Field(item, "id"), timer.id) &&
           Revision(Field(item, "revision"), timer.revision) &&
           Label(Field(item, "label"), validators, timer.label) &&
           Integer(Field(item, "deadline_ms"), 1, kMaximumEpochMs, timer.deadline_ms);
}
}  // namespace

Result Decode(std::string_view bytes, const Context& context, const Validators& validators,
              Snapshot& output) {
    if (validators.label == nullptr || validators.timezone == nullptr)
        return Result::MissingValidators;
    if (!RawBudget(bytes))
        return Result::Malformed;
    // Termination supports cJSON builds which inspect a byte at the parsed end.
    const std::string terminated(bytes);
    const char* end = nullptr;
    const std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
        cJSON_ParseWithLengthOpts(terminated.c_str(), terminated.size() + 1, &end, false),
        cJSON_Delete);
    if (!root || end == nullptr)
        return Result::Malformed;
    while (end < terminated.data() + terminated.size() && Space(*end))
        ++end;
    if (end != terminated.data() + terminated.size() ||
        !Keys(root.get(), {"type", "session_id", "state", "service_schedule"}) ||
        !IsText(Field(root.get(), "type"), "provisions") ||
        !IsText(Field(root.get(), "state"), "service_schedule_snapshot"))
        return Result::Malformed;
    std::string session;
    if (!ParseUuid(Field(root.get(), "session_id"), session))
        return Result::Malformed;
    if (!Uuid(context.websocket_session_id) || session != context.websocket_session_id)
        return Result::SessionMismatch;
    const auto payload = Field(root.get(), "service_schedule");
    Snapshot candidate;
    int64_t version = 0;
    if (!Integer(Field(payload, "version"), 1, 2, version))
        return Result::Malformed;
    candidate.version = static_cast<int>(version);
    const bool v2 = version == 2;
    if ((!v2 && !Keys(payload, {"version", "assignment_id", "device_id", "service_occurrence_id",
                                "service_revision", "snapshot_revision", "service_at_ms",
                                "timezone", "server_now_ms", "cues", "timers"})) ||
        (v2 && !Keys(payload, {"version", "assignment_id", "device_id", "service_occurrence_id",
                               "service_revision", "snapshot_revision", "service_at_ms", "timezone",
                               "cues", "timers"})))
        return Result::Malformed;
    if (!ParseUuid(Field(payload, "assignment_id"), candidate.scope.assignment_id) ||
        !ParseUuid(Field(payload, "device_id"), candidate.scope.device_id) ||
        !Revision(Field(payload, "snapshot_revision"), candidate.snapshot_revision))
        return Result::Malformed;
    const bool absent = v2 && cJSON_IsNull(Field(payload, "service_occurrence_id"));
    if (absent) {
        int64_t revision = 0;
        if (!Integer(Field(payload, "service_revision"), 0, 0, revision) ||
            !Integer(Field(payload, "service_at_ms"), 0, 0, candidate.service_at_ms) ||
            !IsText(Field(payload, "timezone"), ""))
            return Result::Malformed;
        candidate.service_revision = 0;
    } else if (!ParseUuid(Field(payload, "service_occurrence_id"),
                          candidate.service_occurrence_id) ||
               !Revision(Field(payload, "service_revision"), candidate.service_revision) ||
               !Integer(Field(payload, "service_at_ms"), 1, kMaximumEpochMs,
                        candidate.service_at_ms) ||
               !Timezone(Field(payload, "timezone"), validators, candidate.timezone)) {
        return Result::Malformed;
    }
    if (!v2 &&
        !Integer(Field(payload, "server_now_ms"), 1, kMaximumEpochMs, candidate.server_now_ms))
        return Result::Malformed;
    if (!Uuid(context.enrolled_scope.assignment_id) || !Uuid(context.enrolled_scope.device_id) ||
        candidate.scope.assignment_id != context.enrolled_scope.assignment_id ||
        candidate.scope.device_id != context.enrolled_scope.device_id)
        return Result::ScopeMismatch;
    if ((!context.service_occurrence_id.empty() && !Uuid(context.service_occurrence_id)) ||
        candidate.service_occurrence_id != context.service_occurrence_id)
        return Result::OccurrenceMismatch;
    const auto cues = Field(payload, "cues");
    const auto timers = Field(payload, "timers");
    if (!cJSON_IsArray(cues) || !cJSON_IsArray(timers))
        return Result::Malformed;
    const auto cue_count = static_cast<size_t>(cJSON_GetArraySize(cues));
    const auto timer_count = static_cast<size_t>(cJSON_GetArraySize(timers));
    if ((absent && cue_count != 0) || cue_count > kMaximumItems ||
        timer_count > kMaximumItems - cue_count)
        return Result::Malformed;
    candidate.cues.reserve(cue_count);
    candidate.timers.reserve(timer_count);
    std::vector<std::string> ids;
    ids.reserve(cue_count + timer_count);
    auto unique = [&](const std::string& id) {
        if (std::find(ids.begin(), ids.end(), id) != ids.end())
            return false;
        ids.push_back(id);
        return true;
    };
    const cJSON* item = nullptr;
    cJSON_ArrayForEach (item, cues) {
        Cue cue;
        if (!CueValue(item, validators, candidate, cue) || !unique(cue.id))
            return Result::Malformed;
        candidate.cues.push_back(std::move(cue));
    }
    cJSON_ArrayForEach (item, timers) {
        Timer timer;
        if (!TimerValue(item, validators, timer) || !unique(timer.id))
            return Result::Malformed;
        candidate.timers.push_back(std::move(timer));
    }
    output = std::move(candidate);
    return Result::Accepted;
}

Result DecodeClock(std::string_view bytes, const Context& context, ClockResponse& output) {
    if (!RawBudget(bytes))
        return Result::Malformed;
    const std::string terminated(bytes);
    const char* end = nullptr;
    const std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
        cJSON_ParseWithLengthOpts(terminated.c_str(), terminated.size() + 1, &end, false),
        cJSON_Delete);
    if (!root || end == nullptr)
        return Result::Malformed;
    while (end < terminated.data() + terminated.size() && Space(*end))
        ++end;
    if (end != terminated.data() + terminated.size() ||
        !Keys(root.get(), {"type", "session_id", "state", "service_schedule_clock"}) ||
        !IsText(Field(root.get(), "type"), "provisions") ||
        !IsText(Field(root.get(), "state"), "service_schedule_clock"))
        return Result::Malformed;
    ClockResponse candidate;
    auto& request = candidate.request;
    const auto payload = Field(root.get(), "service_schedule_clock");
    int64_t version = 0;
    if (!ParseUuid(Field(root.get(), "session_id"), request.session_id) ||
        !Keys(payload, {"version", "assignment_id", "device_id", "request_id", "snapshot_revision",
                        "server_now_ms"}) ||
        !Integer(Field(payload, "version"), 1, 1, version) ||
        !ParseUuid(Field(payload, "assignment_id"), request.scope.assignment_id) ||
        !ParseUuid(Field(payload, "device_id"), request.scope.device_id) ||
        !ParseUuid(Field(payload, "request_id"), request.request_id) ||
        !Revision(Field(payload, "snapshot_revision"), request.snapshot_revision) ||
        !Integer(Field(payload, "server_now_ms"), 1, kMaximumEpochMs, candidate.server_now_ms))
        return Result::Malformed;
    if (!Uuid(context.websocket_session_id) || request.session_id != context.websocket_session_id)
        return Result::SessionMismatch;
    if (!Uuid(context.enrolled_scope.assignment_id) || !Uuid(context.enrolled_scope.device_id) ||
        request.scope.assignment_id != context.enrolled_scope.assignment_id ||
        request.scope.device_id != context.enrolled_scope.device_id)
        return Result::ScopeMismatch;
    output = std::move(candidate);
    return Result::Accepted;
}

bool EncodeClockRequest(const ClockRequest& request, std::string& output) {
    if (!Uuid(request.session_id) || !Uuid(request.request_id) ||
        !Uuid(request.scope.assignment_id) || !Uuid(request.scope.device_id) ||
        request.snapshot_revision == 0 || request.snapshot_revision > kMaximumRevision)
        return false;
    // Canonical UUIDs have no JSON metacharacters; all other bytes are literals.
    output = "{\"type\":\"provisions\",\"session_id\":\"" + request.session_id +
             "\",\"state\":\"service_schedule_clock_request\",\"service_schedule_clock\":{"
             "\"version\":1,\"assignment_id\":\"" +
             request.scope.assignment_id + "\",\"device_id\":\"" + request.scope.device_id +
             "\",\"request_id\":\"" + request.request_id +
             "\",\"snapshot_revision\":" + std::to_string(request.snapshot_revision) + "}}";
    return true;
}

}  // namespace orbit::service_schedule::wire
