#include "provisions_timer_snapshot.h"

#include <cJSON.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <string_view>

namespace ProvisionsTimerSnapshot {
namespace {

constexpr double kMaximumSafeJsonInteger = 9007199254740991.0;
constexpr std::size_t kMaximumLabelBytes = kMaximumLabelScalars * 4;

bool HasExactKeys(const cJSON* object, std::initializer_list<std::string_view> expected_keys) {
    if (!cJSON_IsObject(object) ||
        cJSON_GetArraySize(object) != static_cast<int>(expected_keys.size())) {
        return false;
    }
    for (const auto expected : expected_keys) {
        bool found = false;
        cJSON* item = nullptr;
        cJSON_ArrayForEach (item, object) {
            if (item->string != nullptr && expected == item->string) {
                if (found) {
                    return false;
                }
                found = true;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

bool ParseSafeInteger(const cJSON* item, int64_t minimum, int64_t& value) {
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        item->valuedouble < static_cast<double>(minimum) ||
        item->valuedouble > kMaximumSafeJsonInteger ||
        std::trunc(item->valuedouble) != item->valuedouble) {
        return false;
    }
    value = static_cast<int64_t>(item->valuedouble);
    return true;
}

bool IsHex(char character) {
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
           (character >= 'A' && character <= 'F');
}

char LowerAscii(char character) {
    return character >= 'A' && character <= 'F' ? static_cast<char>(character - 'A' + 'a')
                                                : character;
}

bool IsForbiddenLabelCodePoint(uint32_t code_point) {
    if (code_point <= 0x1f || (code_point >= 0x7f && code_point <= 0x9f)) {
        return true;
    }
    if (code_point == 0x00ad || (code_point >= 0x0600 && code_point <= 0x0605) ||
        code_point == 0x061c || code_point == 0x06dd || code_point == 0x070f ||
        (code_point >= 0x0890 && code_point <= 0x0891) || code_point == 0x08e2 ||
        code_point == 0x180e || (code_point >= 0x200b && code_point <= 0x200f) ||
        (code_point >= 0x202a && code_point <= 0x202e) ||
        (code_point >= 0x2060 && code_point <= 0x2064) ||
        (code_point >= 0x2066 && code_point <= 0x206f) || code_point == 0xfeff ||
        (code_point >= 0xfff9 && code_point <= 0xfffb) || code_point == 0x110bd ||
        code_point == 0x110cd || (code_point >= 0x13430 && code_point <= 0x1343f) ||
        (code_point >= 0x1bca0 && code_point <= 0x1bca3) ||
        (code_point >= 0x1d173 && code_point <= 0x1d17a) || code_point == 0xe0001 ||
        (code_point >= 0xe0020 && code_point <= 0xe007f)) {
        return true;
    }
    if ((code_point >= 0xe000 && code_point <= 0xf8ff) ||
        (code_point >= 0xf0000 && code_point <= 0xffffd) ||
        (code_point >= 0x100000 && code_point <= 0x10fffd)) {
        return true;
    }
    return (code_point >= 0xfdd0 && code_point <= 0xfdef) || (code_point & 0xffff) == 0xfffe ||
           (code_point & 0xffff) == 0xffff;
}

bool IsValidLabelText(std::string_view text) {
    if (text.empty() || text.size() > kMaximumLabelBytes) {
        return false;
    }

    std::size_t scalar_count = 0;
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<uint8_t>(text[index]);
        uint32_t code_point = 0;
        std::size_t sequence_length = 0;
        if (first <= 0x7f) {
            code_point = first;
            sequence_length = 1;
        } else if (first >= 0xc2 && first <= 0xdf) {
            code_point = first & 0x1f;
            sequence_length = 2;
        } else if (first >= 0xe0 && first <= 0xef) {
            code_point = first & 0x0f;
            sequence_length = 3;
        } else if (first >= 0xf0 && first <= 0xf4) {
            code_point = first & 0x07;
            sequence_length = 4;
        } else {
            return false;
        }

        if (index + sequence_length > text.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset < sequence_length; ++offset) {
            const auto continuation = static_cast<uint8_t>(text[index + offset]);
            if ((continuation & 0xc0) != 0x80) {
                return false;
            }
            code_point = (code_point << 6) | (continuation & 0x3f);
        }
        if ((sequence_length == 3 && first == 0xe0 &&
             static_cast<uint8_t>(text[index + 1]) < 0xa0) ||
            (sequence_length == 3 && first == 0xed &&
             static_cast<uint8_t>(text[index + 1]) > 0x9f) ||
            (sequence_length == 4 && first == 0xf0 &&
             static_cast<uint8_t>(text[index + 1]) < 0x90) ||
            (sequence_length == 4 && first == 0xf4 &&
             static_cast<uint8_t>(text[index + 1]) > 0x8f) ||
            code_point > 0x10ffff || (code_point >= 0xd800 && code_point <= 0xdfff) ||
            IsForbiddenLabelCodePoint(code_point)) {
            return false;
        }

        ++scalar_count;
        if (scalar_count > kMaximumLabelScalars) {
            return false;
        }
        index += sequence_length;
    }
    return true;
}

bool NormalizeUuid(const cJSON* item, std::string& normalized) {
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        return false;
    }
    std::string value(item->valuestring);
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' || value[18] != '-' ||
        value[23] != '-') {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            continue;
        }
        if (!IsHex(value[index])) {
            return false;
        }
        value[index] = LowerAscii(value[index]);
    }
    normalized = std::move(value);
    return true;
}

bool ParseLabel(const cJSON* item, std::string& label) {
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        return false;
    }
    std::string candidate(item->valuestring);
    if (!IsValidLabelText(candidate)) {
        return false;
    }
    label = std::move(candidate);
    return true;
}

