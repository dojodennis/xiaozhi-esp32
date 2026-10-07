#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "provisions_voice_outbox.h"
struct cJSON;

// Service routing is an explicit note action. These values never enter the
// audio journal or a guest-profile write.
namespace provisions::service {
constexpr size_t kTextBytes = 2000, kAliasPage = 16, kLabelBytes = 80;
// The pinned 30 px font has a 40 px maximum advance and a 43 px line height.
// Include padding and line spacing in these conservative readable page bounds.
constexpr unsigned kPreviewColumns = 7, kPreviewLines = 5;
constexpr int kPreviewWidth = 324, kPreviewHeight = 250, kPreviewPadding = 8, kPreviewLineSpace = 2;
struct Alias {
    VoiceId visit_id{}, profile_id{};
    uint32_t guest_number = 0;
    std::string label;
    bool operator==(const Alias& rhs) const {
        return visit_id == rhs.visit_id && profile_id == rhs.profile_id &&
               guest_number == rhs.guest_number && label == rhs.label;
    }
};
struct Cursor {
    uint32_t sequence = 0, text_offset = 0, alias_offset = 0;
    bool operator==(const Cursor& rhs) const {
        return sequence == rhs.sequence && text_offset == rhs.text_offset &&
               alias_offset == rhs.alias_offset;
    }
};
struct Snapshot {
    VoiceId binding_id{}, recording_id{}, segment_id{};
    uint32_t revision = 0, received_segments = 0, full_text_length = 0;
    int expected_segments = -1, next_text_offset = -1, next_sequence = -1, next_alias_offset = -1;
    Cursor cursor;
    bool complete = false, stopped = false, requires_desk_review = false;
    std::string text;
    std::vector<Alias> aliases;
};
struct Route {
    VoiceId binding_id{}, recording_id{}, segment_id{}, request_id{};
    uint32_t revision = 0;
    std::string text;
    bool guest = false;
    Alias alias;
};
enum class ReviewError { None, WrongRecording, Conflict, UpdateRequired };
struct Rejection {
    VoiceId binding_id{}, recording_id{};
    Cursor cursor;
    ReviewError error = ReviewError::None;
};
bool ParseSnapshot(const cJSON* root, const std::string& session, Snapshot& out);
bool ParseSaved(const cJSON* root, const std::string& session, Route& out);
bool ParseRejection(const cJSON* root, const std::string& session, Rejection& out);
std::string ReviewJson(const std::string& session, const VoiceId& recording, Cursor cursor);
std::string RouteJson(const std::string& session, const Route& route);
size_t CharacterCount(const std::string& text);
std::string PreviewPage(const std::string& text, uint32_t page, uint32_t& pages);
bool MultipleGuests(const std::string& text);

enum class Stage { None, Preview, Target, Confirm, Saving, Saved };
class Review {
public:
    void Reset(const VoiceId& recording = {}, const VoiceId& binding = {},
               const std::string& session = "");
    bool Accept(const Snapshot& snapshot, const std::string& session);
    bool Saved(const Route& receipt, const std::string& session);
    bool Reject(const Rejection& rejection, const std::string& session);
    bool RetryRejected();
    void Navigate(Cursor cursor);
    bool Choose();
    void NextTarget();
    bool Confirm(const VoiceId& request);
    void CancelChoice();
    void MarkTextPage(uint32_t page, uint32_t pages);
    void MarkTargetPage(uint32_t page, uint32_t pages);
    bool text_reviewed() const { return text_reviewed_; }
    bool target_reviewed() const { return target_reviewed_; }
    bool GuestAllowed() const;
    bool MoreAliasesSelected() const;
    bool CanRoute() const;
    const Snapshot& snapshot() const { return snapshot_; }
    const Route& pending() const { return pending_; }
    Stage stage() const { return stage_; }
    Cursor cursor() const { return cursor_; }
    size_t target() const { return target_; }
    ReviewError error() const { return error_; }
    std::string TargetLabel() const;
    bool matches(const VoiceId& recording, const VoiceId& binding,
                 const std::string& session) const;

private:
    Snapshot snapshot_;
    Route pending_;
    VoiceId recording_{}, binding_{};
    std::string session_;
    Cursor cursor_;
    Stage stage_ = Stage::None;
    ReviewError error_ = ReviewError::None;
    size_t target_ = 0;  // 0 unassigned; 1 General; subsequent entries exact aliases.
    uint32_t next_text_page_ = 0, next_target_page_ = 0;
    bool text_reviewed_ = false, target_reviewed_ = false;
};
}  // namespace provisions::service
