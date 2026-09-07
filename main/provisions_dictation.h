#ifndef PROVISIONS_DICTATION_H_
#define PROVISIONS_DICTATION_H_
#include <array>
#include <string>
#include "provisions_voice_outbox.h"
struct cJSON;

namespace provisions::dictation {
constexpr uint32_t kMaximumSegments = 60;
enum class Action { None, Start, Stop, Resume, Receipt };
enum class State { Empty, Open, Stopped, Reviewed };
struct Segment {
    VoiceId request_id{};
    uint32_t samples = 0;
    bool terminal = false;
};
struct Record {
    VoiceId id{}, conversation_id{};
    State state = State::Empty;
    Action pending = Action::None;
    uint32_t revision = 0, control_revision = 0, pending_revision = 0, frozen_count = 0;
    int64_t expires_ms = 0;
    bool authorized = false, stop_requested = false;
    uint32_t count = 0;
    std::array<Segment, kMaximumSegments> segments{};
};
struct Reply {
    VoiceId id{};
    Action action = Action::None;
    bool acknowledged = false;
    uint32_t payload_revision = 0, payload_count = 0;
    State state = State::Empty;
    uint32_t revision = 0, control_revision = 0, expected_count = 0;
    int64_t expires_ms = 0;
};
class Store {
public:
    enum class LoadResult { Empty, Present, Fault };
    virtual ~Store() = default;
    virtual LoadResult Load(Record& record) = 0;
    virtual bool Save(const Record& record) = 0;
};
class NvsStore final : public Store {
public:
    LoadResult Load(Record& record) override;
    bool Save(const Record& record) override;
};
// One worker owns this journal. Every mutation commits and reads back before
// publishing new authority, a reserved ordinal, or a terminal segment receipt.
class Journal {
public:
    explicit Journal(Store& store) : store_(store) {}
    bool Initialize();
    const Record& Get() const { return record_; }
    bool Faulted() const { return fault_; }
    bool Start(const VoiceId& id, const VoiceId& conversation);
    bool ReplaceEmpty(const VoiceId& previous, const VoiceId& id, const VoiceId& conversation);
    bool Stop();
    bool Resume();
    bool RequestReceipt();
    bool Apply(const Reply& reply);
    bool Reserve(const VoiceId& request);
    bool AbandonEmpty(const VoiceId& request);
    bool Seal(const VoiceCapture& capture);
    bool Terminal(const VoiceCapture& capture);
    bool CanCapture(const VoiceId& conversation, int64_t now_ms) const;

private:
    Store& store_;
    Record record_{};
    bool fault_ = false;
    bool Commit(const Record& next);
};
bool ParseReply(const cJSON* value, const std::string& transport_session, Reply& output);
bool CanRetireEmpty(const Record& record);
std::string ControlJson(const Record& record, const std::string& transport_session);
std::string RecordJson(const Record& record);
bool ParseRecord(const std::string& text, Record& record);
}  // namespace provisions::dictation
#endif
