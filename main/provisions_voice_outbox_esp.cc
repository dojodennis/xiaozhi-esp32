#include "provisions_voice_outbox_esp.h"

#include <esp_heap_caps.h>
#include <esp_random.h>
#include <mbedtls/platform_util.h>
#include <nvs.h>
#include <psa/crypto.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace provisions {
namespace {
uint32_t Read32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
bool InRegion(size_t offset, size_t size) {
    return offset <= VoiceOutbox::kStoreBytes && size <= VoiceOutbox::kStoreBytes - offset;
}
// PSA multipart GCM keeps the tag separate from the journal payload and bounds
// temporary storage independently of recording length. No unauthenticated output
// escapes this call on decryption failure.
bool CryptAudio(const uint8_t key[32], const uint8_t nonce[12], VoiceBytes aad, VoiceBytes input,
                uint8_t* output, uint8_t* tag_out, const uint8_t* tag_in) {
    if (psa_crypto_init() != PSA_SUCCESS)
        return false;
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 256);
    psa_key_id_t key_id = PSA_KEY_ID_NULL;
    auto status = psa_import_key(&attributes, key, 32, &key_id);
    psa_reset_key_attributes(&attributes);
    psa_aead_operation_t operation = PSA_AEAD_OPERATION_INIT;
    const bool encrypt = tag_out != nullptr;
    if (status == PSA_SUCCESS)
        status = encrypt ? psa_aead_encrypt_setup(&operation, key_id, PSA_ALG_GCM)
                         : psa_aead_decrypt_setup(&operation, key_id, PSA_ALG_GCM);
    if (status == PSA_SUCCESS)
        status = psa_aead_set_lengths(&operation, aad.size, input.size);
    if (status == PSA_SUCCESS)
        status = psa_aead_set_nonce(&operation, nonce, 12);
    if (status == PSA_SUCCESS)
        status = psa_aead_update_ad(&operation, aad.data, aad.size);
    constexpr size_t kChunk = 1024;
    std::array<uint8_t, kChunk + 16> chunk{};
    static_assert(PSA_AEAD_UPDATE_OUTPUT_SIZE(PSA_KEY_TYPE_AES, PSA_ALG_GCM, kChunk) <=
                  kChunk + 16);
    size_t consumed = 0, produced = 0;
    while (status == PSA_SUCCESS && consumed < input.size) {
        const size_t count = std::min(kChunk, input.size - consumed);
        size_t written = 0;
        status = psa_aead_update(&operation, input.data + consumed, count, chunk.data(),
                                 chunk.size(), &written);
        if (status == PSA_SUCCESS && written <= input.size - produced) {
            std::memcpy(output + produced, chunk.data(), written);
            produced += written;
            consumed += count;
        } else {
            status = PSA_ERROR_GENERIC_ERROR;
        }
    }
    size_t final_bytes = 0, tag_bytes = 0;
    if (status == PSA_SUCCESS) {
        status = encrypt ? psa_aead_finish(&operation, chunk.data(), chunk.size(), &final_bytes,
                                           tag_out, 16, &tag_bytes)
                         : psa_aead_verify(&operation, chunk.data(), chunk.size(), &final_bytes,
                                           tag_in, 16);
        if (status == PSA_SUCCESS && final_bytes <= input.size - produced) {
            std::memcpy(output + produced, chunk.data(), final_bytes);
            produced += final_bytes;
        } else {
            status = PSA_ERROR_GENERIC_ERROR;
        }
    }
    psa_aead_abort(&operation);
    const bool destroyed = key_id == PSA_KEY_ID_NULL || psa_destroy_key(key_id) == PSA_SUCCESS;
    const bool ok = status == PSA_SUCCESS && destroyed && produced == input.size &&
                    (!encrypt || tag_bytes == 16);
    mbedtls_platform_zeroize(chunk.data(), chunk.size());
    if (!ok)
        mbedtls_platform_zeroize(output, input.size);
    return ok;
}
}  // namespace

