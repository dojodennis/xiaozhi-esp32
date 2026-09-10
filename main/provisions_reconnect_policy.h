#pragma once

#include <cstdint>
#include <limits>

namespace provisions::reconnect {

constexpr int kMaximumBackoffExponent = 4;
constexpr uint32_t kJitterSpanTicks = 4;

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

constexpr int DelayTicks(int attempts, uint32_t entropy) {
    int exponent = attempts;
    if (exponent < 0) {
        exponent = 0;
    } else if (exponent > kMaximumBackoffExponent) {
        exponent = kMaximumBackoffExponent;
    }
    return (1 << exponent) + static_cast<int>(entropy % kJitterSpanTicks);
}

constexpr RetrySchedule AfterGatewayFailure(int attempts, uint32_t entropy) {
    return {SaturatingIncrement(attempts), DelayTicks(attempts, entropy)};
}

constexpr int WorkerFailureDelayTicks(uint32_t entropy) {
    return DelayTicks(kMaximumBackoffExponent, entropy);
}

}  // namespace provisions::reconnect
