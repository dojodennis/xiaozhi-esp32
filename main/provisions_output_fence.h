#ifndef PROVISIONS_OUTPUT_FENCE_H_
#define PROVISIONS_OUTPUT_FENCE_H_

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace provisions::output_fence {
constexpr uint64_t kMaximumInteger = 9007199254740991ULL;
constexpr size_t kManifestBytes = 224;
using Manifest = std::array<uint8_t, kManifestBytes>;

enum class Phase : uint8_t {
    Owned = 1,
    DrainedPendingCommit = 2,
    Terminal = 3,
    AbortUnacquiredUnclosed = 4,
    AbortUnacquiredDrainedPendingCommit = 5
};
enum class Origin : uint8_t { Normal = 0, AbortUnacquired = 1 };
struct Identity {
    std::string device_id, checkpoint_sha256, lease_id, playback_id;
    std::string request_id, route_epoch, device_connection_id;
    uint64_t fence_epoch = 0, sequence = 0;
    uint32_t response_revision = 0;
};
struct PhoneReceipt {
    bool completed = false, has_started = false;
    uint64_t started_at_ms = 0, stopped_at_ms = 0, drained_at_ms = 0;
};
struct Record {
    Identity identity;
    Phase phase = Phase::Owned;
    Origin origin = Origin::Normal;
    PhoneReceipt receipt;
    std::string backend_commit_id;
};
enum class Command { Acquire, Release, CloseCommit, AbortUnacquired };
struct Message {
    Command command = Command::Acquire;
    Identity identity;
    PhoneReceipt receipt;
    std::string backend_commit_id;
};
enum class Result { Denied, RecoveryRequired, Acquired, DrainPending, Released, AbortPending };
struct Reply {
    Result result = Result::Denied;
    std::string json;
};
enum class Readiness { Uncommissioned, Blocked, RecoveryRequired, Ready };
struct ReadinessSnapshot {
    Readiness state = Readiness::Blocked;
    std::optional<uint64_t> fence_epoch;
};

bool ValidId(std::string_view value);
bool ValidIdentity(const Identity& identity);
bool ValidReceipt(const PhoneReceipt& receipt);
bool SameIdentity(const Identity& a, const Identity& b);
bool SameReceipt(const PhoneReceipt& a, const PhoneReceipt& b);
bool ParseMessage(std::string_view text, Message& message);
std::string ReplyJson(Result result, const Identity& identity);
bool EncodeRecord(const Record& record, Manifest& bytes);
bool DecodeRecord(const Manifest& bytes, Record& record);

class Store {
public:
    enum class LoadResult { Present, Missing, Fault };
    virtual ~Store() = default;
    virtual LoadResult Load(Record& record) = 0;
    virtual bool CompareExchange(const Record& expected, const Record& desired) = 0;
};
// Distinct bounded namespace. No erase, absent-record initialization, or commissioning API.
class NvsStore final : public Store {
public:
    LoadResult Load(Record& record) override;
    bool CompareExchange(const Record& expected, const Record& desired) override;
};

struct Evidence {
    bool gate_owned = false, input_closed = false, output_drained = false,
         fallback_disabled = false;
    bool Closed() const {
        return gate_owned && input_closed && output_drained && fallback_disabled;
    }
};
class Physical {
public:
    virtual ~Physical() = default;
    // Synchronously fence every producer and input admission, including cold boot/unknown NVS.
    virtual void BlockAll() = 0;
    // Install the exact persisted owner and request closure; this is NOT drain evidence.
    virtual bool Hold(const Identity& identity) = 0;
    // Actual input/worker closure and codec/DMA drain under the still-held global owner.
    // Ownership must prevent new I/O between this snapshot and OpenAfterTerminal.
    virtual Evidence Observe(const Identity& identity) = 0;
    // Atomic identity/epoch comparison. May open only this owner's gate after durable terminal.
    virtual bool OpenAfterTerminal(const Identity& identity) = 0;
};

// Bound only by the future authenticated gateway/observed-receipt adapter. No raw-JSON default.
class AbortAuthority {
public:
    virtual ~AbortAuthority() = default;
    virtual bool Allows(const Identity& identity, const PhoneReceipt& receipt) = 0;
};

// Core only: no protocol advertisement/runtime binding. Construct before any I/O admission;
// invoke on a serialized worker, never an ESP_TIMER callback. Hooks must not reenter this core.
// Abort requests require the registered observed stopped event plus validated original grant;
// a caller-supplied no-start literal is not authority. Runtime authentication remains unbound.
class Core {
public:
    Core(Store& store, Physical& physical, std::string device_id,
         AbortAuthority* abort_authority = nullptr);
    bool Hydrate();
    Reply Handle(std::string_view text);
    bool GateOpen() const;
    // Cached Core result only: no storage or physical I/O, and no global admission authority.
    ReadinessSnapshot Snapshot() const;

private:
    Reply Respond(Result result) const;
    bool Closed();
    bool Persist(const Record& next);
    bool OpenTerminal();
    Store& store_;
    Physical& physical_;
    const std::string device_id_;
    AbortAuthority* const abort_authority_;
    mutable std::mutex mutex_;
    Record record_;
    bool loaded_ = false, gate_open_ = false;
    Readiness readiness_ = Readiness::Blocked;
};
}  // namespace provisions::output_fence
#endif
