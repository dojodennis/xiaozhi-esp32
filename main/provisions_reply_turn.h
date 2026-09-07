#ifndef PROVISIONS_REPLY_TURN_H_
#define PROVISIONS_REPLY_TURN_H_

#include <atomic>
#include <cstdint>

// A monotonically increasing talk identity. This is correlation,
// not authorization or a business request ID. Never wrap into an old reply.
class ProvisionsReplyTurn {
public:
    bool Begin() {
        auto current = state_.load(std::memory_order_acquire);
        while ((current & kIdMask) < kIdMask) {
            if (state_.compare_exchange_weak(current, (current & kIdMask) + 1,
                                             std::memory_order_acq_rel)) {
                return true;
            }
        }
        return false;
    }

    void Invalidate() { state_.fetch_or(kInvalid, std::memory_order_acq_rel); }
    uint32_t id() const { return state_.load(std::memory_order_acquire) & kIdMask; }
    bool IsCurrent(uint32_t candidate) const {
        return candidate != 0 && candidate <= kIdMask &&
               candidate == state_.load(std::memory_order_acquire);
    }

private:
    static constexpr uint32_t kInvalid = 0x80000000U;
    static constexpr uint32_t kIdMask = kInvalid - 1;
    std::atomic<uint32_t> state_{0};
};

#endif  // PROVISIONS_REPLY_TURN_H_
