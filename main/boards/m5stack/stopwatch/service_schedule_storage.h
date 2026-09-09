#ifndef ORBIT_SERVICE_SCHEDULE_STORAGE_H_
#define ORBIT_SERVICE_SCHEDULE_STORAGE_H_

#include <cstddef>
#include <cstdint>
#include <vector>
#include "service_schedule_face.h"

namespace orbit::service_schedule::storage {

inline constexpr size_t kMaximumActiveItems = 6;
inline constexpr size_t kMaximumPendingAcks = 6;
inline constexpr size_t kMaximumRecordBytes = 4096;
// Header 93, zone 64, six maximum-label cues 6*357, retired UUIDs 64*16,
// six cue ACKs 6*40, CRC32 4. Timers and timer ACKs are smaller than cues.
inline constexpr size_t kWorstCaseRecordBytes = 93 + 64 + 6 * 357 + 64 * 16 + 6 * 40 + 4;
static_assert(kWorstCaseRecordBytes <= kMaximumRecordBytes);

using Bytes = std::vector<uint8_t>;
enum class CodecResult { Accepted, InvalidState, LimitExceeded, Corrupt, UnsupportedVersion };

// Deterministic little-endian OSS1 format. Encoding 1 bytes remain unchanged for
// v1 snapshots; encoding 2 stores v2 snapshots and an explicitly zero-packed absent
// occurrence only with its exact zero/empty Service fields. Loading never converts
// versions. The server-time slot remains zero for v2. UUIDs are packed
// into 16 bytes; revisions use 7 bytes, epochs 6, signed offsets 4. Active keys
// are derived from the snapshot; pending ACK keys retain their original revision
// and occurrence even after replacement/retirement. Order is preserved exactly.
// No pointers, C++ struct padding, native-endian integers or connection/clock
// trust are serialized. CRC32 detects corruption; it is NOT authentication or
// protection against rollback of an entire valid storage image.
//
// enrolled_scope is caller-owned, never inferred from bytes. FaceModel validates
// scope, UTF-8/scalar bounds, UUIDs and complete state. Network authentication,
// IANA membership and the wire's full Unicode policy remain upstream duties.
// Outputs are unchanged on every failure. No silent truncation or eviction.
CodecResult Encode(const FacePersistentState& state, const Scope& enrolled_scope, Bytes& output);
CodecResult Decode(const uint8_t* data, size_t size, const Scope& enrolled_scope,
                   FacePersistentState& output);

}  // namespace orbit::service_schedule::storage
#endif
