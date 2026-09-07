#include <esp_timer.h>
#include <algorithm>
#include "provisions_voice_recorder.h"
namespace provisions {
bool VoiceRecorder::CanDictate(uint32_t press, const VoiceId& conversation, int64_t now_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto& r = dictation_snapshot_;
    const bool reserved = press != 0 && dictation_ready_press_ == press;
    return storage_ready_.load() && !dictation_error_.load() && !dictation_input_blocked_.load() &&
           has_context_.load() && r.authorized && r.state == dictation::State::Open &&
           r.pending == dictation::Action::None && !r.stop_requested &&
           r.conversation_id == conversation && context_.conversation_id == conversation &&
           now_ms > 0 && now_ms < r.expires_ms && press > dictation_closed_through_ &&
           (reserved || (!dictation_busy_.load() && r.count < 60 &&
                         pending_count_.load() < VoiceOutbox::kSlots));
}
bool VoiceRecorder::BeginDictation(uint32_t press, uint64_t captured_ms) {
    if (!recording_)
        return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto& record = dictation_snapshot_;
        if (dictation_error_.load() || dictation_input_blocked_.load() || !storage_ready_.load() ||
            !has_context_.load() || !record.authorized ||
            record.pending != dictation::Action::None || record.stop_requested ||
            record.conversation_id != context_.conversation_id || captured_ms == 0 ||
            captured_ms >= static_cast<uint64_t>(record.expires_ms))
            return false;
        if (dictation_ready_press_ == press)
            return recording_->IsRecording(press);
        if (dictation_busy_.load() || press == 0 || press <= dictation_closed_through_)
            return false;
        dictation_busy_.store(true);
        dictation_requested_press_ = press;
        dictation_captured_ms_ = captured_ms;
    }
    Wake();
    return false;  // The worker first persists the immutable request and ordinal.
}
bool VoiceRecorder::DictationPreparing(uint32_t press) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dictation_busy_.load() && dictation_requested_press_ == press &&
           press > dictation_closed_through_;
}
dictation::Record VoiceRecorder::DictationRecord() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dictation_snapshot_;
}
bool VoiceRecorder::CanReplaceEmptyDictationLocked(const VoiceId& conversation) const {
    return recording_ && storage_ready_.load() && !dictation_error_.load() &&
           !dictation_busy_.load() && !dictation_replacing_.load() && has_context_.load() &&
           context_.conversation_id == conversation &&
           dictation_snapshot_.conversation_id != conversation &&
           dictation::CanRetireEmpty(dictation_snapshot_) && pending_count_.load() == 0 &&
           dictation_command_ == dictation::Action::None && !dictation_stop_requested_ &&
           !dictation_reply_ && !dictation_requested_press_ && !dictation_ready_press_ &&
           recording_->IsIdle();
}
bool VoiceRecorder::CanReplaceEmptyDictation(const VoiceId& conversation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return CanReplaceEmptyDictationLocked(conversation);
}
bool VoiceRecorder::RequestEmptyDictationReplacement(const VoiceId& conversation) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!CanReplaceEmptyDictationLocked(conversation))
            return false;
        dictation_replacing_.store(true);
        dictation_input_blocked_.store(true);
        dictation_command_ = dictation::Action::Start;
        dictation_command_conversation_ = conversation;
        dictation_replace_previous_ = dictation_snapshot_.id;
        dictation_snapshot_.authorized = false;
    }
    Wake();
    return true;
}
bool VoiceRecorder::ReplaceEmptyDictation(const VoiceId& previous, const VoiceId& conversation) {
    // This worker is the only outbox/journal writer. Admission and assignment
    // activation stay closed during inspection and the single replacement commit.
    if (dictation_retry_work_ || dictation_missing_parts_ || !outbox_.journal() ||
        dictation_journal_.Faulted() || !recording_->IsIdle())
        return false;
    for (size_t slot = 0; slot < VoiceOutbox::kSlots; ++slot) {
        SavedVoiceCapture saved;
        const auto result = outbox_.journal()->Read(slot, saved);
        if (result != VoiceStoreResult::Empty) {
            if (result != VoiceStoreResult::Ok)
                dictation_error_.store(true);
            RefreshCount();
            return false;
        }
    }
    VoiceId id{};
    if (!outbox_.NewRequestId(id)) {
        dictation_error_.store(true);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!dictation_replacing_.load() || !has_context_.load() ||
            context_.conversation_id != conversation ||
            requested_context_.conversation_id != conversation || !requested_commit_ ||
            !context_prepared_ || dictation_stop_requested_ || dictation_requested_press_ ||
            dictation_ready_press_ || !recording_->IsIdle())
            return false;
    }
    return dictation_journal_.ReplaceEmpty(previous, id, conversation);
}
bool VoiceRecorder::RequestDictationControl(dictation::Action action) {
    if (!recording_ || action == dictation::Action::None)
        return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (dictation_replacing_.load())
            return false;
        if (action == dictation::Action::Stop)
            dictation_stop_requested_ = true;
        else {
            if (dictation_command_ != dictation::Action::None)
                return false;
            dictation_command_ = action;
            dictation_command_conversation_ = context_.conversation_id;
        }
        if (action != dictation::Action::Receipt) {
            dictation_snapshot_.authorized = false;
            dictation_input_blocked_.store(true);
        }
    }
    Wake();
    return true;
}
bool VoiceRecorder::AcknowledgeDictation(const dictation::Reply& reply) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (dictation_reply_)
        return false;
    dictation_reply_ = reply;
    Wake();
    return true;
}
void VoiceRecorder::PublishDictation() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dictation_snapshot_ = dictation_journal_.Get();
    }
    notify_(Result::DictationChanged, 0);
}
void VoiceRecorder::PrepareDictation() {
    uint32_t press = 0;
    uint64_t captured_ms = 0;
    VoiceContext context;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        press = dictation_requested_press_;
        if (!press)
            return;
        if (press <= dictation_closed_through_ || dictation_stop_requested_) {
            dictation_requested_press_ = 0;
            dictation_busy_.store(false);
            return;
        }
        captured_ms = dictation_captured_ms_;
        context = context_;
    }
    VoiceId request{};
    const bool eligible = storage_ready_.load() && pending_count_.load() < VoiceOutbox::kSlots &&
                          dictation_journal_.CanCapture(context.conversation_id, captured_ms);
    const bool allocated = eligible && outbox_.NewRequestId(request);
    bool ok = allocated && dictation_journal_.Reserve(request);
    VoiceCapture capture;
    if (ok) {
        const auto& record = dictation_journal_.Get();
        capture.purpose = VoicePurpose::Dictation;
        capture.request_id = request;
        capture.dictation_session_id = record.id;
        capture.chunk_sequence = record.count - 1;
        std::lock_guard<std::mutex> lock(mutex_);
        // A release, Stop or screen exit during the flash write closes this
        // reservation before the application can start the microphone.
        if (press <= dictation_closed_through_ || dictation_stop_requested_ ||
            context_.conversation_id != context.conversation_id)
            ok = false;
        else {
            ok = recording_->Begin(press, context, captured_ms, &capture);
            if (ok)
                dictation_ready_press_ = press;
        }
        dictation_requested_press_ = 0;
    } else {
        std::lock_guard<std::mutex> lock(mutex_);
        dictation_requested_press_ = 0;
    }
    if (!ok) {
        if (capture.IsDictation())
            dictation_journal_.AbandonEmpty(request);
        dictation_busy_.store(false);
        dictation_error_.store(dictation_missing_parts_ || (eligible && !allocated) ||
                               dictation_journal_.Faulted());
    }
    PublishDictation();
    if (ok)
        notify_(Result::DictationReady, press);
}
void VoiceRecorder::ServiceDictation() {
    dictation::Action command;
    bool stop;
    std::optional<dictation::Reply> reply;
    VoiceContext context;
    VoiceId command_conversation;
    VoiceId replace_previous;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        command = dictation_command_;
        dictation_command_ = dictation::Action::None;
        stop = dictation_stop_requested_;
        dictation_stop_requested_ = false;
        reply = dictation_reply_;
        dictation_reply_.reset();
        context = context_;
        command_conversation = dictation_command_conversation_;
        replace_previous = dictation_replace_previous_;
        dictation_replace_previous_ = {};
    }
    bool changed = false, authorized = false;
    // Released/processing holds have already reserved their ordinals. Run after
    // Save so empty taps can withdraw a reservation before Stop freezes the count.
    if (command == dictation::Action::Start && storage_ready_.load() && has_context_.load() &&
        command_conversation == context.conversation_id) {
        if (replace_previous != VoiceId{})
            changed = ReplaceEmptyDictation(replace_previous, command_conversation);
        else {
            VoiceId id{};
            if (outbox_.NewRequestId(id))
                changed = dictation_journal_.Start(id, context.conversation_id) || changed;
        }
    } else if (command == dictation::Action::Resume)
        changed = dictation_journal_.Resume() || changed;
    else if (command == dictation::Action::Receipt)
        changed = dictation_journal_.RequestReceipt() || changed;
    if (stop)
        changed = dictation_journal_.Stop() || changed;
    if (reply && dictation_journal_.Apply(*reply)) {
        changed = true;
        authorized = (reply->action == dictation::Action::Start ||
                      reply->action == dictation::Action::Resume) &&
                     dictation_journal_.Get().authorized;
    }
    if (dictation_journal_.Faulted())
        dictation_error_.store(true);
    if (replace_previous != VoiceId{}) {
        std::lock_guard<std::mutex> lock(mutex_);
        // A rejected replacement preserves the old acknowledged authority for
        // its original assignment. Faults and scope/fresh-edge checks still
        // prevent input; a successful replacement remains pending Start ACK.
        dictation_input_blocked_.store(!dictation_journal_.Get().authorized);
        dictation_replacing_.store(false);
    }
    if (changed || stop || command != dictation::Action::None || reply)
        PublishDictation();
    if (authorized) {
        // Stop may arrive while the ACK commits or its UI notification runs.
        // Publish input authority under the same lock as new control requests.
        // A newer Stop always keeps the physical input closed.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            authorized = !dictation_stop_requested_ && dictation_snapshot_.authorized &&
                         (dictation_command_ == dictation::Action::None ||
                          dictation_command_ == dictation::Action::Receipt);
            if (authorized) {
                dictation_authorization_.fetch_add(1);
                dictation_input_blocked_.store(false);
            }
        }
        if (authorized)
            notify_(Result::DictationAuthorized, 0);
    }
}
}  // namespace provisions
