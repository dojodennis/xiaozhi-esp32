#include "provisions_voice_wire.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <string_view>

namespace provisions {
namespace {
bool Keys(const cJSON* object, std::initializer_list<std::string_view> keys) {
    if (!cJSON_IsObject(object) || cJSON_GetArraySize(object) != static_cast<int>(keys.size()))
        return false;
    for (auto key : keys) {
        size_t count = 0;
        const cJSON* item = nullptr;
        cJSON_ArrayForEach (item, object)
            if (item->string && key == item->string)
                ++count;
        if (count != 1)
            return false;
    }
    return true;
}
bool Integer(const cJSON* value, uint64_t maximum, uint64_t& output) {
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) || value->valuedouble < 0 ||
        value->valuedouble > static_cast<double>(maximum) ||
        std::floor(value->valuedouble) != value->valuedouble)
        return false;
    output = static_cast<uint64_t>(value->valuedouble);
    return true;
}
int Hex(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
bool Bytes(const char* text, size_t chars, uint8_t* bytes) {
    if (!text || std::strlen(text) != chars)
        return false;
    for (size_t i = 0; i < chars; i += 2) {
        const int a = Hex(text[i]), b = Hex(text[i + 1]);
        if (a < 0 || b < 0)
            return false;
        bytes[i / 2] = (a << 4) | b;
    }
    return true;
}
bool ContextFields(const cJSON* value, VoiceContext& context) {
    auto conversation = cJSON_GetObjectItemCaseSensitive(value, "conversation_id");
    auto source = cJSON_GetObjectItemCaseSensitive(value, "source_request_id");
    auto revision = cJSON_GetObjectItemCaseSensitive(value, "source_revision");
    if (!cJSON_IsString(conversation) ||
        !ParseVoiceId(conversation->valuestring, context.conversation_id))
        return false;
    uint64_t number = 0;
    if (cJSON_IsNull(source) && cJSON_IsNull(revision)) {
        context.source_request_id = {};
        context.source_revision = 0;
        return true;
    }
    if (!cJSON_IsString(source) || !ParseVoiceId(source->valuestring, context.source_request_id) ||
        !Integer(revision, 999999999, number))
        return false;
    context.source_revision = number;
    return true;
}
void AddContext(cJSON* object, const VoiceCapture& capture) {
    cJSON_AddStringToObject(object, "conversation_id",
                            VoiceIdText(capture.conversation_id).c_str());
    if (std::any_of(capture.source_request_id.begin(), capture.source_request_id.end(),
                    [](uint8_t b) { return b != 0; })) {
        cJSON_AddStringToObject(object, "source_request_id",
                                VoiceIdText(capture.source_request_id).c_str());
        cJSON_AddNumberToObject(object, "source_revision", capture.source_revision);
    } else {
        cJSON_AddNullToObject(object, "source_request_id");
        cJSON_AddNullToObject(object, "source_revision");
    }
}
}  // namespace
bool ParseVoiceId(const char* value, VoiceId& output) {
    output = {};
    if (!value || std::strlen(value) != 36)
        return false;
    char compact[33]{};
    size_t index = 0;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-')
                return false;
        } else {
            if (Hex(value[i]) < 0)
                return false;
            compact[index++] = value[i];
        }
    }
    return Bytes(compact, 32, output.data()) &&
           std::any_of(output.begin(), output.end(), [](uint8_t byte) { return byte != 0; });
}
std::string VoiceIdText(const VoiceId& id) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(36);
    for (size_t i = 0; i < id.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            result += '-';
        result += hex[id[i] >> 4];
        result += hex[id[i] & 15];
    }
    return result;
}
bool ParseVoiceContext(const cJSON* value, VoiceContext& output) {
    output = {};
    return Keys(value, {"conversation_id", "source_request_id", "source_revision"}) &&
           ContextFields(value, output) && VoiceRecording::ValidContext(output);
}
bool ParseVoiceReceipt(const cJSON* value, VoiceCaptureReceipt& output) {
    output = {};
    if (!Keys(value, {"session_id", "type", "request_id", "capture", "state", "durable"}))
        return false;
    auto type = cJSON_GetObjectItemCaseSensitive(value, "type");
    auto id = cJSON_GetObjectItemCaseSensitive(value, "request_id");
    auto state = cJSON_GetObjectItemCaseSensitive(value, "state");
    auto durable = cJSON_GetObjectItemCaseSensitive(value, "durable");
    auto capture = cJSON_GetObjectItemCaseSensitive(value, "capture");
    if (!cJSON_IsString(type) || std::strcmp(type->valuestring, "capture_receipt") != 0 ||
        !cJSON_IsString(id) || !ParseVoiceId(id->valuestring, output.capture.request_id) ||
        !cJSON_IsString(state) || !cJSON_IsBool(durable) ||
        !Keys(capture, {"conversation_id", "source_request_id", "source_revision", "audio_sha256",
                        "audio_bytes", "packet_count", "captured_unix_ms"}))
        return false;
    const std::string_view status(state->valuestring);
    const bool terminal = status == "transcribed" || status == "unintelligible";
    if (status != "processing" && status != "pending" && status != "needs_attention" && !terminal)
        return false;
    if (cJSON_IsTrue(durable) != terminal)
        return false;
    VoiceContext context;
    uint64_t count = 0, bytes = 0, captured = 0;
    auto digest = cJSON_GetObjectItemCaseSensitive(capture, "audio_sha256");
    if (!ContextFields(capture, context) || !cJSON_IsString(digest) ||
        !Bytes(digest->valuestring, 64, output.digest.data()) ||
        !Integer(cJSON_GetObjectItemCaseSensitive(capture, "packet_count"), 167, count) ||
        count == 0 ||
        !Integer(cJSON_GetObjectItemCaseSensitive(capture, "audio_bytes"),
                 VoiceOutbox::kMaxFrameBytes, bytes) ||
        bytes < count * 3 || bytes > count * 2050 ||
        !Integer(cJSON_GetObjectItemCaseSensitive(capture, "captured_unix_ms"), 253402300799999ULL,
                 captured))
        return false;
    output.capture.conversation_id = context.conversation_id;
    output.capture.source_request_id = context.source_request_id;
    output.capture.source_revision = context.source_revision;
    output.capture.packet_count = count;
    output.capture.captured_unix_ms = captured;
    output.bytes = bytes;
    output.durable = terminal;
    output.needs_attention = status == "needs_attention";
    return true;
}
std::string VoiceCaptureStart(const VoiceReplay& replay, const std::string& session, uint32_t turn,
                              bool deferred) {
    cJSON* root = cJSON_CreateObject();
    if (!root)
        return {};
    cJSON_AddStringToObject(root, "session_id", session.c_str());
    cJSON_AddStringToObject(root, "type", "listen");
    cJSON_AddStringToObject(root, "state", "start");
    cJSON_AddStringToObject(root, "mode", "manual");
    cJSON_AddNumberToObject(root, "turn_id", turn);
    cJSON_AddStringToObject(root, "request_id", VoiceIdText(replay.capture.request_id).c_str());
    cJSON_AddBoolToObject(root, "deferred", deferred);
    cJSON* capture = cJSON_AddObjectToObject(root, "capture");
    if (!capture) {
        cJSON_Delete(root);
        return {};
    }
    AddContext(capture, replay.capture);
    cJSON_AddNumberToObject(capture, "audio_bytes", replay.bytes);
    cJSON_AddNumberToObject(capture, "packet_count", replay.capture.packet_count);
    cJSON_AddNumberToObject(capture, "captured_unix_ms", replay.capture.captured_unix_ms);
    constexpr char hex[] = "0123456789abcdef";
    char digest[65]{};
    for (size_t i = 0; i < replay.digest.size(); ++i) {
        digest[2 * i] = hex[replay.digest[i] >> 4];
        digest[2 * i + 1] = hex[replay.digest[i] & 15];
    }
    cJSON_AddStringToObject(capture, "audio_sha256", digest);
    char* text = cJSON_PrintUnformatted(root);
    std::string result = text ? text : "";
    cJSON_free(text);
    cJSON_Delete(root);
    return result;
}
}  // namespace provisions
