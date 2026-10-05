#include "provisions_service_review.h"
#include <cJSON.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string_view>
#include "provisions_voice_wire.h"

namespace provisions::service {
namespace {
const cJSON* Field(const cJSON* root, const char* name) {
    return cJSON_GetObjectItemCaseSensitive(root, name);
}
bool Keys(const cJSON* root, std::initializer_list<std::string_view> required,
          std::initializer_list<std::string_view> optional = {}) {
    if (!cJSON_IsObject(root))
        return false;
    for (auto* item = root->child; item; item = item->next) {
        if (!item->string)
            return false;
        const std::string_view key(item->string);
        if (std::find(required.begin(), required.end(), key) == required.end() &&
            std::find(optional.begin(), optional.end(), key) == optional.end())
            return false;
        for (auto* other = item->next; other; other = other->next)
            if (other->string && key == other->string)
                return false;
    }
    for (const auto key : required)
        if (!Field(root, key.data()))
            return false;
    return true;
}
bool Text(const cJSON* value, std::string_view expected) {
    return cJSON_IsString(value) && value->valuestring && expected == value->valuestring;
}
bool Number(const cJSON* value, uint32_t maximum, uint32_t& out) {
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) || value->valuedouble < 0 ||
        value->valuedouble > maximum || std::floor(value->valuedouble) != value->valuedouble)
        return false;
    out = static_cast<uint32_t>(value->valuedouble);
    return true;
}
bool OptionalNumber(const cJSON* value, uint32_t maximum, int& out) {
    if (cJSON_IsNull(value)) {
        out = -1;
        return true;
    }
    uint32_t number;
    if (!Number(value, maximum, number))
        return false;
    out = static_cast<int>(number);
    return true;
}
bool Id(const cJSON* value, VoiceId& out, bool nullable = false) {
    if (nullable && cJSON_IsNull(value)) {
        out = {};
        return true;
    }
    return cJSON_IsString(value) && ParseVoiceId(value->valuestring, out);
}
// Validate UTF-8 as well as bounds; controls may otherwise hide guest labels.
bool BoundedText(const cJSON* value, size_t bytes, size_t chars, std::string& out,
                 bool empty = true) {
    if (!cJSON_IsString(value) || !value->valuestring)
        return false;
    const std::string text(value->valuestring);
    if ((!empty && text.empty()) || text.size() > bytes)
        return false;
    size_t count = 0;
    for (size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if ((lead < 32 && lead != '\n' && lead != '\t') || lead == 127)
            return false;
        size_t width = lead < 128                     ? 1
                       : lead >= 0xC2 && lead <= 0xDF ? 2
                       : lead >= 0xE0 && lead <= 0xEF ? 3
                       : lead >= 0xF0 && lead <= 0xF4 ? 4
                                                      : 0;
        if (!width || i + width > text.size())
            return false;
        for (size_t j = 1; j < width; ++j)
            if ((static_cast<unsigned char>(text[i + j]) & 0xC0) != 0x80)
                return false;
        if (width >= 3) {
            const auto second = static_cast<unsigned char>(text[i + 1]);
            if ((lead == 0xE0 && second < 0xA0) || (lead == 0xED && second >= 0xA0) ||
                (lead == 0xF0 && second < 0x90) || (lead == 0xF4 && second >= 0x90))
                return false;
        }
        i += width;
        ++count;
    }
    if (count > chars)
        return false;
    out = text;
    return true;
}
std::string Print(cJSON* root) {
    char* raw = cJSON_PrintUnformatted(root);
    std::string result = raw ? raw : "";
    cJSON_free(raw);
    return result;
}
bool SameRoute(const Route& a, const Route& b) {
    return a.binding_id == b.binding_id && a.recording_id == b.recording_id &&
           a.segment_id == b.segment_id && a.request_id == b.request_id &&
           a.revision == b.revision && a.text == b.text && a.guest == b.guest &&
           (!a.guest ||
            (a.alias.visit_id == b.alias.visit_id && a.alias.profile_id == b.alias.profile_id &&
             a.alias.guest_number == b.alias.guest_number));
}
}  // namespace

