#include "service_schedule_alarm_output.h"

namespace orbit::service_schedule {
void AlarmOutput::Motor(bool active) {
    if (active != motor_) {
        motor_ = active;
        if (hooks_.motor)
            hooks_.motor(active);
    }
}
void AlarmOutput::Fail(int64_t now_ms) {
    fault_ = true;
    Motor(false);
    if (owns_clip_ && hooks_.cancel)
        hooks_.cancel();
    phase_ = Phase::Draining;
    phase_at_ms_ = now_ms;
}
void AlarmOutput::SetWanted(bool wanted, int64_t now_ms) {
    if (wanted == wanted_)
        return;
    wanted_ = wanted;
    if (wanted) {
        next_pulse_ms_ = now_ms;
    } else {
        Motor(false);
        if (owns_clip_ && hooks_.cancel)
            hooks_.cancel();
        phase_ = Phase::Draining;
        phase_at_ms_ = now_ms;
    }
    Poll(now_ms);
}
void AlarmOutput::Poll(int64_t now_ms) {
    if (!hooks_.start || !hooks_.cancel || !hooks_.drained || !hooks_.errors || now_ms < 0 ||
        (last_ms_ >= 0 && now_ms < last_ms_)) {
        Fail(now_ms);
        return;
    }
    last_ms_ = now_ms;
    if (fault_) {
        Motor(false);
        return;
    }
    if (owns_clip_ && hooks_.errors() != errors_) {
        Fail(now_ms);
        return;
    }
    // After any scheduling stall, a stale high gets a real low edge and a full
    // off interval. An absolute modulo phase could accidentally keep it high.
    if (!wanted_) {
        Motor(false);
    } else if (motor_ && now_ms - motor_at_ms_ >= 150) {
        Motor(false);
        next_pulse_ms_ = now_ms + 850;
    } else if (!motor_ && now_ms >= next_pulse_ms_) {
        Motor(true);
        motor_at_ms_ = now_ms;
    }
    if (phase_ == Phase::Playing) {
        if (hooks_.errors() != errors_ || now_ms - phase_at_ms_ > 5000) {
            Fail(now_ms);
            return;
        }
        if (!hooks_.drained())
            return;
        owns_clip_ = false;
        phase_ = Phase::Gap;
        phase_at_ms_ = now_ms;
    } else if (phase_ == Phase::Draining) {
        if (!hooks_.drained()) {
            if (now_ms - phase_at_ms_ > 5000)
                Fail(now_ms);
            return;
        }
        owns_clip_ = false;
        phase_ = Phase::Idle;
    }
    if (!wanted_ || (phase_ == Phase::Gap && now_ms - phase_at_ms_ < 1000))
        return;
    if (!hooks_.drained()) {
        // Another owner violates the isolated bench contract; never clear its
        // queue by calling PlayLocalFeedback or silently compete for output.
        Fail(now_ms);
        return;
    }
    errors_ = hooks_.errors();
    if (!hooks_.start()) {
        Fail(now_ms);
        return;
    }
    owns_clip_ = true;
    phase_ = Phase::Playing;
    phase_at_ms_ = now_ms;
}
bool AlarmOutput::Retry(int64_t now_ms) {
    if (!fault_ || !hooks_.drained || !hooks_.drained() || now_ms < last_ms_)
        return false;
    fault_ = false;
    owns_clip_ = false;
    phase_ = Phase::Idle;
    next_pulse_ms_ = now_ms;
    Poll(now_ms);
    return !fault_;
}
}  // namespace orbit::service_schedule
