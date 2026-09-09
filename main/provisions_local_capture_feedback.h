#ifndef PROVISIONS_LOCAL_CAPTURE_FEEDBACK_H_
#define PROVISIONS_LOCAL_CAPTURE_FEEDBACK_H_

#include <atomic>
#include <cstdint>

namespace provisions {

// Main-task output owns the motor. ESP timer callbacks carry this absolute
// deadline so an older callback cannot terminate a newly armed pulse.
class LocalCapturePulse {
public:
    int64_t Arm(int64_t now_us, uint32_t duration_ms) {
        const int64_t deadline = now_us + static_cast<int64_t>(duration_ms) * 1000;
        deadline_us_.store(deadline);
        return deadline;
    }

    int64_t deadline() const { return deadline_us_.load(); }

    bool IsExpired(int64_t observed_deadline_us, int64_t now_us) const {
        return observed_deadline_us > 0 && deadline_us_.load() == observed_deadline_us &&
               now_us >= observed_deadline_us;
    }

    bool Clear(int64_t observed_deadline_us) {
        return deadline_us_.compare_exchange_strong(observed_deadline_us, 0);
    }

    void Cancel() { deadline_us_.store(0); }

private:
    std::atomic<int64_t> deadline_us_{0};
};

}  // namespace provisions

#endif  // PROVISIONS_LOCAL_CAPTURE_FEEDBACK_H_