bool ParseSnapshot(const cJSON* root, const std::string& session, Snapshot& out) {
    if (!Keys(root,
              {"type",
               "state",
               "session_id",
               "binding_id",
               "recording_id",
               "revision",
               "recording_state",
               "complete",
               "expected_segments",
               "received_segments",
               "segment_id",
               "sequence",
               "text_offset",
               "alias_offset",
               "text",
               "full_text_length",
               "next_text_offset",
               "next_sequence",
               "segment_saved_at",
               "aliases",
               "next_alias_offset",
               "requires_desk_review"},
              {"delivery_elapsed_ms", "review_elapsed_ms", "upload_to_preview_ms"}) ||
        !Text(Field(root, "type"), "dojo_service") || !Text(Field(root, "state"), "transcript") ||
        session.empty() || !Text(Field(root, "session_id"), session))
        return false;
    Snapshot s;
    if (!Id(Field(root, "binding_id"), s.binding_id) ||
        !Id(Field(root, "recording_id"), s.recording_id) ||
        !Id(Field(root, "segment_id"), s.segment_id, true) ||
        !Number(Field(root, "revision"), 2147483647, s.revision) ||
        !Number(Field(root, "sequence"), 59, s.cursor.sequence) ||
        !Number(Field(root, "text_offset"), 100000, s.cursor.text_offset) ||
        !Number(Field(root, "alias_offset"), 100000, s.cursor.alias_offset) ||
        !Number(Field(root, "full_text_length"), 100000, s.full_text_length) ||
        !Number(Field(root, "received_segments"), 60, s.received_segments) ||
        !OptionalNumber(Field(root, "expected_segments"), 60, s.expected_segments) ||
        !OptionalNumber(Field(root, "next_text_offset"), 100000, s.next_text_offset) ||
        !OptionalNumber(Field(root, "next_sequence"), 59, s.next_sequence) ||
        !OptionalNumber(Field(root, "next_alias_offset"), 100000, s.next_alias_offset) ||
        !cJSON_IsBool(Field(root, "complete")) ||
        !cJSON_IsBool(Field(root, "requires_desk_review")) ||
        !BoundedText(Field(root, "text"), kTextBytes, 500, s.text))
        return false;
    s.complete = cJSON_IsTrue(Field(root, "complete"));
    s.requires_desk_review = cJSON_IsTrue(Field(root, "requires_desk_review"));
    s.stopped = Text(Field(root, "recording_state"), "stopped");
    if (!s.stopped && !Text(Field(root, "recording_state"), "open"))
        return false;
    if (s.complete && (!s.stopped || s.expected_segments < 0 ||
                       s.received_segments != static_cast<uint32_t>(s.expected_segments)))
        return false;
    const size_t chars = CharacterCount(s.text);
    if (s.cursor.text_offset > s.full_text_length ||
        chars > s.full_text_length - s.cursor.text_offset ||
        (s.next_text_offset >= 0 &&
         static_cast<uint32_t>(s.next_text_offset) != s.cursor.text_offset + chars) ||
        (s.next_sequence >= 0 && static_cast<uint32_t>(s.next_sequence) <= s.cursor.sequence) ||
        (s.next_alias_offset >= 0 &&
         static_cast<uint32_t>(s.next_alias_offset) <= s.cursor.alias_offset) ||
        (s.segment_id == VoiceId{} && (!s.text.empty() || s.full_text_length)))
        return false;
    auto saved = Field(root, "segment_saved_at");
    std::string ignored;
    if (!cJSON_IsNull(saved) && !BoundedText(saved, 40, 40, ignored, false))
        return false;
    auto aliases = Field(root, "aliases");
    if (!cJSON_IsArray(aliases) || cJSON_GetArraySize(aliases) > static_cast<int>(kAliasPage))
        return false;
    for (auto* entry = aliases->child; entry; entry = entry->next) {
        Alias a;
        if (!Keys(entry, {"visit_id", "visit_label", "guest_number", "profile_id", "label"}) ||
            !Id(Field(entry, "visit_id"), a.visit_id) ||
            !Id(Field(entry, "profile_id"), a.profile_id) ||
            !Number(Field(entry, "guest_number"), 1200, a.guest_number) || !a.guest_number ||
            !BoundedText(Field(entry, "label"), kLabelBytes, kLabelBytes, a.label, false) ||
            !BoundedText(Field(entry, "visit_label"), kLabelBytes, kLabelBytes, ignored, false))
            return false;
        for (const auto& other : s.aliases)
            if (a.visit_id == other.visit_id && a.guest_number == other.guest_number)
                return false;
        s.aliases.push_back(std::move(a));
    }
    for (const auto* key : {"delivery_elapsed_ms", "review_elapsed_ms", "upload_to_preview_ms"}) {
        if (const auto elapsed = Field(root, key)) {
            uint32_t duration;
            if (!Number(elapsed, 2147483647, duration))
                return false;
        }
    }
    out = std::move(s);
    return true;
}

