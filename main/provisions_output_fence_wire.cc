#include "provisions_output_fence.h"

#include <cJSON.h>
#include <cmath>
#include <cstring>
#include <initializer_list>

namespace provisions::output_fence {
namespace {
const cJSON* Field(const cJSON* root, const char* name) {
    return cJSON_GetObjectItemCaseSensitive(root, name);
}
bool Keys(const cJSON* object, std::initializer_list<const char*> keys) {
    if (!cJSON_IsObject(object) || cJSON_GetArraySize(object) != static_cast<int>(keys.size()))
        return false;
    for (const char* key : keys) {
        unsigned count = 0;
        const cJSON* item;
        cJSON_ArrayForEach (item, object)
            if (item->string && std::strcmp(item->string, key) == 0)
                ++count;
        if (count != 1)
            return false;
    }
    return true;
}
bool Literal(const cJSON* value, const char* expected) {
    return cJSON_IsString(value) && std::strcmp(value->valuestring, expected) == 0;
}
bool Id(const cJSON* value, std::string& output) {
    if (!cJSON_IsString(value) || !ValidId(value->valuestring))
        return false;
    output = value->valuestring;
    return true;
}
bool Hex(std::string_view value, size_t length) {
    if (value.size() != length)
        return false;
    for (char c : value)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}
bool Number(const cJSON* value, uint64_t minimum, uint64_t maximum, uint64_t& out) {
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
        value->valuedouble < static_cast<double>(minimum) ||
        value->valuedouble > static_cast<double>(maximum) ||
        value->valuedouble != std::floor(value->valuedouble))
        return false;
    out = static_cast<uint64_t>(value->valuedouble);
    return true;
}
// All v1 messages are one flat object with integer numbers and bounded ASCII identities.
// Reject nested input before cJSON recursion, ambiguous numeric lexemes, and escaped NULs
// (cJSON strings otherwise hide suffixes after U+0000 from strlen/strcmp).
bool Budget(std::string_view text) {
    if (text.empty() || text.size() > 2048)
        return false;
    bool quoted = false;
    unsigned depth = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = text[i];
        if (c == 0)
            return false;
        if (quoted) {
            if (c < 32)
                return false;
            if (c == '\\') {
                if (++i >= text.size())
                    return false;
                if (text[i] == 'u' && text.substr(i + 1, 4) == "0000")
                    return false;
            } else if (c == '"')
                quoted = false;
        } else if (c == '"')
            quoted = true;
        else if (c == '{') {
            if (++depth > 1)
                return false;
        } else if (c == '}') {
            if (depth != 1)
                return false;
            --depth;
        } else if (c == '[' || c == ']' || c == '-' || c == '+' || c == '.' || c == 'e' ||
                   c == 'E') {
            // e occurs in true/false: those keywords are consumed below.
            return false;
        } else if (c == 't' || c == 'f' || c == 'n') {
            const std::string_view word = c == 't' ? "true" : c == 'f' ? "false" : "null";
            if (text.substr(i, word.size()) != word)
                return false;
            i += word.size() - 1;
        } else if (c >= '0' && c <= '9') {
            const size_t first = i;
            while (i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '9')
                ++i;
            if (i - first + 1 > 16 || (i != first && text[first] == '0'))
                return false;
        }
    }
    return !quoted && depth == 0;
}
bool Common(const cJSON* root, Identity& out) {
    const auto hash = Field(root, "checkpoint_sha256");
    uint64_t version;
    if (!Number(Field(root, "version"), 1, 1, version) ||
        !Id(Field(root, "device_id"), out.device_id) ||
        !Number(Field(root, "fence_epoch"), 1, kMaximumInteger, out.fence_epoch) ||
        !cJSON_IsString(hash) || !Hex(hash->valuestring, 64) ||
        !Id(Field(root, "lease_id"), out.lease_id) ||
        !Number(Field(root, "sequence"), 1, kMaximumInteger, out.sequence) ||
        !Id(Field(root, "playback_id"), out.playback_id))
        return false;
    out.checkpoint_sha256 = hash->valuestring;
    return true;
}
bool Response(const cJSON* root, Identity& out) {
    uint64_t revision;
    if (!Id(Field(root, "request_id"), out.request_id) ||
        !Id(Field(root, "route_epoch"), out.route_epoch) ||
        !Number(Field(root, "response_revision"), 1, 2147483647, revision))
        return false;
    out.response_revision = static_cast<uint32_t>(revision);
    return true;
}
}  // namespace
bool ValidId(std::string_view value) {
    if (value.size() != 36)
        return false;
    bool nonzero = false;
    for (size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-')
                return false;
        } else {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                return false;
            nonzero = nonzero || c != '0';
        }
    }
    return nonzero;
}
bool ValidIdentity(const Identity& id) {
    return ValidId(id.device_id) && ValidId(id.lease_id) && ValidId(id.playback_id) &&
           ValidId(id.request_id) && ValidId(id.route_epoch) && ValidId(id.device_connection_id) &&
           Hex(id.checkpoint_sha256, 64) && id.fence_epoch >= 1 &&
           id.fence_epoch <= kMaximumInteger && id.sequence >= 1 &&
           id.sequence <= kMaximumInteger && id.response_revision >= 1 &&
           id.response_revision <= 2147483647;
}
bool ValidReceipt(const PhoneReceipt& r) {
    return (!r.completed || r.has_started) && (r.has_started || r.started_at_ms == 0) &&
           r.started_at_ms <= kMaximumInteger && r.stopped_at_ms <= kMaximumInteger &&
           r.drained_at_ms <= kMaximumInteger &&
           (!r.has_started || r.started_at_ms <= r.stopped_at_ms) &&
           r.drained_at_ms >= r.stopped_at_ms && r.drained_at_ms - r.stopped_at_ms >= 500;
}
bool ParseMessage(std::string_view text, Message& output) {
    if (!Budget(text))
        return false;
    const std::string input(text);
    cJSON* root = cJSON_ParseWithLengthOpts(input.c_str(), input.size() + 1, nullptr, true);
    Message message;
    bool valid = Common(root, message.identity);
    if (valid && Literal(Field(root, "type"), "output_fence_v1.acquire")) {
        valid = Keys(root, {"type", "version", "device_id", "fence_epoch", "checkpoint_sha256",
                            "lease_id", "sequence", "playback_id", "request_id",
                            "response_revision", "route_epoch", "device_connection_id", "mode"}) &&
                Response(root, message.identity) &&
                Id(Field(root, "device_connection_id"), message.identity.device_connection_id) &&
                Literal(Field(root, "mode"), "phone_exclusive");
    } else if (valid && Literal(Field(root, "type"), "output_fence_v1.release")) {
        message.command = Command::Release;
        auto completed = Field(root, "completed"), started = Field(root, "started_at_ms");
        valid = Keys(root, {"type", "version", "device_id", "fence_epoch", "checkpoint_sha256",
                            "lease_id", "sequence", "playback_id", "request_id",
                            "response_revision", "route_epoch", "completed", "started_at_ms",
                            "stopped_at_ms", "drained_at_ms", "physical_state"}) &&
                Response(root, message.identity) && cJSON_IsBool(completed) &&
                Literal(Field(root, "physical_state"), "DRAINED_PENDING_COMMIT");
        message.receipt.completed = cJSON_IsTrue(completed);
        message.receipt.has_started = !cJSON_IsNull(started);
        valid = valid &&
                (!message.receipt.has_started ||
                 Number(started, 0, kMaximumInteger, message.receipt.started_at_ms)) &&
                Number(Field(root, "stopped_at_ms"), 0, kMaximumInteger,
                       message.receipt.stopped_at_ms) &&
                Number(Field(root, "drained_at_ms"), 0, kMaximumInteger,
                       message.receipt.drained_at_ms) &&
                ValidReceipt(message.receipt);
    } else if (valid && Literal(Field(root, "type"), "output_fence_v1.abort_unacquired")) {
        message.command = Command::AbortUnacquired;
        valid = Keys(root, {"type", "version", "device_id", "fence_epoch", "checkpoint_sha256",
                            "lease_id", "sequence", "playback_id", "request_id",
                            "response_revision", "route_epoch", "device_connection_id", "completed",
                            "started_at_ms", "stopped_at_ms", "drained_at_ms"}) &&
                Response(root, message.identity) &&
                Id(Field(root, "device_connection_id"), message.identity.device_connection_id) &&
                cJSON_IsFalse(Field(root, "completed")) &&
                cJSON_IsNull(Field(root, "started_at_ms")) &&
                Number(Field(root, "stopped_at_ms"), 0, kMaximumInteger,
                       message.receipt.stopped_at_ms) &&
                Number(Field(root, "drained_at_ms"), 0, kMaximumInteger,
                       message.receipt.drained_at_ms) &&
                ValidReceipt(message.receipt);
    } else if (valid && Literal(Field(root, "type"), "output_fence_v1.close_commit")) {
        message.command = Command::CloseCommit;
        valid = Keys(root, {"type", "version", "device_id", "fence_epoch", "checkpoint_sha256",
                            "lease_id", "sequence", "playback_id", "backend_commit_id"}) &&
                Id(Field(root, "backend_commit_id"), message.backend_commit_id);
    } else
        valid = false;
    cJSON_Delete(root);
    if (valid)
        output = std::move(message);
    return valid;
}
std::string ReplyJson(Result result, const Identity& id) {
    if (!ValidIdentity(id) || (result != Result::Acquired && result != Result::DrainPending &&
                               result != Result::Released && result != Result::AbortPending))
        return {};
    const char* action = result == Result::Acquired       ? "acquired"
                         : result == Result::DrainPending ? "drain_pending"
                         : result == Result::AbortPending ? "abort_pending"
                                                          : "released";
    std::string json = "{\"type\":\"output_fence_v1." + std::string(action) +
                       "\",\"version\":1,\"device_id\":\"" + id.device_id +
                       "\",\"fence_epoch\":" + std::to_string(id.fence_epoch) +
                       ",\"checkpoint_sha256\":\"" + id.checkpoint_sha256 + "\",\"lease_id\":\"" +
                       id.lease_id + "\",\"sequence\":" + std::to_string(id.sequence) +
                       ",\"playback_id\":\"" + id.playback_id + "\",";
    if (result == Result::Acquired)
        return json +
               "\"capture_closed\":true,\"prior_output_drained\":true,\"fallback_disabled\":true}";
    if (result == Result::AbortPending)
        return json +
               "\"acquire_rejected\":true,\"physical_drained\":true,\"physical_state\":\"DRAINED_"
               "PENDING_COMMIT\"}";
    if (result == Result::DrainPending)
        return json + "\"physical_drained\":true,\"physical_state\":\"DRAINED_PENDING_COMMIT\"}";
    return json +
           "\"physical_drained\":true,\"owner_cleared\":true,\"physical_state\":\"TERMINAL\"}";
}
}  // namespace provisions::output_fence
