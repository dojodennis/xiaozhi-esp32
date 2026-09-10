#pragma once

// Once-per-transition OFFLINE cue for the Provisions StopWatch.
//
// The reconnect loop retries forever, so the cue must not follow retries; it
// follows the edge. The signal is armed by a successful gateway hello and
// disarmed by the first offline note after it, so a cold boot with no gateway
// shows the OFFLINE face silently and every later loss (transport dropped,
// heartbeat expired, gateway refused) fires exactly once until the next hello.
namespace provisions {

class OfflineSignal {
public:
    // Hello succeeded. Returns true when this ends an offline period.
    bool Online() {
        const bool recovered = !armed_;
        armed_ = true;
        return recovered;
    }

    // Gateway lost or refused. Returns true only on the first note after a
    // hello; retries and repeated failure notes return false.
    bool Offline() {
        if (!armed_) {
            return false;
        }
        armed_ = false;
        return true;
    }

    bool armed() const { return armed_; }

private:
    bool armed_ = false;
};

}  // namespace provisions
