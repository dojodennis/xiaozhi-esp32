#include <nvs.h>
#include <algorithm>
#include <array>
#include <cstring>
#include "provisions_dictation.h"
namespace provisions::dictation {
namespace {
constexpr const char* kNamespace = "orbit_dct_v1";
constexpr const char* kKey = "journal";
constexpr size_t kHeaderBytes = 72, kSegmentBytes = 24;
constexpr size_t kMaximumBytes = kHeaderBytes + kMaximumSegments * kSegmentBytes;
constexpr char kMagic[] = "ORDICT01";
using Bytes = std::array<uint8_t, kMaximumBytes>;
void Put(uint8_t* out, uint64_t value, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i)
        out[i] = static_cast<uint8_t>(value >> (8 * i));
}
uint64_t Get(const uint8_t* in, size_t bytes) {
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; ++i)
        value |= static_cast<uint64_t>(in[i]) << (8 * i);
    return value;
}
size_t Encode(const Record& record, Bytes& data) {
    if (RecordJson(record).empty())
        return 0;
    data.fill(0);
    std::memcpy(data.data(), kMagic, 8);
    data[8] = static_cast<uint8_t>(record.state);
    data[9] = static_cast<uint8_t>(record.pending);
    data[10] = (record.authorized ? 1 : 0) | (record.stop_requested ? 2 : 0);
    data[11] = record.count;
    Put(data.data() + 12, record.revision, 4);
    Put(data.data() + 16, record.control_revision, 4);
    Put(data.data() + 20, record.pending_revision, 4);
    Put(data.data() + 24, record.frozen_count, 4);
    Put(data.data() + 28, record.expires_ms, 8);
    std::copy(record.id.begin(), record.id.end(), data.begin() + 36);
    std::copy(record.conversation_id.begin(), record.conversation_id.end(), data.begin() + 52);
    for (uint32_t i = 0; i < record.count; ++i) {
        auto* segment = data.data() + kHeaderBytes + i * kSegmentBytes;
        std::copy(record.segments[i].request_id.begin(), record.segments[i].request_id.end(),
                  segment);
        Put(segment + 16, record.segments[i].samples, 4);
        segment[20] = record.segments[i].terminal ? 1 : 0;
    }
    return kHeaderBytes + record.count * kSegmentBytes;
}
bool Decode(const Bytes& data, size_t bytes, Record& record) {
    record = {};
    if (bytes < kHeaderBytes || std::memcmp(data.data(), kMagic, 8) != 0 || data[10] > 3 ||
        data[11] > kMaximumSegments || bytes != kHeaderBytes + data[11] * kSegmentBytes ||
        data[68] || data[69] || data[70] || data[71])
        return false;
    record.state = static_cast<State>(data[8]);
    record.pending = static_cast<Action>(data[9]);
    record.authorized = data[10] & 1;
    record.stop_requested = data[10] & 2;
    record.count = data[11];
    record.revision = Get(data.data() + 12, 4);
    record.control_revision = Get(data.data() + 16, 4);
    record.pending_revision = Get(data.data() + 20, 4);
    record.frozen_count = Get(data.data() + 24, 4);
    const uint64_t expires = Get(data.data() + 28, 8);
    if (expires > 253402300799999ULL)
        return false;
    record.expires_ms = expires;
    std::copy_n(data.begin() + 36, 16, record.id.begin());
    std::copy_n(data.begin() + 52, 16, record.conversation_id.begin());
    for (uint32_t i = 0; i < record.count; ++i) {
        const auto* segment = data.data() + kHeaderBytes + i * kSegmentBytes;
        if (segment[20] > 1 || segment[21] || segment[22] || segment[23])
            return false;
        std::copy_n(segment, 16, record.segments[i].request_id.begin());
        record.segments[i].samples = Get(segment + 16, 4);
        record.segments[i].terminal = segment[20];
    }
    return !RecordJson(record).empty();
}
}  // namespace
Store::LoadResult NvsStore::Load(Record& record) {
    record = {};
    nvs_handle_t handle = 0;
    const auto opened = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (opened == ESP_ERR_NVS_NOT_FOUND)
        return LoadResult::Empty;
    if (opened != ESP_OK)
        return LoadResult::Fault;
    size_t bytes = 0;
    auto status = nvs_get_blob(handle, kKey, nullptr, &bytes);
    if (status == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return LoadResult::Empty;
    }
    if (status != ESP_OK || bytes < kHeaderBytes || bytes > kMaximumBytes) {
        nvs_close(handle);
        return LoadResult::Fault;
    }
    Bytes data{};
    const size_t expected = bytes;
    status = nvs_get_blob(handle, kKey, data.data(), &bytes);
    nvs_close(handle);
    return status == ESP_OK && bytes == expected && Decode(data, bytes, record)
               ? LoadResult::Present
               : LoadResult::Fault;
}
bool NvsStore::Save(const Record& record) {
    Bytes data{};
    const size_t bytes = Encode(record, data);
    if (!bytes)
        return false;
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK)
        return false;
    auto status = nvs_set_blob(handle, kKey, data.data(), bytes);
    if (status == ESP_OK)
        status = nvs_commit(handle);
    nvs_close(handle);
    Record verified;
    return status == ESP_OK && Load(verified) == LoadResult::Present &&
           RecordJson(verified) == RecordJson(record);
}
namespace {
bool EraseKey(const char* key) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK)
        return false;
    auto status = nvs_erase_key(handle, key);
    if (status == ESP_OK || status == ESP_ERR_NVS_NOT_FOUND)
        status = nvs_commit(handle);
    nvs_close(handle);
    return status == ESP_OK;
}
}  // namespace
bool NvsStore::Clear() {
    if (!EraseKey(kKey))
        return false;
    Record verified;
    return Load(verified) == LoadResult::Empty;
}
Store::LoadResult NvsStore::LoadDiscard(VoiceId& id, uint8_t& slots) {
    id = {};
    slots = 0;
    nvs_handle_t handle = 0;
    auto status = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (status == ESP_ERR_NVS_NOT_FOUND)
        return LoadResult::Empty;
    if (status != ESP_OK)
        return LoadResult::Fault;
    // Versioned, bounded intent. Missing on older firmware means no approval.
    std::array<uint8_t, 21> bytes{};
    size_t size = bytes.size();
    status = nvs_get_blob(handle, "discard_v1", bytes.data(), &size);
    nvs_close(handle);
    if (status == ESP_ERR_NVS_NOT_FOUND)
        return LoadResult::Empty;
    if (status != ESP_OK || size != bytes.size() || std::memcmp(bytes.data(), "ODI1", 4) != 0 ||
        bytes[20] > 15 ||
        std::all_of(bytes.begin() + 4, bytes.begin() + 20, [](uint8_t b) { return b == 0; }))
        return LoadResult::Fault;
    std::copy_n(bytes.begin() + 4, 16, id.begin());
    slots = bytes[20];
    return LoadResult::Present;
}
bool NvsStore::SaveDiscard(const VoiceId& id, uint8_t slots) {
    if (slots > 15 || std::all_of(id.begin(), id.end(), [](uint8_t b) { return b == 0; }))
        return false;
    std::array<uint8_t, 21> bytes{};
    std::memcpy(bytes.data(), "ODI1", 4);
    std::copy(id.begin(), id.end(), bytes.begin() + 4);
    bytes[20] = slots;
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK)
        return false;
    auto status = nvs_set_blob(handle, "discard_v1", bytes.data(), bytes.size());
    if (status == ESP_OK)
        status = nvs_commit(handle);
    nvs_close(handle);
    VoiceId verified;
    uint8_t verified_slots = 0;
    return status == ESP_OK && LoadDiscard(verified, verified_slots) == LoadResult::Present &&
           verified == id && verified_slots == slots;
}
bool NvsStore::ClearDiscard() {
    if (!EraseKey("discard_v1"))
        return false;
    VoiceId id;
    uint8_t slots;
    return LoadDiscard(id, slots) == LoadResult::Empty;
}
}  // namespace provisions::dictation
