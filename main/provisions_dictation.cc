#include "provisions_dictation.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string_view>
#include "provisions_timers.h"
#include "provisions_voice_wire.h"
namespace provisions::dictation {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
const cJSON* Field(const cJSON* value, const char* key) {
    return cJSON_GetObjectItemCaseSensitive(value, key);
}
bool Keys(const cJSON* value, std::initializer_list<std::string_view> keys) {
    if (!cJSON_IsObject(value) || cJSON_GetArraySize(value) != static_cast<int>(keys.size()))
        return false;
    for (const auto key : keys) {
        unsigned count = 0;
        for (auto* item = value->child; item; item = item->next)
            if (item->string && key == item->string)
                ++count;
        if (count != 1)
            return false;
    }
    return true;
}
bool Text(const cJSON* value, std::string_view expected) {
    return cJSON_IsString(value) && expected == value->valuestring;
}
bool Number(const cJSON* value, uint32_t max, uint32_t& out) {
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) || value->valuedouble < 0 ||
        value->valuedouble > max || std::floor(value->valuedouble) != value->valuedouble)
        return false;
    out = static_cast<uint32_t>(value->valuedouble);
    return true;
}
bool Id(const cJSON* value, VoiceId& out) {
    return cJSON_IsString(value) && ParseVoiceId(value->valuestring, out);
}
bool HasId(const VoiceId& id) {
    return std::any_of(id.begin(), id.end(), [](uint8_t b) { return b != 0; });
}
const char* Name(Action action) {
    switch (action) {
        case Action::Start:
            return "start";
        case Action::Stop:
            return "stop";
        case Action::Resume:
            return "resume";
        case Action::Receipt:
            return "receipt";
        default:
            return "none";
    }
}
const char* Name(State state) {
    switch (state) {
        case State::Open:
            return "open";
        case State::Stopped:
            return "stopped";
        case State::Reviewed:
            return "reviewed";
        default:
            return "empty";
    }
}
bool ParseAction(const cJSON* value, Action& out, bool none = false) {
    for (auto action : {Action::Start, Action::Stop, Action::Resume, Action::Receipt, Action::None})
        if ((none || action != Action::None) && Text(value, Name(action))) {
            out = action;
            return true;
        }
    return false;
}
bool ParseState(const cJSON* value, State& out, bool empty = false) {
    for (auto state : {State::Open, State::Stopped, State::Reviewed, State::Empty})
        if ((empty || state != State::Empty) && Text(value, Name(state))) {
            out = state;
            return true;
        }
    return false;
}
std::string Print(cJSON* value) {
    char* text = cJSON_PrintUnformatted(value);
    std::string result = text ? text : "";
    cJSON_free(text);
    return result;
}
cJSON* Payload(cJSON* root, const char* key, const Record& record) {
    auto* payload = cJSON_AddObjectToObject(root, key);
    if (!payload)
        return nullptr;
    if (record.pending == Action::Start) {
        cJSON_AddStringToObject(payload, "kind", "note");
        cJSON_AddStringToObject(payload, "time_zone", "UTC");
    }
    if (record.pending == Action::Stop || record.pending == Action::Resume)
        cJSON_AddNumberToObject(payload, "control_revision", record.pending_revision);
    if (record.pending == Action::Stop)
        cJSON_AddNumberToObject(payload, "expected_segments", record.frozen_count);
    return payload;
}
bool Valid(const Record& r) {
    if (static_cast<unsigned>(r.state) > static_cast<unsigned>(State::Reviewed) ||
        static_cast<unsigned>(r.pending) > static_cast<unsigned>(Action::Receipt))
        return false;
    if (!HasId(r.id) || !HasId(r.conversation_id) || r.count > 60 || r.frozen_count > 60 ||
        r.revision > 2147483647 || r.control_revision > 2147483647 ||
        r.pending_revision > 2147483647)
        return false;
    if (r.pending == Action::Start) {
        if (r.state != State::Empty || r.control_revision != 0 || r.revision != 0 || r.count != 0 ||
            r.authorized)
            return false;
    } else if (r.state == State::Empty || !r.control_revision || !r.revision || r.expires_ms <= 0)
        return false;
    if ((r.pending == Action::Stop || r.pending == Action::Resume) &&
        (!r.pending_revision || r.pending_revision != r.control_revision))
        return false;
    if (r.pending == Action::Stop && (r.state != State::Open || r.frozen_count != r.count))
        return false;
    if (r.pending == Action::Resume && r.state != State::Stopped)
        return false;
    if ((r.state == State::Stopped || r.state == State::Reviewed) && r.frozen_count != r.count)
        return false;
    if (r.authorized && (r.state != State::Open || r.stop_requested ||
                         (r.pending != Action::None && r.pending != Action::Receipt)))
        return false;
    for (uint32_t i = 0; i < r.count; ++i) {
        const auto& segment = r.segments[i];
        if (!HasId(segment.request_id) || segment.samples > 160000 ||
            (segment.terminal && !segment.samples))
            return false;
        for (uint32_t j = 0; j < i; ++j)
            if (segment.request_id == r.segments[j].request_id)
                return false;
    }
    return true;
}
void FreezeStop(Record& record) {
    if (record.stop_requested && record.pending == Action::None && record.state == State::Open &&
        record.control_revision < 2147483647) {
        record.pending = Action::Stop;
        record.pending_revision = record.control_revision;
        record.frozen_count = record.count;
    }
}
}  // namespace
bool ParseReply(const cJSON* value, const std::string& session, Reply& out) {
    out = {};
    const bool ack = Text(Field(value, "action"), "control_ack");
    if (!(ack ? Keys(value, {"type", "action", "session_id", "dictation_session_id",
                             "control_action", "control", "receipt"})
              : Keys(value, {"type", "action", "session_id", "dictation_session_id",
                             "control_action", "control"})) ||
        (!ack && !Text(Field(value, "action"), "control_pending")) ||
        !Text(Field(value, "type"), "dictation") || !Text(Field(value, "session_id"), session) ||
        !Id(Field(value, "dictation_session_id"), out.id) ||
        !ParseAction(Field(value, "control_action"), out.action))
        return false;
    const auto* control = Field(value, "control");
    switch (out.action) {
        case Action::Start:
            if (!Keys(control, {"kind", "time_zone"}) || !Text(Field(control, "kind"), "note") ||
                !Text(Field(control, "time_zone"), "UTC"))
                return false;
            break;
        case Action::Stop:
            if (!Keys(control, {"expected_segments", "control_revision"}) ||
                !Number(Field(control, "expected_segments"), 60, out.payload_count))
                return false;
            [[fallthrough]];
        case Action::Resume:
            if ((out.action == Action::Resume && !Keys(control, {"control_revision"})) ||
                !Number(Field(control, "control_revision"), 2147483647, out.payload_revision) ||
                !out.payload_revision)
                return false;
            break;
        case Action::Receipt:
            if (!Keys(control, {}))
                return false;
            break;
        default:
            return false;
    }
    out.acknowledged = ack;
    if (!ack)
        return true;
    const auto* receipt = Field(value, "receipt");
    if (!Keys(receipt,
              {"state", "revision", "control_revision", "expected_segments", "expires_at"}) ||
        !ParseState(Field(receipt, "state"), out.state) ||
        !Number(Field(receipt, "revision"), 2147483647, out.revision) || !out.revision ||
        !Number(Field(receipt, "control_revision"), 2147483647, out.control_revision) ||
        !out.control_revision ||
        !timers::ParseTimestamp(Field(receipt, "expires_at"), out.expires_ms) ||
        out.expires_ms <= 0)
        return false;
    if (out.state == State::Open
            ? !cJSON_IsNull(Field(receipt, "expected_segments"))
            : !Number(Field(receipt, "expected_segments"), 60, out.expected_count))
        return false;
    if ((out.action == Action::Stop || out.action == Action::Resume) &&
        (out.payload_revision == 2147483647 || out.control_revision != out.payload_revision + 1))
        return false;
    if (out.action == Action::Stop &&
        (out.state == State::Open || out.expected_count != out.payload_count))
        return false;
    if (out.action == Action::Resume && out.state != State::Open)
        return false;
    if (out.action == Action::Start && out.control_revision != 1)
        return false;
    return true;
}
std::string ControlJson(const Record& record, const std::string& session) {
    VoiceId session_id{};
    if (!Valid(record) || record.pending == Action::None ||
        !ParseVoiceId(session.c_str(), session_id))
        return {};
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root)
        return {};
    cJSON_AddStringToObject(root.get(), "type", "dictation");
    cJSON_AddStringToObject(root.get(), "action", Name(record.pending));
    cJSON_AddStringToObject(root.get(), "session_id", session.c_str());
    cJSON_AddStringToObject(root.get(), "dictation_session_id", VoiceIdText(record.id).c_str());
    if (!Payload(root.get(), "payload", record))
        return {};
    return Print(root.get());
}
std::string RecordJson(const Record& record) {
    if (!Valid(record))
        return {};
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root)
        return {};
    cJSON_AddNumberToObject(root.get(), "version", 1);
    cJSON_AddStringToObject(root.get(), "id", VoiceIdText(record.id).c_str());
    cJSON_AddStringToObject(root.get(), "conversation_id",
                            VoiceIdText(record.conversation_id).c_str());
    cJSON_AddStringToObject(root.get(), "state", Name(record.state));
    cJSON_AddStringToObject(root.get(), "pending", Name(record.pending));
    cJSON_AddNumberToObject(root.get(), "revision", record.revision);
    cJSON_AddNumberToObject(root.get(), "control_revision", record.control_revision);
    cJSON_AddNumberToObject(root.get(), "pending_revision", record.pending_revision);
    cJSON_AddNumberToObject(root.get(), "frozen_count", record.frozen_count);
    cJSON_AddNumberToObject(root.get(), "expires_ms", record.expires_ms);
    cJSON_AddBoolToObject(root.get(), "authorized", record.authorized);
    cJSON_AddBoolToObject(root.get(), "stop_requested", record.stop_requested);
    auto* segments = cJSON_AddArrayToObject(root.get(), "segments");
    if (!segments)
        return {};
    for (uint32_t i = 0; i < record.count; ++i) {
        auto* item = cJSON_CreateObject();
        if (!item)
            return {};
        cJSON_AddItemToArray(segments, item);
        cJSON_AddStringToObject(item, "request_id",
                                VoiceIdText(record.segments[i].request_id).c_str());
        cJSON_AddNumberToObject(item, "samples", record.segments[i].samples);
        cJSON_AddBoolToObject(item, "terminal", record.segments[i].terminal);
    }
    return Print(root.get());
}
bool ParseRecord(const std::string& text, Record& record) {
    record = {};
    if (text.size() > 8192 || text.find('\0') != std::string::npos ||
        !timers::WithinJsonBudget(text))
        return false;
    Json root(cJSON_ParseWithOpts(text.c_str(), nullptr, true), cJSON_Delete);
    auto* r = root.get();
    uint32_t version = 0;
    if (!Keys(r, {"version", "id", "conversation_id", "state", "pending", "revision",
                  "control_revision", "pending_revision", "frozen_count", "expires_ms",
                  "authorized", "stop_requested", "segments"}) ||
        !Number(Field(r, "version"), 1, version) || version != 1 ||
        !Id(Field(r, "id"), record.id) ||
        !Id(Field(r, "conversation_id"), record.conversation_id) ||
        !ParseState(Field(r, "state"), record.state, true) ||
        !ParseAction(Field(r, "pending"), record.pending, true) ||
        !Number(Field(r, "revision"), 2147483647, record.revision) ||
        !Number(Field(r, "control_revision"), 2147483647, record.control_revision) ||
        !Number(Field(r, "pending_revision"), 2147483647, record.pending_revision) ||
        !Number(Field(r, "frozen_count"), 60, record.frozen_count) ||
        !cJSON_IsBool(Field(r, "authorized")) || !cJSON_IsBool(Field(r, "stop_requested")))
        return false;
    const auto* expires = Field(r, "expires_ms");
    if (!cJSON_IsNumber(expires) || !std::isfinite(expires->valuedouble) ||
        expires->valuedouble < 0 || expires->valuedouble > 253402300799999.0 ||
        std::floor(expires->valuedouble) != expires->valuedouble)
        return false;
    record.expires_ms = static_cast<int64_t>(expires->valuedouble);
    record.authorized = cJSON_IsTrue(Field(r, "authorized"));
    record.stop_requested = cJSON_IsTrue(Field(r, "stop_requested"));
    const auto* segments = Field(r, "segments");
    if (!cJSON_IsArray(segments) || cJSON_GetArraySize(segments) > 60)
        return false;
    for (auto* item = segments->child; item; item = item->next) {
        auto& segment = record.segments[record.count++];
        if (!Keys(item, {"request_id", "samples", "terminal"}) ||
            !Id(Field(item, "request_id"), segment.request_id) ||
            !Number(Field(item, "samples"), 160000, segment.samples) ||
            !cJSON_IsBool(Field(item, "terminal")))
            return false;
        segment.terminal = cJSON_IsTrue(Field(item, "terminal"));
    }
    return Valid(record);
}
bool Journal::Initialize() {
    const auto result = store_.Load(record_);
    fault_ = result == Store::LoadResult::Fault;
    return !fault_;
}
bool Journal::Commit(const Record& next) {
    if (!Valid(next) || !store_.Save(next)) {
        fault_ = true;
        return false;
    }
    record_ = next;
    fault_ = false;
    return true;
}
bool Journal::Start(const VoiceId& id, const VoiceId& conversation) {
    if (fault_ || (record_.state != State::Empty && record_.state != State::Reviewed) ||
        record_.pending != Action::None)
        return false;
    for (uint32_t i = 0; i < record_.count; ++i)
        if (!record_.segments[i].terminal)
            return false;
    Record next;
    next.id = id;
    next.conversation_id = conversation;
    next.pending = Action::Start;
    return Commit(next);
}
bool CanRetireEmpty(const Record& record) {
    return Valid(record) && record.state != State::Empty && record.pending == Action::None &&
           !record.stop_requested && record.count == 0;
}
bool Journal::ReplaceEmpty(const VoiceId& previous, const VoiceId& id,
                           const VoiceId& conversation) {
    if (fault_ || record_.id != previous || id == previous ||
        conversation == record_.conversation_id || !CanRetireEmpty(record_))
        return false;
    Record next;
    next.id = id;
    next.conversation_id = conversation;
    next.pending = Action::Start;
    return Commit(next);
}
bool Journal::Stop() {
    if (fault_ || !HasId(record_.id) ||
        (record_.state == State::Stopped && record_.pending != Action::Resume) ||
        record_.state == State::Reviewed)
        return false;
    auto next = record_;
    next.authorized = false;
    next.stop_requested = true;
    FreezeStop(next);
    return Commit(next);
}
bool Journal::Resume() {
    if (fault_ || record_.state != State::Stopped || record_.pending != Action::None ||
        record_.control_revision == 2147483647 || record_.count >= 60)
        return false;
    auto next = record_;
    next.pending = Action::Resume;
    next.pending_revision = next.control_revision;
    next.stop_requested = false;
    next.authorized = false;
    return Commit(next);
}
bool Journal::RequestReceipt() {
    if (fault_ || record_.state == State::Empty || record_.pending != Action::None)
        return false;
    auto next = record_;
    next.pending = Action::Receipt;
    return Commit(next);
}
bool Journal::Apply(const Reply& reply) {
    if (fault_ || record_.pending == Action::None || record_.id != reply.id ||
        record_.pending != reply.action ||
        ((reply.action == Action::Stop || reply.action == Action::Resume) &&
         record_.pending_revision != reply.payload_revision) ||
        (reply.action == Action::Stop && record_.frozen_count != reply.payload_count) ||
        !reply.acknowledged || reply.revision < record_.revision ||
        reply.control_revision < record_.control_revision ||
        (reply.state != State::Open && reply.expected_count != record_.count))
        return false;
    auto next = record_;
    next.state = reply.state;
    next.revision = reply.revision;
    next.control_revision = reply.control_revision;
    next.expires_ms = reply.expires_ms;
    next.pending = Action::None;
    next.pending_revision = 0;
    if (reply.action == Action::Start || reply.action == Action::Resume)
        next.authorized = reply.state == State::Open && !next.stop_requested;
    if (reply.action == Action::Receipt && reply.control_revision != record_.control_revision)
        next.authorized = false;
    if (reply.state != State::Open) {
        next.authorized = false;
        next.stop_requested = false;
        next.frozen_count = reply.expected_count;
    }
    FreezeStop(next);
    return Commit(next);
}
bool Journal::CanCapture(const VoiceId& conversation, int64_t now_ms) const {
    return !fault_ && record_.authorized && record_.state == State::Open &&
           record_.pending == Action::None && !record_.stop_requested &&
           record_.conversation_id == conversation && now_ms > 0 && now_ms < record_.expires_ms &&
           record_.count < 60;
}
bool Journal::Reserve(const VoiceId& request) {
    if (fault_ || !record_.authorized || record_.pending != Action::None ||
        record_.stop_requested || record_.count >= 60 || !HasId(request))
        return false;
    auto next = record_;
    next.segments[next.count++] = {request, 0, false};
    return Commit(next);
}
bool Journal::AbandonEmpty(const VoiceId& request) {
    if (fault_ || !record_.count || record_.pending == Action::Stop ||
        record_.state == State::Stopped || record_.state == State::Reviewed)
        return false;
    const auto& last = record_.segments[record_.count - 1];
    if (last.request_id != request || last.samples != 0 || last.terminal)
        return false;
    auto next = record_;
    next.segments[--next.count] = {};
    return Commit(next);
}
bool Journal::Seal(const VoiceCapture& capture) {
    if (!capture.IsDictation() || capture.dictation_session_id != record_.id ||
        capture.conversation_id != record_.conversation_id ||
        capture.chunk_sequence >= record_.count || !capture.sample_count ||
        capture.sample_count > 160000)
        return false;
    const auto& segment = record_.segments[capture.chunk_sequence];
    if (segment.request_id != capture.request_id ||
        (segment.samples && segment.samples != capture.sample_count))
        return false;
    if (segment.samples == capture.sample_count)
        return true;
    auto next = record_;
    next.segments[capture.chunk_sequence].samples = capture.sample_count;
    return Commit(next);
}
bool Journal::Terminal(const VoiceCapture& capture) {
    if (!Seal(capture))
        return false;
    if (record_.segments[capture.chunk_sequence].terminal)
        return true;
    auto next = record_;
    next.segments[capture.chunk_sequence].terminal = true;
    return Commit(next);
}
}  // namespace provisions::dictation
