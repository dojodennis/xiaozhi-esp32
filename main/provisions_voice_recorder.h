#ifndef PROVISIONS_VOICE_RECORDER_H
#define PROVISIONS_VOICE_RECORDER_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include "provisions_dictation.h"
#include "provisions_voice_outbox_esp.h"
#include "provisions_voice_recording.h"

namespace provisions {
struct VoiceReplay {
    VoiceCapture capture{};
    size_t slot = VoiceOutbox::kSlots;
    size_t bytes = 0;
    uint8_t* frames = nullptr;
    std::array<uint8_t, 32> digest{};
    uint32_t press = 0;     // Zero after a restart: replay must stay silent.
    VoiceId retry_token{};  // Only an explicit retry offers the server challenge.
    ~VoiceReplay();
};
struct VoiceCaptureReceipt {
    VoiceCapture capture{};
    size_t bytes = 0;
    std::array<uint8_t, 32> digest{};
    bool durable = false;
    bool needs_attention = false;
    VoiceId retry_token{};
    bool retry_used = false;
};

// The button and input callbacks only copy bounded PCM/control state. This one
// worker owns NVS, flash, encoding and replay preparation. Network transmission
// consumes an immutable replay buffer elsewhere and cannot delay local saves.
class VoiceRecorder {
public:
    enum class Result {
        Saved,
        Failed,
        NeedsAttention,
        Synced,
        ContextReady,
        RetryQueued,
        RetryUnavailable,
        DictationChanged,
        DictationReady,
        DictationAuthorized,
        DictationRecorded
    };
    using Notify = std::function<void(Result, uint32_t)>;
    using ReplayReady = std::function<void(std::shared_ptr<const VoiceReplay>)>;
    VoiceRecorder() = default;
    ~VoiceRecorder();
    bool Start(Notify notify, ReplayReady replay_ready);
    // Network callbacks may wait up to one second for the worker to prepare a
    // durable cache. Neither function performs flash I/O on its caller.
    bool PrepareContext(const VoiceContext& context);
    bool ActivateContext(const VoiceContext& context);
    bool UpdateContext(const VoiceContext& context);
    bool Begin(uint32_t press, uint64_t captured_unix_ms);
    bool BeginDictation(uint32_t press, uint64_t captured_unix_ms);
    bool CanDictate(uint32_t press, const VoiceId& conversation, int64_t now_ms) const;
    bool DictationPreparing(uint32_t press) const;
    bool MatchesConversation(const VoiceId& conversation) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return has_context_.load() && context_.conversation_id == conversation;
    }
    bool RequestDictationControl(dictation::Action action);
    bool CanReplaceEmptyDictation(const VoiceId& conversation) const;
    bool RequestEmptyDictationReplacement(const VoiceId& conversation);
    bool AcknowledgeDictation(const dictation::Reply& reply);
    dictation::Record DictationRecord() const;
    bool DictationFaulted() const { return dictation_error_.load(); }
    bool DictationBusy() const { return dictation_busy_.load() || dictation_replacing_.load(); }
    bool DictationCapped(uint32_t press) const {
        return press != 0 && dictation_capped_press_.load() == press;
    }
    uint32_t DictationAuthorization() const { return dictation_authorization_.load(); }
    bool Append(uint32_t press, const int16_t* pcm, size_t frames, size_t channels);
    void Fail(uint32_t press);
    void Release(uint32_t press);
    void RequestReplay();
    bool RequestRetry();
    bool Acknowledge(const VoiceCaptureReceipt& receipt);
    bool HasContext() const { return has_context_.load(); }
    bool IsReady() const { return storage_ready_.load(); }
    unsigned PendingCount() const { return pending_count_.load(); }
    bool NeedsAttention() const { return needs_attention_.load(); }
    bool CanRetry() const { return can_retry_.load(); }
    bool RetryPending() const { return retry_pending_count_.load() != 0; }

private:
    std::array<int16_t*, VoiceRecording::kBufferCount> pcm_{};
    uint8_t* frames_ = nullptr;
    std::unique_ptr<VoiceRecording> recording_;
    std::shared_ptr<VoiceReplay> replay_;
    EspVoiceOutbox outbox_;
    Notify notify_;
    ReplayReady replay_ready_;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> stopped_{true};
    std::atomic<bool> storage_ready_{false};
    std::atomic<bool> has_context_{false};
    std::atomic<unsigned> pending_count_{0};
    std::atomic<bool> needs_attention_{false};
    std::atomic<bool> can_retry_{false};
    std::atomic<unsigned> retry_pending_count_{0};
    mutable std::mutex mutex_;
    std::condition_variable context_cv_;
    VoiceContext context_{};
    VoiceContext requested_context_{};
    bool requested_commit_ = false;
    bool context_prepared_ = false;
    bool context_prepare_failed_ = false;
    bool context_dirty_ = false;
    bool context_write_failed_ = false;
    bool allow_key_creation_ = false;
    bool retry_storage_ = false;
    bool replay_requested_ = false;
    bool repair_requested_ = false;
    VoiceId repair_conversation_id_{};
    std::array<VoiceCaptureReceipt, VoiceOutbox::kSlots> receipts_{};
    size_t receipt_count_ = 0;
    std::array<uint32_t, VoiceOutbox::kSlots> presses_{};
    std::array<int64_t, VoiceOutbox::kSlots> retry_after_{};
    std::array<bool, VoiceOutbox::kSlots> attention_{};
    std::array<bool, VoiceOutbox::kSlots> offered_{};
    std::array<VoiceId, VoiceOutbox::kSlots> retry_tokens_{};
    std::array<bool, VoiceOutbox::kSlots> retry_used_{};
    std::array<bool, VoiceOutbox::kSlots> retry_pending_{};
    dictation::NvsStore dictation_store_;
    dictation::Journal dictation_journal_{dictation_store_};
    dictation::Record dictation_snapshot_{};
    std::atomic<bool> dictation_busy_{false}, dictation_error_{false};
    std::atomic<uint32_t> dictation_capped_press_{0};
    std::atomic<uint32_t> dictation_authorization_{0};
    std::atomic<bool> dictation_input_blocked_{true};
    uint32_t dictation_requested_press_ = 0, dictation_ready_press_ = 0,
             dictation_closed_through_ = 0;
    uint64_t dictation_captured_ms_ = 0;
    dictation::Action dictation_command_ = dictation::Action::None;
    VoiceId dictation_command_conversation_{};
    VoiceId dictation_replace_previous_{};
    std::atomic<bool> dictation_replacing_{false};
    bool CanReplaceEmptyDictationLocked(const VoiceId& conversation) const;
    bool ReplaceEmptyDictation(const VoiceId& previous, const VoiceId& conversation);
    bool dictation_stop_requested_ = false;
    std::optional<dictation::Reply> dictation_reply_;
    std::optional<VoiceRecording::Work> dictation_retry_work_;
    bool dictation_missing_parts_ = false;  // Worker-owned reboot recovery finding.
    int64_t dictation_retry_after_ = 0;
    void PrepareDictation();
    void ServiceDictation();
    void PublishDictation();
    void Wake();
    void Run();
    bool LoadContext(VoiceContext& context, bool committed = true);
    bool SaveContext(const VoiceContext& context, bool committed);
    bool Encode(const VoiceRecording::Work& work, VoiceCapture& capture, size_t& bytes);
    void Save(const VoiceRecording::Work& work);
    void PrepareReplay();
    bool PrepareRetry(const VoiceId& conversation_id);
    void ApplyReceipt(const VoiceCaptureReceipt& receipt);
    void RefreshCount();
};
}  // namespace provisions
#endif
