#include "provisions_voice_recording.h"
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace provisions {
namespace {
bool Nonzero(const VoiceId& id) {
    return std::any_of(id.begin(), id.end(), [](uint8_t byte) { return byte != 0; });
}
}  // namespace
VoiceRecording::VoiceRecording(int16_t* first, int16_t* second, size_t capacity) {
    if (first != nullptr && second != nullptr && first != second && capacity >= kMaxSamples) {
        buffers_[0].pcm = first;
        buffers_[1].pcm = second;
        capacity_ = kMaxSamples;
    }
}
std::string DescribeCapture(uint32_t press, bool saved, size_t samples, const CaptureLevels& levels,
                            size_t opus_bytes, int64_t press_to_first_chunk_ms) {
    constexpr size_t kRateHz = 16000;
    const unsigned rms =
        samples == 0 ? 0
                     : static_cast<unsigned>(std::lround(
                           std::sqrt(static_cast<double>(levels.sum_squares) / samples)));
    // Button-down to the first non-zero sample: time until the first microphone
    // chunk arrived plus the silent samples at the head of the capture.
    long long first_audio_ms = -1;
    if (levels.first_nonzero != SIZE_MAX && press_to_first_chunk_ms >= 0)
        first_audio_ms = press_to_first_chunk_ms +
                         static_cast<long long>(levels.first_nonzero * 1000 / kRateHz);
    char text[240];
    std::snprintf(text, sizeof(text),
                  "capture press=%" PRIu32 " saved=%d duration_ms=%zu samples=%zu opus_bytes=%zu "
                  "rate_hz=%zu peak=%u rms=%u press_to_first_chunk_ms=%lld "
                  "press_to_first_nonzero_ms=%lld leading_zero_ms=%lld",
                  press, saved ? 1 : 0, samples * 1000 / kRateHz, samples, opus_bytes, kRateHz,
                  static_cast<unsigned>(levels.peak), rms,
                  static_cast<long long>(press_to_first_chunk_ms), first_audio_ms,
                  levels.first_nonzero == SIZE_MAX
                      ? -1LL
                      : static_cast<long long>(levels.first_nonzero * 1000 / kRateHz));
    return text;
}
bool VoiceRecording::ValidContext(const VoiceContext& context) {
    return Nonzero(context.conversation_id) && context.source_revision <= 999999999 &&
           (Nonzero(context.source_request_id) || context.source_revision == 0);
}
bool VoiceRecording::Begin(uint32_t press, const VoiceContext& context, uint64_t captured_unix_ms,
                           const VoiceCapture* dictation) {
    if (press == 0 || !ValidContext(context) || captured_unix_ms > 253402300799999ULL)
        return false;
    if (dictation && (!dictation->IsDictation() || !Nonzero(dictation->request_id) ||
                      !Nonzero(dictation->dictation_session_id) || dictation->chunk_sequence >= 60))
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (capacity_ == 0 || press <= last_press_ || order_ == std::numeric_limits<uint64_t>::max())
        return false;
    for (const auto& buffer : buffers_) {
        if (buffer.state == State::Recording ||
            (buffer.state != State::Empty && buffer.press == press))
            return false;
    }
    for (auto& buffer : buffers_) {
        if (buffer.state != State::Empty)
            continue;
        buffer.state = State::Recording;
        buffer.press = press;
        buffer.capture = dictation ? *dictation : VoiceCapture{};
        buffer.capture.conversation_id = context.conversation_id;
        buffer.capture.source_request_id = context.source_request_id;
        buffer.capture.source_revision = context.source_revision;
        buffer.capture.captured_unix_ms = captured_unix_ms;
        buffer.samples = 0;
        buffer.failed = false;
        buffer.capped = false;
        buffer.levels = {};
        buffer.order = ++order_;
        last_press_ = press;
        return true;
    }
    return false;
}
bool VoiceRecording::Append(uint32_t press, const int16_t* pcm, size_t frames, size_t channels) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& buffer : buffers_) {
        if (buffer.state != State::Recording || buffer.press != press)
            continue;
        if (buffer.failed)
            return false;
        if (pcm == nullptr || frames == 0 || frames > kMaxInputSamples ||
            (channels != 1 && channels != 2) ||
            (!buffer.capture.IsDictation() && frames > capacity_ - buffer.samples)) {
            buffer.failed = true;
            return false;
        }
        // Stereo codecs use the microphone on the left, matching AudioService's
        // existing local testing path. Never include the playback reference.
        if (buffer.capture.IsDictation())
            frames = std::min(frames, capacity_ - buffer.samples);
        for (size_t i = 0; i < frames; ++i) {
            const int16_t sample = pcm[i * channels];
            buffer.pcm[buffer.samples + i] = sample;
            const auto magnitude = static_cast<uint16_t>(std::abs(static_cast<int32_t>(sample)));
            buffer.levels.peak = std::max(buffer.levels.peak, magnitude);
            buffer.levels.sum_squares += static_cast<uint64_t>(magnitude) * magnitude;
            if (sample != 0 && buffer.levels.first_nonzero == SIZE_MAX)
                buffer.levels.first_nonzero = buffer.samples + i;
        }
        buffer.samples += frames;
        if (buffer.capture.IsDictation() && buffer.samples == capacity_) {
            buffer.capped = true;
            buffer.state = State::Released;
        }
        return true;
    }
    return false;
}
void VoiceRecording::Fail(uint32_t press) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& buffer : buffers_) {
        if (buffer.state == State::Recording && buffer.press == press)
            buffer.failed = true;
    }
}
void VoiceRecording::Release(uint32_t press) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& buffer : buffers_) {
        if (buffer.state == State::Recording && buffer.press == press)
            buffer.state = State::Released;
    }
}
bool VoiceRecording::Take(Work& work) {
    work = {};
    std::lock_guard<std::mutex> lock(mutex_);
    size_t oldest = kBufferCount;
    for (size_t i = 0; i < kBufferCount; ++i) {
        if (buffers_[i].state == State::Released &&
            (oldest == kBufferCount || buffers_[i].order < buffers_[oldest].order))
            oldest = i;
    }
    if (oldest == kBufferCount)
        return false;
    auto& buffer = buffers_[oldest];
    buffer.state = State::Processing;
    work = {oldest,     buffer.press,   buffer.capture,
            buffer.pcm, buffer.samples, buffer.failed || buffer.samples == 0};
    work.levels = buffer.levels;
    return true;
}
void VoiceRecording::Finish(const Work& work) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (work.slot < kBufferCount) {
        auto& buffer = buffers_[work.slot];
        if (buffer.state == State::Processing && buffer.press == work.press &&
            buffer.pcm == work.pcm)
            buffer.state = State::Empty;
    }
}
bool VoiceRecording::IsRecording(uint32_t press) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(buffers_.begin(), buffers_.end(), [press](const Buffer& buffer) {
        return buffer.state == State::Recording && buffer.press == press && !buffer.failed;
    });
}
bool VoiceRecording::IsCapped(uint32_t press) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(buffers_.begin(), buffers_.end(), [press](const Buffer& buffer) {
        return buffer.press == press && buffer.capped;
    });
}
bool VoiceRecording::IsIdle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::all_of(buffers_.begin(), buffers_.end(),
                       [](const Buffer& buffer) { return buffer.state == State::Empty; });
}
}  // namespace provisions
