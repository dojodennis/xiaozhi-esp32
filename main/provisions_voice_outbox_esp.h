#ifndef PROVISIONS_VOICE_OUTBOX_ESP_H
#define PROVISIONS_VOICE_OUTBOX_ESP_H

#include <esp_partition.h>
#include <memory>
#include "provisions_voice_outbox.h"

namespace provisions {
// Own on the outbox worker only. Initialize performs bounded flash/NVS I/O;
// failures leave prior recordings and all existing asset bytes untouched.
class EspVoiceOutbox final : private VoiceFlash, private VoiceCipher {
public:
    EspVoiceOutbox() = default;
    ~EspVoiceOutbox();
    EspVoiceOutbox(const EspVoiceOutbox&) = delete;
    EspVoiceOutbox& operator=(const EspVoiceOutbox&) = delete;
    // Existing keys open offline. First creation requires an authenticated
    // context after radio setup, which also supplies hardware RNG entropy.
    bool Initialize(bool allow_key_creation = false);
    // Consume the same durable nonce counter for an independent opaque request
    // ID. This remains unique across offline boots and emptied journals.
    bool NewRequestId(VoiceId& output);
    VoiceOutbox* journal() { return journal_.get(); }
    static constexpr size_t kAssetLimit = 6 * 1024 * 1024;

private:
    const esp_partition_t* partition_ = nullptr;
    std::array<uint8_t, 32> key_{};
    uint8_t* cipher_buffer_ = nullptr;
    uint8_t* plain_buffer_ = nullptr;
    std::unique_ptr<VoiceOutbox> journal_;
    bool CheckAssetBoundary();
    bool LoadKey(bool allow_key_creation);
    bool RegionIsErased();
    bool Read(size_t offset, void* output, size_t size) override;
    bool Write(size_t offset, const void* input, size_t size) override;
    bool Erase(size_t offset, size_t size) override;
    bool NextNonce(uint8_t nonce[12]) override;
    bool Seal(const uint8_t nonce[12], VoiceBytes aad, VoiceBytes plain, uint8_t* cipher,
              uint8_t tag[16]) override;
    bool Open(const uint8_t nonce[12], VoiceBytes aad, VoiceBytes cipher, const uint8_t tag[16],
              uint8_t* plain) override;
};
}  // namespace provisions
#endif
