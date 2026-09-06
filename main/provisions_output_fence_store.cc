#include "provisions_output_fence.h"

#include <nvs.h>
#include <algorithm>

namespace provisions::output_fence {
namespace {
constexpr const char* kNamespace = "orbit_of_v1";
constexpr const char* kKey = "owner";
constexpr uint8_t kMagic[] = {'O', 'R', 'F', 'E', 'N', 'C', '0', '1'};
void Put(Manifest& bytes, size_t offset, uint64_t value, size_t count) {
    for (size_t i = 0; i < count; ++i)
        bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
uint64_t Get(const Manifest& bytes, size_t offset, size_t count) {
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i)
        value |= static_cast<uint64_t>(bytes[offset + i]) << (8 * i);
    return value;
}
unsigned Nibble(char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }
void Pack(Manifest& bytes, size_t offset, std::string_view text) {
    bool high = true;
    for (char c : text) {
        if (c == '-')
            continue;
        if (high)
            bytes[offset] = Nibble(c) << 4;
        else
            bytes[offset++] |= Nibble(c);
        high = !high;
    }
}
std::string Unpack(const Manifest& bytes, size_t offset, size_t count, bool uuid) {
    constexpr char hex[] = "0123456789abcdef";
    std::string text;
    for (size_t i = 0; i < count; ++i) {
        if (uuid && (i == 4 || i == 6 || i == 8 || i == 10))
            text += '-';
        text += hex[bytes[offset + i] >> 4];
        text += hex[bytes[offset + i] & 15];
    }
    return text;
}
uint32_t Crc(const Manifest& bytes) {
    uint32_t crc = 0xffffffff;
    for (size_t i = 0; i < 220; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
    }
    return ~crc;
}
bool ValidRecord(const Record& record) {
    if (!ValidIdentity(record.identity))
        return false;
    if (record.phase == Phase::Owned)
        return SameReceipt(record.receipt, {}) && record.backend_commit_id.empty();
    if (!ValidReceipt(record.receipt))
        return false;
    if (record.phase == Phase::DrainedPendingCommit)
        return record.backend_commit_id.empty();
    return record.phase == Phase::Terminal && ValidId(record.backend_commit_id);
}
bool Transition(const Record& before, const Record& after) {
    if (SameIdentity(before.identity, after.identity)) {
        if (before.phase == after.phase)
            return SameReceipt(before.receipt, after.receipt) &&
                   before.backend_commit_id == after.backend_commit_id;
        return (before.phase == Phase::Owned && after.phase == Phase::DrainedPendingCommit) ||
               (before.phase == Phase::DrainedPendingCommit && after.phase == Phase::Terminal &&
                SameReceipt(before.receipt, after.receipt));
    }
    const auto& old = before.identity;
    const auto& next = after.identity;
    return before.phase == Phase::Terminal && after.phase == Phase::Owned &&
           old.device_id == next.device_id && next.fence_epoch > old.fence_epoch &&
           next.checkpoint_sha256 != old.checkpoint_sha256 && next.playback_id != old.playback_id &&
           (next.lease_id != old.lease_id || next.sequence > old.sequence);
}
}  // namespace
bool EncodeRecord(const Record& record, Manifest& bytes) {
    if (!ValidRecord(record))
        return false;
    bytes.fill(0);
    std::copy(std::begin(kMagic), std::end(kMagic), bytes.begin());
    bytes[8] = static_cast<uint8_t>(record.phase);
    bytes[9] = record.receipt.completed;
    bytes[10] = record.receipt.has_started;
    const auto& id = record.identity;
    Put(bytes, 12, id.fence_epoch, 8);
    Put(bytes, 20, id.sequence, 8);
    Put(bytes, 28, id.response_revision, 4);
    Pack(bytes, 32, id.device_id);
    Pack(bytes, 48, id.lease_id);
    Pack(bytes, 64, id.playback_id);
    Pack(bytes, 80, id.request_id);
    Pack(bytes, 96, id.route_epoch);
    Pack(bytes, 112, id.device_connection_id);
    Pack(bytes, 128, id.checkpoint_sha256);
    Put(bytes, 160, record.receipt.started_at_ms, 8);
    Put(bytes, 168, record.receipt.stopped_at_ms, 8);
    Put(bytes, 176, record.receipt.drained_at_ms, 8);
    if (!record.backend_commit_id.empty())
        Pack(bytes, 184, record.backend_commit_id);
    Put(bytes, 220, Crc(bytes), 4);
    return true;
}
bool DecodeRecord(const Manifest& bytes, Record& output) {
    if (!std::equal(std::begin(kMagic), std::end(kMagic), bytes.begin()) || bytes[9] > 1 ||
        bytes[10] > 1 || bytes[11] != 0 || Get(bytes, 220, 4) != Crc(bytes) ||
        std::any_of(bytes.begin() + 200, bytes.begin() + 220,
                    [](uint8_t byte) { return byte != 0; }))
        return false;
    Record record;
    record.phase = static_cast<Phase>(bytes[8]);
    record.receipt.completed = bytes[9];
    record.receipt.has_started = bytes[10];
    auto& id = record.identity;
    id.fence_epoch = Get(bytes, 12, 8);
    id.sequence = Get(bytes, 20, 8);
    id.response_revision = Get(bytes, 28, 4);
    id.device_id = Unpack(bytes, 32, 16, true);
    id.lease_id = Unpack(bytes, 48, 16, true);
    id.playback_id = Unpack(bytes, 64, 16, true);
    id.request_id = Unpack(bytes, 80, 16, true);
    id.route_epoch = Unpack(bytes, 96, 16, true);
    id.device_connection_id = Unpack(bytes, 112, 16, true);
    id.checkpoint_sha256 = Unpack(bytes, 128, 32, false);
    record.receipt.started_at_ms = Get(bytes, 160, 8);
    record.receipt.stopped_at_ms = Get(bytes, 168, 8);
    record.receipt.drained_at_ms = Get(bytes, 176, 8);
    if (std::any_of(bytes.begin() + 184, bytes.begin() + 200,
                    [](uint8_t byte) { return byte != 0; }))
        record.backend_commit_id = Unpack(bytes, 184, 16, true);
    if (!ValidRecord(record))
        return false;
    output = std::move(record);
    return true;
}
Store::LoadResult NvsStore::Load(Record& record) {
    nvs_handle_t handle;
    const auto opened = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (opened == ESP_ERR_NVS_NOT_FOUND)
        return LoadResult::Missing;
    if (opened != ESP_OK)
        return LoadResult::Fault;
    size_t size = 0;
    auto status = nvs_get_blob(handle, kKey, nullptr, &size);
    if (status == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return LoadResult::Missing;
    }
    Manifest bytes;
    if (status != ESP_OK || size != bytes.size()) {
        nvs_close(handle);
        return LoadResult::Fault;
    }
    status = nvs_get_blob(handle, kKey, bytes.data(), &size);
    nvs_close(handle);
    return status == ESP_OK && size == bytes.size() && DecodeRecord(bytes, record)
               ? LoadResult::Present
               : LoadResult::Fault;
}
bool NvsStore::CompareExchange(const Record& expected, const Record& desired) {
    static std::mutex namespace_mutex;
    std::lock_guard<std::mutex> lock(namespace_mutex);
    Manifest before, after, actual;
    Record prior;
    if (!Transition(expected, desired) || !EncodeRecord(expected, before) ||
        !EncodeRecord(desired, after) || Load(prior) != LoadResult::Present ||
        !EncodeRecord(prior, actual) || actual != before)
        return false;
    if (before == after)
        return true;
    // Serialize compare/write/readback across all instances of this namespace adapter.
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK)
        return false;
    const bool written = nvs_set_blob(handle, kKey, after.data(), after.size()) == ESP_OK &&
                         nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return written && Load(prior) == LoadResult::Present && EncodeRecord(prior, actual) &&
           actual == after;
}
}  // namespace provisions::output_fence
