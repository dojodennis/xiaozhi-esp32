#include "provisions_voice_outbox.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace provisions {
namespace {
constexpr uint8_t kMagic[8] = {'O', 'R', 'B', 'A', 'U', 'D', '0', '2'};
constexpr size_t kAuthenticatedBytes = 108;
uint64_t Get(const uint8_t* p, size_t bytes) {
    uint64_t result = 0;
    for (size_t i = 0; i < bytes; ++i)
        result |= uint64_t(p[i]) << (8 * i);
    return result;
}
void Put(uint8_t* p, uint64_t value, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i)
        p[i] = (value >> (8 * i)) & 255;
}
bool Nonzero(const uint8_t* p, size_t size) {
    return std::any_of(p, p + size, [](uint8_t value) { return value != 0; });
}
bool ValidMetadata(const VoiceCapture& capture) {
    return Nonzero(capture.request_id.data(), 16) && Nonzero(capture.conversation_id.data(), 16) &&
           capture.captured_unix_ms <= 253402300799999ULL && capture.source_revision <= 999999999 &&
           (Nonzero(capture.source_request_id.data(), 16) || capture.source_revision == 0);
}
bool Same(const VoiceCapture& a, const VoiceCapture& b) {
    return a.request_id == b.request_id && a.conversation_id == b.conversation_id &&
           a.captured_unix_ms == b.captured_unix_ms && a.packet_count == b.packet_count &&
           a.source_request_id == b.source_request_id && a.source_revision == b.source_revision;
}
bool Overlaps(VoiceBytes input, const uint8_t* buffer, size_t size) {
    const auto a = reinterpret_cast<uintptr_t>(input.data);
    const auto b = reinterpret_cast<uintptr_t>(buffer);
    return a <= b ? b - a < input.size : a - b < size;
}
}  // namespace

VoiceOutbox::VoiceOutbox(VoiceFlash& flash, VoiceCipher& cipher, uint8_t* cipher_buffer,
                         uint8_t* plain_buffer, size_t buffer_bytes)
    : flash_(flash),
      cipher_(cipher),
      cipher_buffer_(cipher_buffer),
      plain_buffer_(plain_buffer),
      buffer_bytes_(buffer_bytes) {}

bool VoiceOutbox::BuffersReady() const {
    return cipher_buffer_ && plain_buffer_ && buffer_bytes_ >= kMaxFrameBytes &&
           !Overlaps({cipher_buffer_, buffer_bytes_}, plain_buffer_, buffer_bytes_);
}

bool VoiceOutbox::ValidFrames(VoiceBytes frames, uint32_t packet_count) {
    if (!frames.data || frames.size > kMaxFrameBytes || packet_count < 1 ||
        packet_count > kMaxPackets)
        return false;
    size_t offset = 0;
    for (uint32_t i = 0; i < packet_count; ++i) {
        if (frames.size - offset < 2)
            return false;
        const size_t size = Get(frames.data + offset, 2);
        offset += 2;
        if (size == 0 || size > kMaxPacketBytes || size > frames.size - offset)
            return false;
        offset += size;
    }
    return offset == frames.size;
}

VoiceStoreResult VoiceOutbox::Read(size_t slot, SavedVoiceCapture& output) {
    output = {};
    if (slot >= kSlots || !BuffersReady())
        return VoiceStoreResult::Invalid;
    std::array<uint8_t, kHeaderBytes> header;
    const size_t offset = slot * kSlotBytes;
    if (!flash_.Read(offset, header.data(), header.size()))
        return VoiceStoreResult::IoError;
    if (std::all_of(header.begin(), header.end(), [](uint8_t b) { return b == 255; }))
        return VoiceStoreResult::Empty;
    const size_t bytes = Get(header.data() + 12, 4);
    VoiceCapture capture;
    std::copy_n(header.data() + 24, 16, capture.request_id.begin());
    std::copy_n(header.data() + 40, 16, capture.conversation_id.begin());
    capture.captured_unix_ms = Get(header.data() + 56, 8);
    capture.packet_count = Get(header.data() + 64, 4);
    std::copy_n(header.data() + 76, 16, capture.source_request_id.begin());
    capture.source_revision = Get(header.data() + 92, 4);
    const uint64_t sequence = Get(header.data() + 16, 8);
    if (std::memcmp(header.data(), kMagic, 8) != 0 || Get(header.data() + 8, 4) != 2 || bytes < 3 ||
        bytes > kMaxFrameBytes || sequence == 0 || !ValidMetadata(capture) ||
        Get(header.data() + 68, 4) != 16000 || Get(header.data() + 72, 4) != 60 ||
        !Nonzero(header.data() + 96, 12))
        return VoiceStoreResult::Corrupt;
    if (!flash_.Read(offset + kBodyOffset, cipher_buffer_, bytes))
        return VoiceStoreResult::IoError;
    if (!cipher_.Open(header.data() + 96, {header.data(), kAuthenticatedBytes},
                      {cipher_buffer_, bytes}, header.data() + 108, plain_buffer_)) {
        std::memset(plain_buffer_, 0, bytes);
        return VoiceStoreResult::Corrupt;
    }
    if (!ValidFrames({plain_buffer_, bytes}, capture.packet_count)) {
        std::memset(plain_buffer_, 0, bytes);
        return VoiceStoreResult::Corrupt;
    }
    output = {capture, sequence, slot, {plain_buffer_, bytes}};
    return VoiceStoreResult::Ok;
}