bool ParseSaved(const cJSON* root, const std::string& session, Route& out) {
    const bool guest = Text(Field(root, "target"), "guest");
    if (!Keys(root,
              {"type", "state", "session_id", "binding_id", "recording_id", "segment_id",
               "request_id", "expected_revision", "target", "text", "saved_at", "replayed"},
              guest ? std::initializer_list<std::string_view>{"visit_id", "guest_number",
                                                              "profile_id"}
                    : std::initializer_list<std::string_view>{}) ||
        !Text(Field(root, "type"), "dojo_service") || !Text(Field(root, "state"), "note_saved") ||
        session.empty() || !Text(Field(root, "session_id"), session))
        return false;
    Route route;
    route.guest = guest;
    std::string saved;
    if ((!guest && !Text(Field(root, "target"), "general")) ||
        !Id(Field(root, "binding_id"), route.binding_id) ||
        !Id(Field(root, "recording_id"), route.recording_id) ||
        !Id(Field(root, "segment_id"), route.segment_id) ||
        !Id(Field(root, "request_id"), route.request_id) ||
        !Number(Field(root, "expected_revision"), 2147483647, route.revision) ||
        !BoundedText(Field(root, "text"), kTextBytes, 500, route.text, false) ||
        !BoundedText(Field(root, "saved_at"), 40, 40, saved, false) ||
        !cJSON_IsBool(Field(root, "replayed")))
        return false;
    if (guest && (!Id(Field(root, "visit_id"), route.alias.visit_id) ||
                  !Id(Field(root, "profile_id"), route.alias.profile_id) ||
                  !Number(Field(root, "guest_number"), 1200, route.alias.guest_number) ||
                  !route.alias.guest_number))
        return false;
    out = std::move(route);
    return true;
}