bool ParseTimer(const cJSON* item, Timer& timer) {
    if (!HasExactKeys(item, {"id", "label", "deadline_ms", "status"})) {
        return false;
    }

    const cJSON* id = cJSON_GetObjectItemCaseSensitive(item, "id");
    const cJSON* label = cJSON_GetObjectItemCaseSensitive(item, "label");
    const cJSON* deadline = cJSON_GetObjectItemCaseSensitive(item, "deadline_ms");
    const cJSON* status = cJSON_GetObjectItemCaseSensitive(item, "status");
    if (!NormalizeUuid(id, timer.id) || !ParseLabel(label, timer.label) ||
        !ParseSafeInteger(deadline, 1, timer.deadline_ms) || !cJSON_IsString(status) ||
        status->valuestring == nullptr) {
        return false;
    }
    if (std::string_view(status->valuestring) == "active") {
        timer.status = TimerStatus::kActive;
    } else if (std::string_view(status->valuestring) == "attention") {
        timer.status = TimerStatus::kAttention;
    } else {
        return false;
    }
    return true;
}

bool ParseSnapshotFrame(const cJSON* frame, std::string_view websocket_session_id,
                        Snapshot& snapshot, bool& websocket_session_matches) {
    websocket_session_matches = false;
    if (!HasExactKeys(frame, {"type", "session_id", "state", "timer_snapshot"})) {
        return false;
    }

    const cJSON* type = cJSON_GetObjectItemCaseSensitive(frame, "type");
    const cJSON* websocket_session = cJSON_GetObjectItemCaseSensitive(frame, "session_id");
    const cJSON* state = cJSON_GetObjectItemCaseSensitive(frame, "state");
    const cJSON* payload = cJSON_GetObjectItemCaseSensitive(frame, "timer_snapshot");
    if (!cJSON_IsString(type) || type->valuestring == nullptr ||
        std::string_view(type->valuestring) != "provisions" || !cJSON_IsString(websocket_session) ||
        websocket_session->valuestring == nullptr || !cJSON_IsString(state) ||
        state->valuestring == nullptr || std::string_view(state->valuestring) != "timer_snapshot") {
        return false;
    }
    if (websocket_session_id != websocket_session->valuestring) {
        return true;
    }
    websocket_session_matches = true;

    if (!HasExactKeys(payload, {"version", "session_id", "revision", "server_now_ms", "timers"})) {
        return false;
    }
    const cJSON* version = cJSON_GetObjectItemCaseSensitive(payload, "version");
    const cJSON* galley_session = cJSON_GetObjectItemCaseSensitive(payload, "session_id");
    const cJSON* revision = cJSON_GetObjectItemCaseSensitive(payload, "revision");
    const cJSON* server_now = cJSON_GetObjectItemCaseSensitive(payload, "server_now_ms");
    const cJSON* timers = cJSON_GetObjectItemCaseSensitive(payload, "timers");

    int64_t parsed_version = 0;
    if (!ParseSafeInteger(version, kVersion, parsed_version) || parsed_version != kVersion ||
        !NormalizeUuid(galley_session, snapshot.session_id) ||
        !ParseSafeInteger(revision, 0, snapshot.revision) ||
        !ParseSafeInteger(server_now, 1, snapshot.server_now_ms) || !cJSON_IsArray(timers)) {
        return false;
    }

    const int timer_count = cJSON_GetArraySize(timers);
    if (timer_count < 0 || timer_count > static_cast<int>(kMaximumTimers)) {
        return false;
    }
    snapshot.timers.reserve(static_cast<std::size_t>(timer_count));
    cJSON* item = nullptr;
    cJSON_ArrayForEach (item, timers) {
        Timer timer;
        if (!ParseTimer(item, timer) ||
            std::any_of(snapshot.timers.begin(), snapshot.timers.end(),
                        [&timer](const Timer& existing) { return existing.id == timer.id; })) {
            return false;
        }
        snapshot.timers.push_back(std::move(timer));
    }
    return true;
}

