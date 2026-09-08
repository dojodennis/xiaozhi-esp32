#ifndef ORBIT_SERVICE_SCHEDULE_ALARM_OUTPUT_H_
#define ORBIT_SERVICE_SCHEDULE_ALARM_OUTPUT_H_

#include <cstdint>
#include <functional>
#include <utility>

namespace orbit::service_schedule {

struct AlarmOutputHooks {
    // All hooks are bounded admission/observation operations. Playback is owned
    // exclusively by this bench; start must use AudioService's queued clip path.
    std::function<bool()> start;
    std::function<void()> cancel;
    std::function<bool()> drained;
    std::function<uint32_t()> errors;
    std::function<void(bool)> motor;
};

// Main-owner, nonblocking alarm sequencer. It never touches a codec, parses an
// asset, sleeps, writes NVS or treats queue drain as proof of audible hardware.
// Motor requests are 150 ms on / 850 ms off when polled at 50 ms. Scheduling
// stalls can extend a physical pulse; I2C/timing requires a board measurement.
class AlarmOutput {
public:
    explicit AlarmOutput(AlarmOutputHooks hooks) : hooks_(std::move(hooks)) {}
    void SetWanted(bool wanted, int64_t now_ms);
    void Poll(int64_t now_ms);
    // Explicit operator retry only, after a fault. Does not clear a timer/ACK.
    bool Retry(int64_t now_ms);
    bool fault() const { return fault_; }
    bool wanted() const { return wanted_; }

private:
    enum class Phase { Idle, Playing, Draining, Gap };
    AlarmOutputHooks hooks_;
    Phase phase_ = Phase::Idle;
    bool wanted_ = false, fault_ = false, motor_ = false, owns_clip_ = false;
    int64_t phase_at_ms_ = 0, motor_at_ms_ = 0, next_pulse_ms_ = 0, last_ms_ = -1;
    uint32_t errors_ = 0;
    void Motor(bool active);
    void Fail(int64_t now_ms);
};

}  // namespace orbit::service_schedule
#endif