std::string ReviewJson(const std::string& session, const VoiceId& recording, Cursor cursor) {
    if (session.empty() || recording == VoiceId{} || cursor.sequence > 59 ||
        cursor.text_offset > 100000 || cursor.alias_offset > 100000)
        return "";
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddStringToObject(root.get(), "type", "dojo_service");
    cJSON_AddStringToObject(root.get(), "action", "review");
    cJSON_AddStringToObject(root.get(), "session_id", session.c_str());
    cJSON_AddStringToObject(root.get(), "recording_id", VoiceIdText(recording).c_str());
    cJSON_AddNumberToObject(root.get(), "sequence", cursor.sequence);
    cJSON_AddNumberToObject(root.get(), "text_offset", cursor.text_offset);
    cJSON_AddNumberToObject(root.get(), "alias_offset", cursor.alias_offset);
    return Print(root.get());
}
std::string RouteJson(const std::string& session, const Route& route) {
    if (session.empty() || route.recording_id == VoiceId{} || route.segment_id == VoiceId{} ||
        route.request_id == VoiceId{} || route.text.empty() || route.text.size() > kTextBytes ||
        CharacterCount(route.text) > 500)
        return "";
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddStringToObject(root.get(), "type", "dojo_service");
    cJSON_AddStringToObject(root.get(), "action", "route_note");
    cJSON_AddStringToObject(root.get(), "session_id", session.c_str());
    cJSON_AddStringToObject(root.get(), "recording_id", VoiceIdText(route.recording_id).c_str());
    cJSON_AddStringToObject(root.get(), "segment_id", VoiceIdText(route.segment_id).c_str());
    cJSON_AddStringToObject(root.get(), "request_id", VoiceIdText(route.request_id).c_str());
    cJSON_AddNumberToObject(root.get(), "expected_revision", route.revision);
    cJSON_AddBoolToObject(root.get(), "confirmed", true);
    cJSON_AddStringToObject(root.get(), "text", route.text.c_str());
    cJSON_AddStringToObject(root.get(), "target", route.guest ? "guest" : "general");
    if (route.guest) {
        cJSON_AddStringToObject(root.get(), "visit_id", VoiceIdText(route.alias.visit_id).c_str());
        cJSON_AddStringToObject(root.get(), "profile_id",
                                VoiceIdText(route.alias.profile_id).c_str());
        cJSON_AddNumberToObject(root.get(), "guest_number", route.alias.guest_number);
    }
    return Print(root.get());
}
size_t CharacterCount(const std::string& text) {
    return std::count_if(text.begin(), text.end(),
                         [](unsigned char c) { return (c & 0xC0) != 0x80; });
}
std::string PreviewPage(const std::string& text, uint32_t page, uint32_t& pages) {
    // Conservative pages for the maximum glyph metrics; line breaks and UTF-8
    // survive unchanged, and every received character remains reachable.
    std::vector<size_t> boundaries{0};
    unsigned columns = 0, lines = 1;
    for (size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        const size_t width = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        if (columns >= kPreviewColumns) {
            columns = 0;
            if (++lines > kPreviewLines) {
                boundaries.push_back(i);
                lines = 1;
            }
        }
        ++columns;
        i = std::min(text.size(), i + width);
        if (lead == '\n') {
            columns = 0;
            if (++lines > kPreviewLines && i < text.size()) {
                boundaries.push_back(i);
                lines = 1;
            }
        }
    }
    boundaries.push_back(text.size());
    pages = boundaries.size() - 1;
    page = std::min(page, pages - 1);
    return text.substr(boundaries[page], boundaries[page + 1] - boundaries[page]);
}
bool MultipleGuests(const std::string& text) {
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    unsigned previous = 0;
    for (size_t pos = 0; (pos = lower.find("guest", pos)) != std::string::npos; pos += 5) {
        size_t i = pos + 5;
        while (i < lower.size() && (lower[i] == ' ' || lower[i] == '#'))
            ++i;
        unsigned value = 0;
        while (i < lower.size() && std::isdigit(static_cast<unsigned char>(lower[i]))) {
            value = std::min(1201U, value * 10 + static_cast<unsigned>(lower[i] - '0'));
            ++i;
        }
        if (value && previous && value != previous)
            return true;
        if (value)
            previous = value;
    }
    return false;
}
void Review::Reset(const VoiceId& recording, const VoiceId& binding, const std::string& session) {
    recording_ = recording;
    binding_ = binding;
    session_ = session;
    snapshot_ = {};
    pending_ = {};
    cursor_ = {};
    stage_ = Stage::None;
    target_ = 0;
    next_text_page_ = next_target_page_ = 0;
    text_reviewed_ = target_reviewed_ = false;
}
bool Review::matches(const VoiceId& recording, const VoiceId& binding,
                     const std::string& session) const {
    return recording_ == recording && binding_ == binding && session_ == session;
}
bool Review::Accept(const Snapshot& snapshot, const std::string& session) {
    if (!matches(snapshot.recording_id, snapshot.binding_id, session) ||
        !(cursor_ == snapshot.cursor) || snapshot.revision < snapshot_.revision ||
        stage_ == Stage::Saving || stage_ == Stage::Saved ||
        (snapshot.revision == snapshot_.revision &&
         ((snapshot_.complete && !snapshot.complete) ||
          snapshot.received_segments < snapshot_.received_segments)))
        return false;
    const bool changed = snapshot_.segment_id != snapshot.segment_id ||
                         snapshot_.revision != snapshot.revision ||
                         snapshot_.text != snapshot.text || snapshot_.aliases != snapshot.aliases ||
                         snapshot_.requires_desk_review != snapshot.requires_desk_review;
    snapshot_ = snapshot;
    if (changed || stage_ == Stage::None) {
        target_ = 0;
        stage_ = Stage::Preview;
        next_text_page_ = next_target_page_ = 0;
        text_reviewed_ = target_reviewed_ = false;
    }
    return true;
}
void Review::Navigate(Cursor cursor) {
    if (stage_ == Stage::Saving)
        return;
    cursor_ = cursor;
    stage_ = Stage::None;
    target_ = 0;
    pending_ = {};
    next_text_page_ = next_target_page_ = 0;
    text_reviewed_ = target_reviewed_ = false;
}
bool Review::CanRoute() const {
    return snapshot_.complete && snapshot_.stopped && snapshot_.segment_id != VoiceId{} &&
           !snapshot_.text.empty();
}
bool Review::GuestAllowed() const {
    return CanRoute() && !snapshot_.requires_desk_review && snapshot_.full_text_length <= 500 &&
           !MultipleGuests(snapshot_.text);
}
bool Review::Choose() {
    if (stage_ == Stage::Preview && CanRoute() && text_reviewed_) {
        stage_ = Stage::Target;
        target_ = 0;
        next_target_page_ = 0;
        target_reviewed_ = false;
        return true;
    }
    if (stage_ == Stage::Target && target_ > 0 && !MoreAliasesSelected() && target_reviewed_) {
        stage_ = Stage::Confirm;
        return true;
    }
    return false;
}
void Review::NextTarget() {
    if (stage_ != Stage::Target)
        return;
    const size_t options =
        2 + (GuestAllowed() ? snapshot_.aliases.size() + (snapshot_.next_alias_offset >= 0 ? 1 : 0)
                            : 0);
    target_ = (target_ + 1) % options;
    next_target_page_ = 0;
    target_reviewed_ = false;
}
std::string Review::TargetLabel() const {
    if (target_ == 0)
        return "Unassigned";
    if (target_ == 1)
        return "General note";
    if (MoreAliasesSelected())
        return "More guests";
    return target_ - 2 < snapshot_.aliases.size() ? snapshot_.aliases[target_ - 2].label
                                                  : "Unassigned";
}
bool Review::MoreAliasesSelected() const {
    return GuestAllowed() && snapshot_.next_alias_offset >= 0 &&
           target_ == snapshot_.aliases.size() + 2;
}
bool Review::Confirm(const VoiceId& request) {
    if (stage_ != Stage::Confirm || !CanRoute() || !target_ || !text_reviewed_ ||
        !target_reviewed_ || request == VoiceId{})
        return false;
    pending_ = {binding_,           recording_,     snapshot_.segment_id, request,
                snapshot_.revision, snapshot_.text, target_ > 1,          {}};
    const auto first = pending_.text.find_first_not_of(" \t\r\n");
    const auto last = pending_.text.find_last_not_of(" \t\r\n");
    if (first == std::string::npos)
        return false;
    pending_.text = pending_.text.substr(first, last - first + 1);
    if (pending_.guest) {
        if (!GuestAllowed() || target_ - 2 >= snapshot_.aliases.size())
            return false;
        pending_.alias = snapshot_.aliases[target_ - 2];
    }
    stage_ = Stage::Saving;
    return true;
}
bool Review::Saved(const Route& receipt, const std::string& session) {
    if (stage_ != Stage::Saving || session != session_ || !SameRoute(pending_, receipt))
        return false;
    stage_ = Stage::Saved;
    return true;
}
void Review::CancelChoice() {
    if (stage_ == Stage::Saving)
        return;
    target_ = 0;
    stage_ = Stage::Preview;
    pending_ = {};
    next_target_page_ = 0;
    target_reviewed_ = false;
}
void Review::MarkTextPage(uint32_t page, uint32_t pages) {
    if (stage_ != Stage::Preview || !pages || page != next_text_page_ || page >= pages)
        return;
    ++next_text_page_;
    if (next_text_page_ == pages)
        text_reviewed_ = true;
}
void Review::MarkTargetPage(uint32_t page, uint32_t pages) {
    if (stage_ != Stage::Target || !pages || page != next_target_page_ || page >= pages)
        return;
    ++next_target_page_;
    if (next_target_page_ == pages)
        target_reviewed_ = true;
}
}  // namespace provisions::service
