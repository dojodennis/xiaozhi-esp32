#include "service_schedule_storage.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace orbit::service_schedule::storage {
namespace {
constexpr uint8_t kMagic[] = {'O', 'S', 'S', '1'};
constexpr uint8_t kEncodingVersion = 1;
constexpr size_t kHeaderBytes = 93;

uint32_t Crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool WithinLimits(const FacePersistentState& state) {
    const auto& s = state.schedule.snapshot;
    return s.cues.size() <= kMaximumActiveItems &&
           s.timers.size() <= kMaximumActiveItems - s.cues.size() &&
           state.schedule.items.size() <= kMaximumActiveItems &&
           state.pending.size() <= kMaximumPendingAcks &&
           state.schedule.retired_ids.size() <= kMaximumRetiredIds;
}

class Writer {
public:
    Bytes bytes;
    void Number(uint64_t value, size_t width) {
        for (size_t i = 0; i < width; ++i) {
            bytes.push_back(static_cast<uint8_t>(value));
            value >>= 8;
        }
    }
    void Text(std::string_view value, size_t width) {
        Number(value.size(), width);
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void Uuid(std::string_view value) {
        // Called only after complete FaceModel validation of canonical UUIDs.
        int high = -1;
        for (char c : value) {
            if (c == '-')
                continue;
            const int digit = c <= '9' ? c - '0' : c - 'a' + 10;
            if (high < 0)
                high = digit;
            else {
                bytes.push_back(static_cast<uint8_t>((high << 4) | digit));
                high = -1;
            }
        }
    }
};

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    uint64_t Number(size_t width) {
        if (!available(width))
            return 0;
        uint64_t result = 0;
        for (size_t i = 0; i < width; ++i)
            result |= static_cast<uint64_t>(data_[position_++]) << (i * 8);
        return result;
    }
    std::string Text(size_t width, size_t maximum) {
        const auto length = Number(width);
        if (length > maximum || !available(static_cast<size_t>(length))) {
            valid_ = false;
            return {};
        }
        const auto* begin = reinterpret_cast<const char*>(data_ + position_);
        position_ += length;
        return std::string(begin, static_cast<size_t>(length));
    }
    std::string Uuid() {
        if (!available(16))
            return {};
        constexpr char hex[] = "0123456789abcdef";
        std::string value;
        value.reserve(36);
        for (size_t i = 0; i < 16; ++i) {
            if (i == 4 || i == 6 || i == 8 || i == 10)
                value.push_back('-');
            const auto byte = data_[position_++];
            value.push_back(hex[byte >> 4]);
            value.push_back(hex[byte & 15]);
        }
        return value;
    }
    bool done() const { return valid_ && position_ == size_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t position_ = 0;
    bool valid_ = true;
    bool available(size_t count) {
        if (!valid_ || count > size_ - position_) {
            valid_ = false;
            return false;
        }
        return true;
    }
};

uint8_t StateFlags(const ItemState& state) {
    return (state.due ? 1 : 0) | (state.acknowledged ? 2 : 0);
}
bool ReadFlags(Reader& reader, const AlarmKey& key, std::vector<ItemState>& items) {
    const auto flags = reader.Number(1);
    if (flags > 3)
        return false;
    items.push_back({key, (flags & 1) != 0, (flags & 2) != 0});
    return true;
}
}  // namespace

CodecResult Encode(const FacePersistentState& state, const Scope& enrolled_scope, Bytes& output) {
    if (!WithinLimits(state))
        return CodecResult::LimitExceeded;
    FaceModel validated(enrolled_scope);
    if (!validated.RestoreState(state))
        return CodecResult::InvalidState;

    const auto& schedule = state.schedule;
    const auto& s = schedule.snapshot;
    Writer w;
    w.bytes.reserve(kWorstCaseRecordBytes);
    w.bytes.insert(w.bytes.end(), std::begin(kMagic), std::end(kMagic));
    w.Number(s.version == 2 ? 2 : kEncodingVersion, 1);
    w.Number(0, 2);  // Patched with exact final record length below.
    w.Number(0, 1);  // Reserved flags: v1 accepts only zero.
    w.Number(s.cues.size(), 1);
    w.Number(s.timers.size(), 1);
    w.Number(schedule.retired_ids.size(), 1);
    w.Number(state.pending.size(), 1);
    w.Uuid(s.scope.assignment_id);
    w.Uuid(s.scope.device_id);
    if (s.version == 2 && s.service_occurrence_id.empty()) {
        w.Number(0, 8);
        w.Number(0, 8);
    } else {
        w.Uuid(s.service_occurrence_id);
    }
    w.Number(s.service_revision, 7);
    w.Number(s.snapshot_revision, 7);
    w.Number(s.service_at_ms, 6);
    w.Number(s.server_now_ms, 6);
    w.Number(schedule.last_known_epoch_ms, 6);
    w.Text(s.timezone, 1);
    size_t index = 0;
    for (const auto& cue : s.cues) {
        w.Number(cue.kind == CueKind::ServiceOffset ? 0 : 1, 1);
        w.Uuid(cue.id);
        w.Number(cue.revision, 7);
        w.Number(cue.deadline_ms, 6);
        w.Number(static_cast<uint32_t>(cue.offset_ms), 4);
        w.Text(cue.label, 2);
        w.Number(StateFlags(schedule.items[index++]), 1);
    }
    for (const auto& timer : s.timers) {
        w.Uuid(timer.id);
        w.Number(timer.revision, 7);
        w.Number(timer.deadline_ms, 6);
        w.Text(timer.label, 2);
        w.Number(StateFlags(schedule.items[index++]), 1);
    }
    for (const auto& retired : schedule.retired_ids)
        w.Uuid(retired);
    for (const auto& key : state.pending) {
        w.Number(key.kind == ItemKind::Cue ? 0 : 1, 1);
        w.Uuid(key.id);
        w.Number(key.revision, 7);
        if (key.kind == ItemKind::Cue)
            w.Uuid(key.service_occurrence_id);
    }
    const auto length = w.bytes.size() + 4;
    if (length > kWorstCaseRecordBytes || length > kMaximumRecordBytes)
        return CodecResult::LimitExceeded;
    w.bytes[5] = static_cast<uint8_t>(length);
    w.bytes[6] = static_cast<uint8_t>(length >> 8);
    w.Number(Crc32(w.bytes.data(), w.bytes.size()), 4);
    output = std::move(w.bytes);
    return CodecResult::Accepted;
}

CodecResult Decode(const uint8_t* data, size_t size, const Scope& enrolled_scope,
                   FacePersistentState& output) {
    if (size > kMaximumRecordBytes)
        return CodecResult::LimitExceeded;
    if (!data || size < kHeaderBytes + 4 || !std::equal(std::begin(kMagic), std::end(kMagic), data))
        return CodecResult::Corrupt;
    if (data[4] != kEncodingVersion && data[4] != 2)
        return CodecResult::UnsupportedVersion;
    Reader checksum(data + size - 4, 4);
    if (Crc32(data, size - 4) != checksum.Number(4))
        return CodecResult::Corrupt;
    Reader r(data, size - 4);
    r.Number(4);  // Magic already checked.
    r.Number(1);  // Version already checked.
    if (r.Number(2) != size || r.Number(1) != 0)
        return CodecResult::Corrupt;
    const auto cues = r.Number(1), timers = r.Number(1), retired = r.Number(1),
               pending = r.Number(1);
    if (cues > kMaximumActiveItems || timers > kMaximumActiveItems - cues ||
        retired > kMaximumRetiredIds || pending > kMaximumPendingAcks)
        return CodecResult::LimitExceeded;

    FacePersistentState candidate;
    auto& schedule = candidate.schedule;
    auto& s = schedule.snapshot;
    s.version = data[4];
    s.scope.assignment_id = r.Uuid();
    s.scope.device_id = r.Uuid();
    s.service_occurrence_id = r.Uuid();
    if (s.version == 2 && s.service_occurrence_id == "00000000-0000-0000-0000-000000000000")
        s.service_occurrence_id.clear();
    s.service_revision = r.Number(7);
    s.snapshot_revision = r.Number(7);
    s.service_at_ms = static_cast<int64_t>(r.Number(6));
    s.server_now_ms = static_cast<int64_t>(r.Number(6));
    schedule.last_known_epoch_ms = static_cast<int64_t>(r.Number(6));
    s.timezone = r.Text(1, 64);
    for (size_t i = 0; i < cues; ++i) {
        Cue cue;
        const auto kind = r.Number(1);
        if (kind > 1)
            return CodecResult::Corrupt;
        cue.kind = kind == 0 ? CueKind::ServiceOffset : CueKind::Fixed;
        cue.id = r.Uuid();
        cue.revision = r.Number(7);
        cue.deadline_ms = static_cast<int64_t>(r.Number(6));
        const auto offset = r.Number(4);
        cue.offset_ms = offset > 0x7fffffffu ? static_cast<int64_t>(offset) - 0x100000000LL
                                             : static_cast<int64_t>(offset);
        cue.label = r.Text(2, 320);
        if (!ReadFlags(r, {s.scope, ItemKind::Cue, s.service_occurrence_id, cue.id, cue.revision},
                       schedule.items))
            return CodecResult::Corrupt;
        s.cues.push_back(std::move(cue));
    }
    for (size_t i = 0; i < timers; ++i) {
        Timer timer;
        timer.id = r.Uuid();
        timer.revision = r.Number(7);
        timer.deadline_ms = static_cast<int64_t>(r.Number(6));
        timer.label = r.Text(2, 320);
        if (!ReadFlags(r, {s.scope, ItemKind::Timer, {}, timer.id, timer.revision}, schedule.items))
            return CodecResult::Corrupt;
        s.timers.push_back(std::move(timer));
    }
    for (size_t i = 0; i < retired; ++i)
        schedule.retired_ids.push_back(r.Uuid());
    for (size_t i = 0; i < pending; ++i) {
        AlarmKey key;
        key.scope = s.scope;
        const auto kind = r.Number(1);
        if (kind > 1)
            return CodecResult::Corrupt;
        key.kind = kind == 0 ? ItemKind::Cue : ItemKind::Timer;
        key.id = r.Uuid();
        key.revision = r.Number(7);
        if (key.kind == ItemKind::Cue)
            key.service_occurrence_id = r.Uuid();
        candidate.pending.push_back(std::move(key));
    }
    if (!r.done())
        return CodecResult::Corrupt;
    FaceModel validated(enrolled_scope);
    if (!validated.RestoreState(candidate))
        return CodecResult::InvalidState;
    output = std::move(candidate);
    return CodecResult::Accepted;
}

}  // namespace orbit::service_schedule::storage