VoiceStoreResult VoiceOutbox::Save(const VoiceCapture& capture, VoiceBytes frames,
                                   SavedVoiceCapture& output) {
    output = {};
    if (!BuffersReady() || !ValidMetadata(capture) || !ValidFrames(frames, capture.packet_count) ||
        Overlaps(frames, cipher_buffer_, buffer_bytes_) ||
        Overlaps(frames, plain_buffer_, buffer_bytes_))
        return VoiceStoreResult::Invalid;
    size_t available = kSlots;
    uint64_t sequence = 0;
    for (size_t slot = 0; slot < kSlots; ++slot) {
        SavedVoiceCapture saved;
        const auto result = Read(slot, saved);
        if (result == VoiceStoreResult::Empty && available == kSlots)
            available = slot;
        else if (result == VoiceStoreResult::IoError || result == VoiceStoreResult::Invalid)
            return result;
        else if (result == VoiceStoreResult::Ok) {
            sequence = std::max(sequence, saved.sequence);
            if (saved.capture.request_id == capture.request_id) {
                if (!Same(saved.capture, capture) || saved.frames.size != frames.size ||
                    std::memcmp(saved.frames.data, frames.data, frames.size) != 0)
                    return VoiceStoreResult::Conflict;
                output = saved;
                return VoiceStoreResult::Ok;
            }
        }
        // Corrupt records stay quarantined; never erase them to make room.
    }
    if (available == kSlots)
        return VoiceStoreResult::Full;
    if (sequence == std::numeric_limits<uint64_t>::max())
        return VoiceStoreResult::Invalid;
    std::array<uint8_t, kHeaderBytes> header{};
    std::copy_n(kMagic, 8, header.begin());
    Put(header.data() + 8, 2, 4);
    Put(header.data() + 12, frames.size, 4);
    Put(header.data() + 16, sequence + 1, 8);
    std::copy(capture.request_id.begin(), capture.request_id.end(), header.begin() + 24);
    std::copy(capture.conversation_id.begin(), capture.conversation_id.end(), header.begin() + 40);
    Put(header.data() + 56, capture.captured_unix_ms, 8);
    Put(header.data() + 64, capture.packet_count, 4);
    Put(header.data() + 68, 16000, 4);
    Put(header.data() + 72, 60, 4);
    std::copy(capture.source_request_id.begin(), capture.source_request_id.end(),
              header.begin() + 76);
    Put(header.data() + 92, capture.source_revision, 4);
    if (!cipher_.NextNonce(header.data() + 96) || !Nonzero(header.data() + 96, 12) ||
        !cipher_.Seal(header.data() + 96, {header.data(), kAuthenticatedBytes}, frames,
                      cipher_buffer_, header.data() + 108))
        return VoiceStoreResult::CryptoError;
    const size_t offset = available * kSlotBytes;
    if (!flash_.Erase(offset, kSlotBytes) ||
        !flash_.Write(offset + kBodyOffset, cipher_buffer_, frames.size) ||
        !flash_.Write(offset, header.data(), header.size()))
        return VoiceStoreResult::IoError;
    // Header (including authentication tag) is written last. Only authenticated
    // read-back can produce Saved; a torn erase/body/header is never a receipt.
    auto result = Read(available, output);
    if (result != VoiceStoreResult::Ok)
        return result;
    if (!Same(output.capture, capture) || output.frames.size != frames.size ||
        std::memcmp(output.frames.data, frames.data, frames.size) != 0) {
        output = {};
        return VoiceStoreResult::Corrupt;
    }
    return VoiceStoreResult::Ok;
}

VoiceStoreResult VoiceOutbox::RemoveAfterReceipt(size_t slot, const VoiceId& request_id,
                                                 const VoiceId& conversation_id) {
    SavedVoiceCapture saved;
    const auto result = Read(slot, saved);
    if (result != VoiceStoreResult::Ok)
        return result;
    if (saved.capture.request_id != request_id || saved.capture.conversation_id != conversation_id)
        return VoiceStoreResult::Conflict;
    return flash_.Erase(slot * kSlotBytes, kSlotBytes) ? VoiceStoreResult::Ok
                                                       : VoiceStoreResult::IoError;
}
}  // namespace provisions