EspVoiceOutbox::~EspVoiceOutbox() {
    journal_.reset();
    mbedtls_platform_zeroize(key_.data(), key_.size());
    if (plain_buffer_)
        mbedtls_platform_zeroize(plain_buffer_, VoiceOutbox::kMaxFrameBytes);
    if (cipher_buffer_)
        mbedtls_platform_zeroize(cipher_buffer_, VoiceOutbox::kMaxFrameBytes);
    heap_caps_free(plain_buffer_);
    heap_caps_free(cipher_buffer_);
}

bool EspVoiceOutbox::Initialize(bool allow_key_creation) {
    if (journal_)
        return true;
    partition_ = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                          ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "assets");
    if (!partition_ || partition_->address != 0x800000 ||
        partition_->size != kAssetLimit + VoiceOutbox::kStoreBytes || !CheckAssetBoundary())
        return false;
    // Allocate once, explicitly in PSRAM. Do not consume the microphone's scarce
    // internal SRAM, and do not turn allocation/NVS failure into a device reset.
    if (!cipher_buffer_)
        cipher_buffer_ = static_cast<uint8_t*>(
            heap_caps_malloc(VoiceOutbox::kMaxFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!plain_buffer_)
        plain_buffer_ = static_cast<uint8_t*>(
            heap_caps_malloc(VoiceOutbox::kMaxFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!cipher_buffer_ || !plain_buffer_ || !LoadKey(allow_key_creation))
        return false;
    journal_.reset(new (std::nothrow) VoiceOutbox(*this, *this, cipher_buffer_, plain_buffer_,
                                                  VoiceOutbox::kMaxFrameBytes));
    return journal_ != nullptr;
}

bool EspVoiceOutbox::CheckAssetBoundary() {
    std::array<uint8_t, 12> header;
    if (esp_partition_read(partition_, 0, header.data(), header.size()) != ESP_OK)
        return false;
    const uint32_t files = Read32(header.data()), bytes = Read32(header.data() + 8);
    constexpr size_t kEntryBytes = 44;
    if (files == 0 || files > 1024 || bytes < files * kEntryBytes ||
        bytes > kAssetLimit - header.size())
        return false;
    const size_t body_start = header.size() + files * kEntryBytes;
    for (uint32_t i = 0; i < files; ++i) {
        std::array<uint8_t, kEntryBytes> entry;
        if (esp_partition_read(partition_, header.size() + i * kEntryBytes, entry.data(),
                               entry.size()) != ESP_OK)
            return false;
        const uint32_t size = Read32(entry.data() + 32), offset = Read32(entry.data() + 36);
        const uint64_t end = uint64_t(body_start) + offset + 2 + size;
        if (!std::memchr(entry.data(), 0, 32) || end > uint64_t(header.size()) + bytes)
            return false;
    }
    return true;
}

bool EspVoiceOutbox::RegionIsErased() {
    std::array<uint8_t, 4096> block;
    for (size_t offset = 0; offset < VoiceOutbox::kStoreBytes; offset += block.size()) {
        if (!Read(offset, block.data(), block.size()) ||
            !std::all_of(block.begin(), block.end(), [](uint8_t byte) { return byte == 255; }))
            return false;
    }
    return true;
}

bool EspVoiceOutbox::LoadKey(bool allow_key_creation) {
    // This independent audio key survives ordinary device-credential rotation.
    // Never regenerate a missing key over existing ciphertext or erase that data.
    nvs_handle_t handle = 0;
    if (nvs_open("orbit_audio", NVS_READWRITE, &handle) != ESP_OK)
        return false;
    size_t size = key_.size();
    auto result = nvs_get_blob(handle, "key_v1", key_.data(), &size);
    if (result == ESP_ERR_NVS_NOT_FOUND && allow_key_creation && RegionIsErased()) {
        esp_fill_random(key_.data(), key_.size());
        result = nvs_set_blob(handle, "key_v1", key_.data(), key_.size());
        if (result == ESP_OK)
            result = nvs_set_u64(handle, "nonce_v1", 0);
        if (result == ESP_OK)
            result = nvs_commit(handle);
    }
    nvs_close(handle);
    if (result != ESP_OK || size != key_.size())
        return false;
    // Confirm committed storage through a new handle before any recording write.
    if (nvs_open("orbit_audio", NVS_READONLY, &handle) != ESP_OK)
        return false;
    std::array<uint8_t, 32> verified{};
    size = verified.size();
    result = nvs_get_blob(handle, "key_v1", verified.data(), &size);
    nvs_close(handle);
    const bool valid =
        result == ESP_OK && size == key_.size() && verified == key_ &&
        std::any_of(key_.begin(), key_.end(), [](uint8_t value) { return value != 0; });
    mbedtls_platform_zeroize(verified.data(), verified.size());
    return valid;
}

bool EspVoiceOutbox::Read(size_t offset, void* output, size_t size) {
    return partition_ && InRegion(offset, size) &&
           esp_partition_read(partition_, kAssetLimit + offset, output, size) == ESP_OK;
}
bool EspVoiceOutbox::Write(size_t offset, const void* input, size_t size) {
    return partition_ && InRegion(offset, size) &&
           esp_partition_write(partition_, kAssetLimit + offset, input, size) == ESP_OK;
}
bool EspVoiceOutbox::Erase(size_t offset, size_t size) {
    return partition_ && InRegion(offset, size) && offset % 4096 == 0 && size % 4096 == 0 &&
           esp_partition_erase_range(partition_, kAssetLimit + offset, size) == ESP_OK;
}
bool EspVoiceOutbox::NewRequestId(VoiceId& output) {
    output = {};
    if (journal_ == nullptr)
        return false;
    // A fresh AES-GCM counter value gives an opaque, independent UUID without
    // requiring radio entropy on an offline boot. This nonce is never reused
    // for the recording; Save consumes its own next counter value.
    constexpr uint8_t domain[16] = {'O', 'r', 'b', 'i', 't', ' ', 'r', 'e',
                                    'q', 'u', 'e', 's', 't', ' ', 'v', '1'};
    uint8_t nonce[12]{};
    uint8_t tag[16]{};
    const uint8_t plain[16]{};
    if (!NextNonce(nonce) ||
        !Seal(nonce, {domain, sizeof(domain)}, {plain, sizeof(plain)}, output.data(), tag)) {
        output = {};
        return false;
    }
    output[6] = (output[6] & 0x0f) | 0x40;
    output[8] = (output[8] & 0x3f) | 0x80;
    return true;
}

bool EspVoiceOutbox::NextNonce(uint8_t nonce[12]) {
    // Uniqueness does not depend on radio entropy during an offline cold boot.
    // A committed counter may be skipped on failure, but is never reused.
    nvs_handle_t handle = 0;
    if (nvs_open("orbit_audio", NVS_READWRITE, &handle) != ESP_OK)
        return false;
    uint64_t counter = 0;
    auto result = nvs_get_u64(handle, "nonce_v1", &counter);
    if (result == ESP_OK && counter != std::numeric_limits<uint64_t>::max()) {
        ++counter;
        result = nvs_set_u64(handle, "nonce_v1", counter);
        if (result == ESP_OK)
            result = nvs_commit(handle);
    } else {
        nvs_close(handle);
        return false;
    }
    nvs_close(handle);
    if (result != ESP_OK || nvs_open("orbit_audio", NVS_READONLY, &handle) != ESP_OK)
        return false;
    uint64_t verified = 0;
    result = nvs_get_u64(handle, "nonce_v1", &verified);
    nvs_close(handle);
    if (result != ESP_OK || verified != counter)
        return false;
    std::memcpy(nonce, "ORB1", 4);
    for (size_t i = 0; i < 8; ++i)
        nonce[4 + i] = (counter >> (i * 8)) & 255;
    return true;
}
bool EspVoiceOutbox::Seal(const uint8_t nonce[12], VoiceBytes aad, VoiceBytes plain,
                          uint8_t* cipher, uint8_t tag[16]) {
    return CryptAudio(key_.data(), nonce, aad, plain, cipher, tag, nullptr);
}
bool EspVoiceOutbox::Open(const uint8_t nonce[12], VoiceBytes aad, VoiceBytes cipher,
                          const uint8_t tag[16], uint8_t* plain) {
    return CryptAudio(key_.data(), nonce, aad, cipher, plain, nullptr, tag);
}
}  // namespace provisions
