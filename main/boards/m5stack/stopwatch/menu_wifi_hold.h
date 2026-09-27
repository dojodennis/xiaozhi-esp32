#pragma once
#include <cstdint>

namespace ProvisionsStopWatch {
// Owned under the board's chord mutex. A menu press never starts the microphone.
class MenuWifiHold {
public:
    static constexpr int64_t kHoldUs = 5LL * 1000000;
    enum class Release { Unhandled, Confirm, Consumed };
    void Begin(int64_t now, bool menu, bool locked) {
        ++generation_;
        reserved_ = menu && !locked;
        cancelled_ = fired_ = false;
        deadline_ = now + kHoldUs;
    }
    void Cancel() { cancelled_ = true; }
    bool Reserved() const { return reserved_; }
    uint32_t Generation() const { return generation_; }
    bool Fire(int64_t now) {
        if (!reserved_ || cancelled_ || fired_ || now < deadline_)
            return false;
        fired_ = true;
        return true;
    }
    bool StillHeld(uint32_t generation) const {
        return generation == generation_ && reserved_ && fired_ && !cancelled_;
    }
    Release End(int64_t now) {
        const auto result = !reserved_
                                ? Release::Unhandled
                                : (!cancelled_ && !fired_ && now < deadline_ ? Release::Confirm
                                                                             : Release::Consumed);
        reserved_ = false;
        return result;
    }

private:
    bool reserved_ = false, cancelled_ = false, fired_ = false;
    int64_t deadline_ = 0;
    uint32_t generation_ = 0;
};
}  // namespace ProvisionsStopWatch
