#include "provisions_voice_recorder.h"
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <mbedtls/platform_util.h>
#include <nvs.h>
#include <psa/crypto.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include "esp_audio_enc.h"
#include "esp_opus_enc.h"

namespace provisions {
namespace {
constexpr size_t kContextBytes = 40;
constexpr int64_t kRetryDelayUs = 30LL * 1000 * 1000;
// The packaged ESP Opus encoder reports 104 samples of lookahead at 16 kHz.
// Budget 20 ms of zero padding after microphone closure so the final samples
// leave the encoder even when capture ends on a frame boundary. This fits the
// existing 167-frame journal/wire cap at the full 160000 captured samples.
constexpr size_t kCaptureTailSamples = 320;
// The packaged 60 ms VOIP encoder alone needs over 20 KiB of task stack.
// Keep room for this worker's capture/receipt locals and the persistence path.
constexpr size_t kRecordingStackBytes = 40 * 1024;
static_assert((VoiceRecording::kMaxSamples + kCaptureTailSamples + 959) / 960 <=
              VoiceOutbox::kMaxPackets);
bool HasId(const VoiceId& id) {
    return std::any_of(id.begin(), id.end(), [](uint8_t byte) { return byte != 0; });
}
std::array<uint8_t, kContextBytes> ContextBytes(const VoiceContext& context, bool committed) {
    std::array<uint8_t, kContextBytes> data{};
    std::memcpy(data.data(), committed ? "ORC1" : "ORP1", 4);
    std::copy(context.conversation_id.begin(), context.conversation_id.end(), data.begin() + 4);
    std::copy(context.source_request_id.begin(), context.source_request_id.end(),
              data.begin() + 20);
    for (size_t i = 0; i < 4; ++i)
        data[36 + i] = (context.source_revision >> (8 * i)) & 255;
    return data;
}
bool Digest(VoiceBytes input, std::array<uint8_t, 32>& digest) {
    size_t written = 0;
    return psa_crypto_init() == PSA_SUCCESS &&
           psa_hash_compute(PSA_ALG_SHA_256, input.data, input.size, digest.data(), digest.size(),
                            &written) == PSA_SUCCESS &&
           written == digest.size();
}
bool SameCapture(const VoiceCapture& a, const VoiceCapture& b) {
    return a.request_id == b.request_id && a.conversation_id == b.conversation_id &&
           a.source_request_id == b.source_request_id && a.source_revision == b.source_revision &&
           a.captured_unix_ms == b.captured_unix_ms && a.packet_count == b.packet_count &&
           a.purpose == b.purpose && a.dictation_session_id == b.dictation_session_id &&
           a.chunk_sequence == b.chunk_sequence && a.sample_count == b.sample_count;
}
}  // namespace
VoiceReplay::~VoiceReplay() {
    if (frames) {
        mbedtls_platform_zeroize(frames, VoiceOutbox::kMaxFrameBytes);
        heap_caps_free(frames);
    }
}
VoiceRecorder::~VoiceRecorder() {
    stopping_.store(true);
    Wake();
    while (!stopped_.load())
        vTaskDelay(pdMS_TO_TICKS(10));
    for (auto* pcm : pcm_) {
        if (pcm) {
            mbedtls_platform_zeroize(pcm, VoiceRecording::kMaxSamples * sizeof(int16_t));
            heap_caps_free(pcm);
        }
    }
    if (frames_) {
        mbedtls_platform_zeroize(frames_, VoiceOutbox::kMaxFrameBytes);
        heap_caps_free(frames_);
    }
}
bool VoiceRecorder::Start(Notify notify, ReplayReady replay_ready) {
    if (recording_ || !notify || !replay_ready)
        return false;
    notify_ = std::move(notify);
    replay_ready_ = std::move(replay_ready);
    for (auto& pcm : pcm_)
        pcm = static_cast<int16_t*>(heap_caps_calloc(VoiceRecording::kMaxSamples, sizeof(int16_t),
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    frames_ = static_cast<uint8_t*>(
        heap_caps_malloc(VoiceOutbox::kMaxFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    replay_ = std::make_shared<VoiceReplay>();
    replay_->frames = static_cast<uint8_t*>(
        heap_caps_malloc(VoiceOutbox::kMaxFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pcm_[0] || !pcm_[1] || !frames_ || !replay_->frames)
        return false;
    recording_ = std::make_unique<VoiceRecording>(pcm_[0], pcm_[1], VoiceRecording::kMaxSamples);
    stopped_.store(false);
    if (xTaskCreate(
            [](void* argument) {
                auto* recorder = static_cast<VoiceRecorder*>(argument);
                recorder->Run();
                recorder->stopped_.store(true);
                vTaskDelete(nullptr);
            },
            "orbit_record", kRecordingStackBytes, this, 3, &task_) != pdPASS) {
        stopped_.store(true);
        task_ = nullptr;
        return false;
    }
    return true;
}
void VoiceRecorder::Wake() {
    if (task_)
        xTaskNotifyGive(task_);
}
bool VoiceRecorder::PrepareContext(const VoiceContext& context) {
    if (!VoiceRecording::ValidContext(context) || !recording_)
        return false;
    std::unique_lock<std::mutex> lock(mutex_);
    if (context_prepared_ && requested_context_ == context)
        return true;
    requested_context_ = context;
    requested_commit_ = false;
    context_prepared_ = false;
    context_prepare_failed_ = false;
    context_dirty_ = true;
    allow_key_creation_ = true;  // Authenticated, negotiated gateway frames only.
    retry_storage_ = true;
    Wake();
    return context_cv_.wait_for(lock, std::chrono::milliseconds(1000),
                                [&]() {
                                    return !(requested_context_ == context) || context_prepared_ ||
                                           context_prepare_failed_ || stopping_.load();
                                }) &&
           requested_context_ == context && context_prepared_ && !stopping_.load();
}
bool VoiceRecorder::ActivateContext(const VoiceContext& context) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!context_prepared_ || !(requested_context_ == context))
            return false;
        context_ = context;
        has_context_.store(true);
        if (!requested_commit_) {
            requested_commit_ = true;
            context_dirty_ = true;
            retry_storage_ = true;
        }
    }
    Wake();
    return true;
}
bool VoiceRecorder::UpdateContext(const VoiceContext& context) {
    return PrepareContext(context) && ActivateContext(context);
}

bool VoiceRecorder::Begin(uint32_t press, uint64_t captured_unix_ms) {
    if (!recording_ || !storage_ready_.load() || !has_context_.load())
        return false;
    VoiceContext context;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        context = context_;
    }
    return recording_->Begin(press, context, captured_unix_ms);
}
bool VoiceRecorder::Append(uint32_t press, const int16_t* pcm, size_t frames, size_t channels) {
    const bool appended = recording_ && recording_->Append(press, pcm, frames, channels);
    if (appended && recording_->IsCapped(press)) {
        dictation_capped_press_.store(press);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dictation_closed_through_ = std::max(dictation_closed_through_, press);
        }
        Wake();
    }
    return appended;
}
void VoiceRecorder::Fail(uint32_t press) {
    if (recording_) {
        recording_->Fail(press);
        recording_->Release(press);
        Wake();
    }
}
void VoiceRecorder::Release(uint32_t press) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dictation_closed_through_ = std::max(dictation_closed_through_, press);
    }
    if (recording_) {
        recording_->Release(press);
        Wake();
    }
}
void VoiceRecorder::RequestReplay() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        replay_requested_ = true;
    }
    Wake();
}
bool VoiceRecorder::RequestRetry() {
    if (!CanRetry() || !IsReady() || !HasContext())
        return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        repair_requested_ = true;
        repair_conversation_id_ = context_.conversation_id;
    }
    Wake();
    return true;
}
bool VoiceRecorder::Acknowledge(const VoiceCaptureReceipt& receipt) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (receipt_count_ == receipts_.size())
        return false;
    receipts_[receipt_count_++] = receipt;
    Wake();
    return true;
}
bool VoiceRecorder::LoadContext(VoiceContext& context, bool committed) {
    nvs_handle_t handle = 0;
    if (nvs_open("orbit_audio", NVS_READONLY, &handle) != ESP_OK)
        return false;
    std::array<uint8_t, kContextBytes> data{};
    size_t bytes = data.size();
    const auto result = nvs_get_blob(handle, "context_v1", data.data(), &bytes);
    nvs_close(handle);
    if (result != ESP_OK || bytes != data.size() ||
        std::memcmp(data.data(), committed ? "ORC1" : "ORP1", 4) != 0)
        return false;
    std::copy_n(data.begin() + 4, 16, context.conversation_id.begin());
    std::copy_n(data.begin() + 20, 16, context.source_request_id.begin());
    context.source_revision = 0;
    for (size_t i = 0; i < 4; ++i)
        context.source_revision |= static_cast<uint32_t>(data[36 + i]) << (8 * i);
    return VoiceRecording::ValidContext(context);
}
bool VoiceRecorder::SaveContext(const VoiceContext& context, bool committed) {
    const auto data = ContextBytes(context, committed);
    nvs_handle_t handle = 0;
    if (nvs_open("orbit_audio", NVS_READWRITE, &handle) != ESP_OK)
        return false;
    auto result = nvs_set_blob(handle, "context_v1", data.data(), data.size());
    if (result == ESP_OK)
        result = nvs_commit(handle);
    nvs_close(handle);
    VoiceContext verified;
    return result == ESP_OK && LoadContext(verified, committed) && verified == context;
}
bool VoiceRecorder::Encode(const VoiceRecording::Work& work, VoiceCapture& capture, size_t& bytes) {
    bytes = 0;
    if (work.failed || work.samples == 0 || work.samples > VoiceRecording::kMaxSamples)
        return false;
    esp_opus_enc_config_t config = {
        .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,
        .channel = ESP_AUDIO_MONO,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .bitrate = ESP_OPUS_BITRATE_AUTO,
        .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
        .application_mode = ESP_OPUS_ENC_APPLICATION_VOIP,
        .complexity = 0,
        .enable_fec = false,
        .enable_dtx = true,
        .enable_vbr = true,
    };
    void* encoder = nullptr;
    if (esp_opus_enc_open(&config, sizeof(config), &encoder) != ESP_AUDIO_ERR_OK || !encoder)
        return false;
    int input_bytes = 0, output_bytes = 0;
    bool ok =
        esp_opus_enc_get_frame_size(encoder, &input_bytes, &output_bytes) == ESP_AUDIO_ERR_OK &&
        input_bytes == 1920 && output_bytes > 0 && output_bytes <= 2048;
    std::array<int16_t, 960> input{};
    std::array<uint8_t, 2048> output{};
    capture = work.capture;
    if (capture.IsDictation())
        capture.sample_count = work.samples;
    capture.packet_count = 0;
    size_t opus_bytes = 0;
    for (size_t offset = 0; ok && offset < work.samples + kCaptureTailSamples;
         offset += input.size()) {
        input.fill(0);
        if (offset < work.samples)
            std::copy_n(work.pcm + offset, std::min(input.size(), work.samples - offset),
                        input.begin());
        esp_audio_enc_in_frame_t in{.buffer = reinterpret_cast<uint8_t*>(input.data()),
                                    .len = 1920};
        esp_audio_enc_out_frame_t out{.buffer = output.data(),
                                      .len = static_cast<uint32_t>(output.size()),
                                      .encoded_bytes = 0,
                                      .pts = 0};
        ok = esp_opus_enc_process(encoder, &in, &out) == ESP_AUDIO_ERR_OK &&
             out.encoded_bytes > 0 && out.encoded_bytes <= output.size() &&
             bytes + 2 + out.encoded_bytes <= VoiceOutbox::kMaxFrameBytes &&
             opus_bytes + out.encoded_bytes <= 256 * 1024;
        if (!ok)
            break;
        frames_[bytes++] = out.encoded_bytes & 255;
        frames_[bytes++] = (out.encoded_bytes >> 8) & 255;
        std::memcpy(frames_ + bytes, output.data(), out.encoded_bytes);
        bytes += out.encoded_bytes;
        opus_bytes += out.encoded_bytes;
        ++capture.packet_count;
    }
    esp_opus_enc_close(encoder);
    mbedtls_platform_zeroize(input.data(), sizeof(input));
    mbedtls_platform_zeroize(output.data(), output.size());
    return ok && capture.packet_count > 0 && capture.packet_count <= VoiceOutbox::kMaxPackets;
}
void VoiceRecorder::Save(const VoiceRecording::Work& work) {
    VoiceCapture capture;
    SavedVoiceCapture saved;
    size_t bytes = 0;
    bool manifest = true;
    if (work.capture.IsDictation()) {
        capture = work.capture;
        capture.sample_count = work.samples;
        manifest = work.samples == 0 ? dictation_journal_.AbandonEmpty(capture.request_id)
                                     : dictation_journal_.Seal(capture);
    }
    const bool empty_dictation = work.capture.IsDictation() && work.samples == 0 && manifest;
    bool ok = empty_dictation ||
              (manifest && storage_ready_.load() && Encode(work, capture, bytes) &&
               (capture.IsDictation() || outbox_.NewRequestId(capture.request_id)) &&
               outbox_.journal()->Save(capture, {frames_, bytes}, saved) == VoiceStoreResult::Ok);
    if (!ok && work.capture.IsDictation()) {
        // Preserve the Processing PCM and its durable ordinal for a later write.
        // A pending Stop counts this reservation even before the raw part syncs.
        mbedtls_platform_zeroize(frames_, VoiceOutbox::kMaxFrameBytes);
        dictation_retry_work_ = work;
        dictation_retry_after_ = esp_timer_get_time() + kRetryDelayUs;
        dictation_error_.store(true);
        PublishDictation();
        return;
    }
    // No other task may touch Processing memory; clear it before freeing its lease.
    mbedtls_platform_zeroize(pcm_[work.slot], VoiceRecording::kMaxSamples * sizeof(int16_t));
    mbedtls_platform_zeroize(frames_, VoiceOutbox::kMaxFrameBytes);
    recording_->Finish(work);
    if (ok && !empty_dictation) {
        presses_[saved.slot] = work.press;
        retry_after_[saved.slot] = 0;
        attention_[saved.slot] = false;
        offered_[saved.slot] = false;
        retry_tokens_[saved.slot] = {};
        retry_used_[saved.slot] = retry_pending_[saved.slot] = false;
    }
    RefreshCount();
    if (work.capture.IsDictation()) {
        dictation_busy_.store(false);
        dictation_error_.store(false);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dictation_ready_press_ = 0;
        }
        PublishDictation();
        if (!empty_dictation)
            RequestReplay();
    } else
        notify_(ok ? Result::Saved : Result::Failed, work.press);
}
void VoiceRecorder::RefreshCount() {
    unsigned count = 0;
    unsigned retry_count = 0;
    bool can_retry = false;
    bool attention = context_write_failed_;
    VoiceContext context;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        context = context_;
    }
    if (outbox_.journal()) {
        for (size_t slot = 0; slot < VoiceOutbox::kSlots; ++slot) {
            SavedVoiceCapture saved;
            const auto result = outbox_.journal()->Read(slot, saved);
            if (result != VoiceStoreResult::Empty)
                ++count;
            if (result != VoiceStoreResult::Ok && result != VoiceStoreResult::Empty)
                attention = true;
            attention = attention || attention_[slot];
            if (result == VoiceStoreResult::Ok &&
                saved.capture.conversation_id == context.conversation_id) {
                if (retry_pending_[slot])
                    ++retry_count;
                can_retry = can_retry || (attention_[slot] && !retry_used_[slot] &&
                                          !retry_pending_[slot] && HasId(retry_tokens_[slot]));
            }
        }
    }
    pending_count_.store(count);
    needs_attention_.store(attention);
    can_retry_.store(can_retry);
    retry_pending_count_.store(retry_count);
}
bool VoiceRecorder::PrepareRetry(const VoiceId& conversation_id) {
    if (!outbox_.journal() || !has_context_.load())
        return false;
    VoiceContext context;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        context = context_;
    }
    if (context.conversation_id != conversation_id)
        return false;
    size_t selected = VoiceOutbox::kSlots;
    uint64_t oldest = UINT64_MAX;
    for (size_t slot = 0; slot < VoiceOutbox::kSlots; ++slot) {
        if (!attention_[slot] || retry_used_[slot] || retry_pending_[slot] ||
            !HasId(retry_tokens_[slot]))
            continue;
        SavedVoiceCapture saved;
        if (outbox_.journal()->Read(slot, saved) == VoiceStoreResult::Ok &&
            saved.capture.conversation_id == context.conversation_id && saved.sequence < oldest) {
            selected = slot;
            oldest = saved.sequence;
        }
    }
    if (selected == VoiceOutbox::kSlots)
        return false;
    retry_pending_[selected] = true;
    retry_after_[selected] = 0;
    RefreshCount();
    return true;
}
void VoiceRecorder::PrepareReplay() {
    if (!outbox_.journal() || replay_.use_count() != 1 || !has_context_.load())
        return;
    VoiceContext context;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        context = context_;
    }
    size_t selected = VoiceOutbox::kSlots;
    uint64_t oldest = UINT64_MAX;
    for (size_t slot = 0; slot < VoiceOutbox::kSlots; ++slot) {
        if ((attention_[slot] && !retry_pending_[slot]) ||
            retry_after_[slot] > esp_timer_get_time())
            continue;
        SavedVoiceCapture saved;
        if (outbox_.journal()->Read(slot, saved) == VoiceStoreResult::Ok &&
            saved.capture.conversation_id == context.conversation_id &&
            (selected == VoiceOutbox::kSlots ||
             (retry_pending_[slot] && !retry_pending_[selected]) ||
             (retry_pending_[slot] == retry_pending_[selected] && saved.sequence < oldest))) {
            selected = slot;
            oldest = saved.sequence;
        }
    }
    SavedVoiceCapture saved;
    if (selected == VoiceOutbox::kSlots ||
        outbox_.journal()->Read(selected, saved) != VoiceStoreResult::Ok)
        return;
    replay_->capture = saved.capture;
    replay_->slot = selected;
    replay_->bytes = saved.frames.size;
    replay_->retry_token = retry_pending_[selected] ? retry_tokens_[selected] : VoiceId{};
    replay_->press = saved.capture.IsDictation() || retry_pending_[selected] || offered_[selected]
                         ? 0
                         : presses_[selected];
    if (!Digest(saved.frames, replay_->digest))
        return;
    std::memcpy(replay_->frames, saved.frames.data, saved.frames.size);
    retry_after_[selected] = esp_timer_get_time() + kRetryDelayUs;
    offered_[selected] = true;
    replay_ready_(replay_);
}
void VoiceRecorder::ApplyReceipt(const VoiceCaptureReceipt& receipt) {
    if (!outbox_.journal())
        return;
    for (size_t slot = 0; slot < VoiceOutbox::kSlots; ++slot) {
        SavedVoiceCapture saved;
        if (outbox_.journal()->Read(slot, saved) != VoiceStoreResult::Ok ||
            !SameCapture(saved.capture, receipt.capture) || saved.frames.size != receipt.bytes)
            continue;
        std::array<uint8_t, 32> digest{};
        if (!Digest(saved.frames, digest) || digest != receipt.digest)
            return;
        if (receipt.durable) {
            if (saved.capture.IsDictation() && !dictation_journal_.Terminal(saved.capture)) {
                dictation_error_.store(true);
                PublishDictation();
                return;
            }
            if (saved.capture.IsDictation())
                dictation_error_.store(dictation_missing_parts_ ||
                                       dictation_retry_work_.has_value() ||
                                       dictation_journal_.Faulted());
            if (outbox_.journal()->RemoveAfterReceipt(slot, receipt.capture.request_id,
                                                      receipt.capture.conversation_id) ==
                VoiceStoreResult::Ok) {
                attention_[slot] = false;
                retry_tokens_[slot] = {};
                retry_used_[slot] = retry_pending_[slot] = false;
                RefreshCount();
                if (saved.capture.IsDictation())
                    PublishDictation();
                else
                    notify_(Result::Synced, presses_[slot]);
            }
        } else if (receipt.retry_used) {
            if (!HasId(receipt.retry_token) ||
                (HasId(retry_tokens_[slot]) && retry_tokens_[slot] != receipt.retry_token))
                return;
            retry_tokens_[slot] = receipt.retry_token;
            retry_used_[slot] = true;
            retry_pending_[slot] = false;
            attention_[slot] = receipt.needs_attention;
            RefreshCount();
            notify_(receipt.needs_attention ? Result::NeedsAttention : Result::RetryQueued, 0);
        } else if (receipt.needs_attention && !retry_used_[slot]) {
            if (HasId(retry_tokens_[slot]) && retry_tokens_[slot] != receipt.retry_token)
                return;
            attention_[slot] = true;
            retry_tokens_[slot] = receipt.retry_token;
            RefreshCount();
            notify_(Result::NeedsAttention, presses_[slot]);
        }
        return;
    }
}
void VoiceRecorder::Run() {
    VoiceContext cached;
    if (LoadContext(cached)) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!context_dirty_) {
            context_ = cached;
            requested_context_ = cached;
            requested_commit_ = true;
            context_prepared_ = true;
            has_context_.store(true);
        }
    }
    storage_ready_.store(outbox_.Initialize(false));
    dictation_error_.store(!dictation_journal_.Initialize());
    // A reserved ordinal without a committed raw part is an interrupted write,
    // never an empty or successfully synced segment after a restart.
    std::array<bool, dictation::kMaximumSegments> dictation_present{};
    if (outbox_.journal()) {
        for (size_t slot = 0; slot < VoiceOutbox::kSlots; ++slot) {
            SavedVoiceCapture saved;
            if (outbox_.journal()->Read(slot, saved) != VoiceStoreResult::Ok ||
                !saved.capture.IsDictation())
                continue;
            const auto& record = dictation_journal_.Get();
            if (saved.capture.dictation_session_id != record.id ||
                saved.capture.chunk_sequence >= record.count ||
                !dictation_journal_.Seal(saved.capture)) {
                dictation_missing_parts_ = true;
                dictation_error_.store(true);
            } else
                dictation_present[saved.capture.chunk_sequence] = true;
        }
    }
    for (uint32_t i = 0; i < dictation_journal_.Get().count; ++i)
        if (!dictation_journal_.Get().segments[i].terminal && !dictation_present[i]) {
            dictation_missing_parts_ = true;
            dictation_error_.store(true);
        }
    dictation_input_blocked_.store(!dictation_journal_.Get().authorized);
    PublishDictation();
    RefreshCount();
    if (storage_ready_.load() && has_context_.load())
        notify_(Result::ContextReady, 0);
    unsigned storage_attempts = 0;
    int64_t storage_retry_after = 0;
    while (!stopping_.load()) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        VoiceContext context;
        VoiceId repair_conversation;
        bool dirty = false, allow = false, replay = false, retry_storage = false,
             commit_context = false, repair = false;
        std::array<VoiceCaptureReceipt, VoiceOutbox::kSlots> receipts;
        size_t count = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            context = requested_context_;
            commit_context = requested_commit_;
            dirty = context_dirty_;
            context_dirty_ = false;
            allow = allow_key_creation_;
            replay = replay_requested_;
            replay_requested_ = false;
            repair = repair_requested_;
            repair_conversation = repair_conversation_id_;
            repair_requested_ = false;
            retry_storage = retry_storage_;
            retry_storage_ = false;
            count = receipt_count_;
            std::copy_n(receipts_.begin(), count, receipts.begin());
            receipt_count_ = 0;
        }
        if (retry_storage || dirty) {
            storage_attempts = 0;
            storage_retry_after = 0;
        }
        if ((dirty || context_write_failed_ || (!storage_ready_.load() && allow)) &&
            storage_attempts < 3 && esp_timer_get_time() >= storage_retry_after) {
            ++storage_attempts;
            storage_retry_after = esp_timer_get_time() + kRetryDelayUs;
            if (!storage_ready_.load() && allow)
                storage_ready_.store(outbox_.Initialize(true));
            context_write_failed_ = !storage_ready_.load() || !SaveContext(context, commit_context);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (requested_context_ == context && requested_commit_ == commit_context) {
                    context_prepared_ = !context_write_failed_;
                    context_prepare_failed_ = context_write_failed_;
                    context_cv_.notify_all();
                }
            }
            RefreshCount();
            if (context_write_failed_)
                notify_(Result::NeedsAttention, 0);
            else if (commit_context)
                notify_(Result::ContextReady, 0);
        }
        VoiceRecording::Work work;
        PrepareDictation();
        if (dictation_retry_work_ && esp_timer_get_time() >= dictation_retry_after_) {
            const auto retry = *dictation_retry_work_;
            dictation_retry_work_.reset();
            Save(retry);
        }
        while (recording_->Take(work))
            Save(work);
        ServiceDictation();
        for (size_t i = 0; i < count; ++i)
            ApplyReceipt(receipts[i]);
        if (repair) {
            const bool queued = PrepareRetry(repair_conversation);
            notify_(queued ? Result::RetryQueued : Result::RetryUnavailable, 0);
            replay = replay || queued;
        }
        if (replay)
            PrepareReplay();
    }
}
}  // namespace provisions
