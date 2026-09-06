#ifndef PROVISIONS_VOICE_OUTBOX_H
#define PROVISIONS_VOICE_OUTBOX_H

#include <array>
#include <cstddef>
#include <cstdint>

// A bounded encrypted journal. Its owner runs flash work off the audio/main tasks
// and supplies buffers once; this class never allocates or silently evicts audio.
namespace provisions {
using VoiceId = std::array<uint8_t, 16>;
struct VoiceBytes {
    const uint8_t* data;
    size_t size;
};
enum class VoicePurpose : uint32_t { Command = 0, Dictation = 1 };
struct VoiceCapture {
    VoiceId request_id{};
    VoiceId conversation_id{};
    uint64_t captured_unix_ms = 0;  // Zero means the device had no trusted clock.
    uint32_t packet_count = 0;
    VoiceId source_request_id{};  // All-zero means no prior presented answer.
    uint32_t source_revision = 0;
    VoicePurpose purpose = VoicePurpose::Command;
    VoiceId dictation_session_id{};
    uint32_t chunk_sequence = 0;
    uint32_t sample_count = 0;  // Actual microphone samples; excludes encoder padding.
    bool IsDictation() const { return purpose == VoicePurpose::Dictation; }
};
struct SavedVoiceCapture {
    VoiceCapture capture;
    uint64_t sequence = 0;
    size_t slot = 0;
    VoiceBytes frames{};  // Valid until the next journal call.
};
enum class VoiceStoreResult { Ok, Empty, Full, Invalid, Corrupt, IoError, CryptoError, Conflict };

class VoiceFlash {
public:
    virtual ~VoiceFlash() = default;
    virtual bool Read(size_t offset, void* output, size_t size) = 0;
    virtual bool Write(size_t offset, const void* input, size_t size) = 0;
    virtual bool Erase(size_t offset, size_t size) = 0;
};
class VoiceCipher {
public:
    virtual ~VoiceCipher() = default;
    virtual bool NextNonce(uint8_t nonce[12]) = 0;
    virtual bool Seal(const uint8_t nonce[12], VoiceBytes aad, VoiceBytes plain, uint8_t* cipher,
                      uint8_t tag[16]) = 0;
    virtual bool Open(const uint8_t nonce[12], VoiceBytes aad, VoiceBytes cipher,
                      const uint8_t tag[16], uint8_t* plain) = 0;
};

class VoiceOutbox {
public:
    static constexpr size_t kSlots = 4;
    static constexpr size_t kSlotBytes = 512 * 1024;
    static constexpr size_t kStoreBytes = kSlots * kSlotBytes;
    static constexpr size_t kBodyOffset = 4096;
    static constexpr size_t kMaxPackets = 167;
    static constexpr size_t kMaxPacketBytes = 2048;
    static constexpr size_t kMaxFrameBytes = kMaxPackets * (2 + kMaxPacketBytes);
    static constexpr size_t kHeaderBytes = 124;
    static constexpr size_t kDictationHeaderBytes = 152;

    VoiceOutbox(VoiceFlash& flash, VoiceCipher& cipher, uint8_t* cipher_buffer,
                uint8_t* plain_buffer, size_t buffer_bytes);
    VoiceStoreResult Read(size_t slot, SavedVoiceCapture& output);
    // Frames are LE uint16 length + Opus bytes. Input must not alias scratch.
    VoiceStoreResult Save(const VoiceCapture& capture, VoiceBytes frames,
                          SavedVoiceCapture& output);
    // The transport owner must first validate a durable server receipt for these
    // exact IDs. A queued receipt can never erase a reused slot or another scope.
    VoiceStoreResult RemoveAfterReceipt(size_t slot, const VoiceId& request_id,
                                        const VoiceId& conversation_id);
    static bool ValidFrames(VoiceBytes frames, uint32_t packet_count);

private:
    VoiceFlash& flash_;
    VoiceCipher& cipher_;
    uint8_t* cipher_buffer_;
    uint8_t* plain_buffer_;
    size_t buffer_bytes_;
    bool BuffersReady() const;
};
}  // namespace provisions
#endif
