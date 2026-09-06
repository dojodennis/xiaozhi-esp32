#ifndef PROVISIONS_VOICE_RECORDING_H
#define PROVISIONS_VOICE_RECORDING_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include "provisions_voice_outbox.h"

namespace provisions {
struct VoiceContext {
    VoiceId conversation_id{};
    VoiceId source_request_id{};
    uint32_t source_revision = 0;
    bool operator==(const VoiceContext& other) const {
        return conversation_id == other.conversation_id &&
               source_request_id == other.source_request_id &&
               source_revision == other.source_revision;
    }
};

// Owns no allocation or I/O. The audio task appends a bounded 10 ms microphone
// chunk; a separate worker owns a released buffer until Finish. No socket,
// flash operation or Opus encoder runs under this mutex.
class VoiceRecording {
public:
    static constexpr size_t kMaxSamples = 160000;
    static constexpr size_t kBufferCount = 2;
    static constexpr size_t kMaxInputSamples = 160;
    enum class State { Empty, Recording, Released, Processing };
    struct Work {
        size_t slot = kBufferCount;
        uint32_t press = 0;
        VoiceCapture capture{};
        const int16_t* pcm = nullptr;
        size_t samples = 0;
        bool failed = false;
    };

    VoiceRecording(int16_t* first, int16_t* second, size_t capacity);
    bool Begin(uint32_t press, const VoiceContext& context, uint64_t captured_unix_ms);
    bool Append(uint32_t press, const int16_t* pcm, size_t frames, size_t channels);
    void Fail(uint32_t press);
    void Release(uint32_t press);
    bool Take(Work& work);
    // The worker must clear its PCM before Finish; the slot cannot be reused
    // while that clear, encoding, or persistence is in progress.
    void Finish(const Work& work);
    bool IsRecording(uint32_t press) const;
    static bool ValidContext(const VoiceContext& context);

private:
    struct Buffer {
        int16_t* pcm = nullptr;
        State state = State::Empty;
        uint32_t press = 0;
        VoiceCapture capture{};
        size_t samples = 0;
        bool failed = false;
        uint64_t order = 0;
    };
    mutable std::mutex mutex_;
    std::array<Buffer, kBufferCount> buffers_{};
    size_t capacity_ = 0;
    uint64_t order_ = 0;
    uint32_t last_press_ = 0;
};
}  // namespace provisions
#endif