bool ParseResetFrame(const cJSON* frame, std::string_view websocket_session_id,
                     bool& websocket_session_matches) {
    websocket_session_matches = false;
    if (!HasExactKeys(frame, {"type", "session_id", "state", "timer_snapshot_reset"})) {
        return false;
    }
    const cJSON* type = cJSON_GetObjectItemCaseSensitive(frame, "type");
    const cJSON* websocket_session = cJSON_GetObjectItemCaseSensitive(frame, "session_id");
    const cJSON* state = cJSON_GetObjectItemCaseSensitive(frame, "state");
    const cJSON* payload = cJSON_GetObjectItemCaseSensitive(frame, "timer_snapshot_reset");
    if (!cJSON_IsString(type) || type->valuestring == nullptr ||
        std::string_view(type->valuestring) != "provisions" || !cJSON_IsString(websocket_session) ||
        websocket_session->valuestring == nullptr || !cJSON_IsString(state) ||
        state->valuestring == nullptr ||
        std::string_view(state->valuestring) != "timer_snapshot_reset") {
        return false;
    }
    if (websocket_session_id != websocket_session->valuestring) {
        return true;
    }
    websocket_session_matches = true;
    if (!HasExactKeys(payload, {"version"})) {
        return false;
    }
    int64_t version = 0;
    return ParseSafeInteger(cJSON_GetObjectItemCaseSensitive(payload, "version"), kVersion,
                            version) &&
           version == kVersion;
}

}  // namespace

ApplyResult Gate::Apply(const cJSON* frame, std::string_view websocket_session_id,
                        Update& accepted_update) {
    const cJSON* state =
        cJSON_IsObject(frame) ? cJSON_GetObjectItemCaseSensitive(frame, "state") : nullptr;
    if (cJSON_IsString(state) && state->valuestring != nullptr &&
        std::string_view(state->valuestring) == "timer_snapshot_reset") {
        bool websocket_session_matches = false;
        if (!ParseResetFrame(frame, websocket_session_id, websocket_session_matches)) {
            return ApplyResult::kMalformed;
        }
        if (!websocket_session_matches) {
            return ApplyResult::kWebsocketSessionMismatch;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        has_snapshot_ = false;
        session_id_.clear();
        revision_ = 0;
        accepted_update = Update{.kind = Update::Kind::kReset};
        return ApplyResult::kAccepted;
    }

    Snapshot candidate;
    bool websocket_session_matches = false;
    if (!ParseSnapshotFrame(frame, websocket_session_id, candidate, websocket_session_matches)) {
        return ApplyResult::kMalformed;
    }
    if (!websocket_session_matches) {
        return ApplyResult::kWebsocketSessionMismatch;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (has_snapshot_ && candidate.session_id == session_id_ && candidate.revision <= revision_) {
        return ApplyResult::kStaleRevision;
    }
    has_snapshot_ = true;
    session_id_ = candidate.session_id;
    revision_ = candidate.revision;
    accepted_update = Update{.kind = Update::Kind::kSnapshot, .snapshot = std::move(candidate)};
    return ApplyResult::kAccepted;
}

}  // namespace ProvisionsTimerSnapshot
