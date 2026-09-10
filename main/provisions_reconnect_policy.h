#pragma once

#include <cstdint>
#include <limits>

// Gateway reconnect schedule for the Provisions StopWatch. Pure arithmetic so
// the host tests can pin every value; the caller supplies entropy.
//
// One tick is one maintenance pass (one second). The schedule doubles from
// one second, caps the base at kMaximumBackoffTicks and spreads each wait by
// ±25 % of that base so a fleet of Orbits behind one hotspot does not hammer
// the gateway in lockstep. Attempts saturate rather than wrap; nothing here
// ever gives up. A run of gateway rejections (401/403/429 on the upgrade, or a
// refused hello) raises the post-jitter floor to kRejectionFloorTicks after
// kRejectionFloorAfter consecutive rejections, which keeps a stale credential
// under the gateway's auths-per-minute cap while it keeps trying.
namespace provisions::reconnect {

constexpr int kMaximumBackoffTicks = 30;
constexpr int kRejectionFloorTicks = 10;
constexpr int kRejectionFloorAfter = 3;
constexpr int kJitterDivisor = 4;  // ±25 % of the base

struct RetrySchedule {
    int attempts;
    int wait_ticks;
};

constexpr int SaturatingIncrement(int attempts) {
    if (attempts >= std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    return attempts + 1;
}

constexpr int BaseTicks(int attempts) {
    int base = 1;
    for (int i = 0; i < attempts && base < kMaximumBackoffTicks; ++i) {
        base *= 2;
    }
    return base > kMaximumBackoffTicks ? kMaximumBackoffTicks : base;
}

constexpr int FloorTicks(int consecutive_rejections) {
    return consecutive_rejections >= kRejectionFloorAfter ? kRejectionFloorTicks : 1;
}

// Jittered wait in [base - base/4, base + base/4], then raised to the
// rejection floor so a refused credential never retries faster than
// kRejectionFloorTicks. Never below one tick.
constexpr int DelayTicks(int attempts, int consecutive_rejections, uint32_t entropy) {
    const int base = BaseTicks(attempts);
    const int quarter = base / kJitterDivisor;
    const int span = 2 * quarter + 1;
    const int wait = base - quarter + static_cast<int>(entropy % static_cast<uint32_t>(span));
    const int floor = FloorTicks(consecutive_rejections);
    return wait < floor ? floor : wait;
}

constexpr int MinimumDelayTicks(int attempts, int consecutive_rejections) {
    const int base = BaseTicks(attempts);
    const int wait = base - base / kJitterDivisor;
    const int floor = FloorTicks(consecutive_rejections);
    return wait < floor ? floor : wait;
}

constexpr int MaximumDelayTicks(int attempts, int consecutive_rejections) {
    const int base = BaseTicks(attempts);
    const int wait = base + base / kJitterDivisor;
    const int floor = FloorTicks(consecutive_rejections);
    return wait < floor ? floor : wait;
}

constexpr RetrySchedule AfterGatewayFailure(int attempts, int consecutive_rejections,
                                            uint32_t entropy) {
    return {SaturatingIncrement(attempts), DelayTicks(attempts, consecutive_rejections, entropy)};
}

// Worker admission failures are not gateway-health evidence: wait at the cap
// without advancing the attempt counter.
constexpr int WorkerFailureDelayTicks(uint32_t entropy) {
    return DelayTicks(std::numeric_limits<int>::max(), 0, entropy);
}

}  // namespace provisions::reconnect
