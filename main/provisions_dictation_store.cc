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
}  // namespace provisions::dictation
