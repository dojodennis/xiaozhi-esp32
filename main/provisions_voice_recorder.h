#ifndef PROVISIONS_VOICE_RECORDER_H
#define PROVISIONS_VOICE_RECORDER_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include "provisions_voice_outbox_esp.h"
#include "provisions_voice_recording.h"

namespace provisions {
struct VoiceReplay {
    VoiceCapture capture{};
    size_t slot = VoiceOutbox::kSlots;
    size_t bytes = 0;
    uint8_t* frames = nullptr;
    std::array<uint8_t, 32> digest{};
    uint32_t press = 0;  // Zero after a restart: replay must stay silent.
    ~VoiceReplay();
};
struct VoiceCaptureReceipt {
    VoiceCapture capture{};
    size_t bytes = 0;
    std::array<uint8_t, 32> digest{};
    bool durable = false;
    bool needs_attention = false;
};

// The button and input callbacks only copy bounded PCM/control state. This one
// worker owns NVS, flash, encoding and replay preparation. Network transmission
// consumes an immutable replay buffer elsewhere and cannot delay local saves.
class VoiceRecorder {
public:
    enum class Result { Saved, Failed, NeedsAttention, Synced, ContextReady };
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
    bool Append(uint32_t press, const int16_t* pcm, size_t frames, size_t channels);
    void Fail(uint32_t press);
    void Release(uint32_t press);
    void RequestReplay();
    bool Acknowledge(const VoiceCaptureReceipt& receipt);
    bool HasContext() const { return has_context_.load(); }
    bool IsReady() const { return storage_ready_.load(); }
    unsigned PendingCount() const { return pending_count_.load(); }
    bool NeedsAttention() const { return needs_attention_.load(); }

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
    std::mutex mutex_;
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
    std::array<VoiceCaptureReceipt, VoiceOutbox::kSlots> receipts_{};
    size_t receipt_count_ = 0;
    std::array<uint32_t, VoiceOutbox::kSlots> presses_{};
    std::array<int64_t, VoiceOutbox::kSlots> retry_after_{};
    std::array<bool, VoiceOutbox::kSlots> attention_{};
    std::array<bool, VoiceOutbox::kSlots> offered_{};
    void Wake();
    void Run();
    bool LoadContext(VoiceContext& context, bool committed = true);
    bool SaveContext(const VoiceContext& context, bool committed);
    bool Encode(const VoiceRecording::Work& work, VoiceCapture& capture, size_t& bytes);
    void Save(const VoiceRecording::Work& work);
    void PrepareReplay();
    void ApplyReceipt(const VoiceCaptureReceipt& receipt);
    void RefreshCount();
};
}  // namespace provisions
#endif
