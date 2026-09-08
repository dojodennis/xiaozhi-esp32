#include "service_schedule_alarm_output.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using orbit::service_schedule::AlarmOutput;
using orbit::service_schedule::AlarmOutputHooks;

namespace {
struct Fixture {
    int64_t now = 0;
    bool drained = true, admit = true, motor = false;
    uint32_t errors = 0;
    int starts = 0, cancels = 0;
    std::vector<std::pair<int64_t, bool>> pulses;
    AlarmOutput output;

    Fixture()
        : output(AlarmOutputHooks{[this] {
                                      ++starts;
                                      if (!admit)
                                          return false;
                                      drained = false;
                                      return true;
                                  },
                                  [this] { ++cancels; }, [this] { return drained; },
                                  [this] { return errors; },
                                  [this](bool active) {
                                      motor = active;
                                      pulses.emplace_back(now, active);
                                  }}) {}

    void Poll(int64_t time) {
        now = time;
        output.Poll(now);
    }
    void Want(bool wanted, int64_t time) {
        now = time;
        output.SetWanted(wanted, now);
    }
    bool Retry(int64_t time) {
        now = time;
        return output.Retry(now);
    }
};

void IdleAndSingleAdmission() {
    Fixture f;
    for (int64_t time = 0; time <= 1000; time += 50)
        f.Poll(time);
    assert(f.starts == 0 && f.cancels == 0 && !f.motor && !f.output.fault());
    f.Want(true, 1000);
    assert(f.starts == 1 && !f.drained && f.motor && f.output.wanted());
    for (int64_t time = 1050; time <= 1900; time += 50) {
        f.Want(true, time);
        f.Poll(time);
    }
    assert(f.starts == 1 && f.cancels == 0 && !f.output.fault());
}

void CompleteDrainAndGap() {
    Fixture f;
    f.Want(true, 0);
    f.Poll(300);
    assert(f.starts == 1);
    f.drained = true;
    f.Poll(350);
    f.Poll(1349);
    assert(f.starts == 1 && !f.output.fault());
    f.Poll(1350);
    assert(f.starts == 2 && !f.drained && f.cancels == 0);
    f.Poll(1351);
    assert(f.starts == 2);
}

void CancellationWaitsForPhysicalTail() {
    Fixture f;
    f.Want(true, 0);
    f.Want(false, 50);
    assert(f.cancels == 1 && !f.motor && !f.output.wanted() && !f.drained);
    f.Want(true, 100);
    for (int64_t time = 150; time <= 400; time += 50)
        f.Poll(time);
    assert(f.starts == 1 && f.cancels == 1 && !f.output.fault());
    f.drained = true;
    f.Poll(450);
    assert(f.starts == 2 && !f.drained);
}

void CancelledClipNeverRestartsWithoutDemand() {
    Fixture f;
    f.Want(true, 0);
    f.Want(false, 50);
    f.drained = true;
    f.Poll(100);
    ++f.errors;  // Late observation while no clip is owned cannot revive playback.
    for (int64_t time = 150; time <= 2000; time += 50)
        f.Poll(time);
    assert(f.starts == 1 && f.cancels == 1 && !f.motor && !f.output.fault());
    f.Want(true, 2050);  // A new admission takes a fresh error-counter baseline.
    f.Poll(2100);
    assert(f.starts == 2 && !f.output.fault());
}

void ForeignOwnerIsNeverCancelled() {
    Fixture f;
    f.drained = false;
    f.Want(true, 0);
    assert(f.output.fault() && f.starts == 0 && f.cancels == 0 && !f.motor);
    assert(!f.Retry(50));
    assert(f.starts == 0 && f.cancels == 0);
    f.drained = true;
    assert(f.Retry(100));
    assert(f.starts == 1 && f.motor && !f.output.fault());
}

void ForeignOwnerDuringGapIsNeverCancelled() {
    Fixture f;
    f.Want(true, 0);
    f.drained = true;
    f.Poll(50);
    f.drained = false;  // Another owner appears after our completed clip.
    f.Poll(1050);
    assert(f.output.fault() && !f.motor && f.starts == 1 && f.cancels == 0);
}

void FailedAdmissionRequiresExplicitRetry() {
    Fixture f;
    f.admit = false;
    f.Want(true, 0);
    assert(f.output.fault() && f.starts == 1 && f.cancels == 0 && !f.motor);
    f.admit = true;
    for (int64_t time = 50; time <= 1000; time += 50)
        f.Poll(time);
    assert(f.starts == 1);
    assert(f.Retry(1050));
    assert(f.starts == 2 && !f.output.fault());
    assert(!f.Retry(1100));
}

void ErrorWinsOverDrainAndIsLatched() {
    Fixture f;
    f.Want(true, 0);
    ++f.errors;
    f.drained = true;
    f.Poll(50);
    assert(f.output.fault() && !f.motor && f.cancels == 1 && f.starts == 1);
    f.Poll(1500);
    assert(f.starts == 1);
    assert(f.Retry(1550));
    f.Poll(1600);
    assert(f.starts == 2 && !f.output.fault());
}

void CounterWrapStillDetectsFailure() {
    Fixture f;
    f.errors = std::numeric_limits<uint32_t>::max();
    f.Want(true, 0);
    f.errors = 0;
    f.Poll(50);
    assert(f.output.fault() && f.starts == 1 && f.cancels == 1 && !f.motor);
}

void PlaybackAndTailTimeouts() {
    Fixture playing;
    playing.Want(true, 0);
    playing.Poll(5000);
    assert(!playing.output.fault());
    playing.Poll(5001);
    assert(playing.output.fault() && playing.cancels == 1 && !playing.motor);
    assert(!playing.Retry(5050));
    playing.drained = true;
    assert(playing.Retry(5100) && playing.starts == 2);

    Fixture tail;
    tail.Want(true, 0);
    tail.Want(false, 50);
    tail.Poll(5050);
    assert(!tail.output.fault());
    tail.Poll(5051);
    assert(tail.output.fault() && !tail.motor && tail.starts == 1);
    tail.drained = true;
    assert(tail.Retry(5100));
    assert(tail.starts == 1 && !tail.output.wanted());
}

void StaleTimeFailsClosedAfterCancellation() {
    Fixture f;
    f.Want(true, 100);
    f.Want(false, 150);
    f.drained = true;
    f.Poll(200);
    const int cancelled = f.cancels;
    f.Poll(199);
    assert(f.output.fault() && !f.motor && f.starts == 1 && f.cancels == cancelled);
    assert(!f.Retry(199));
    assert(f.Retry(201));
    assert(f.starts == 1 && !f.output.wanted());
}

void PulsePatternAtFiftyMilliseconds() {
    Fixture f;
    f.Want(true, 0);
    for (int64_t time = 50; time <= 2150; time += 50)
        f.Poll(time);
    const std::vector<std::pair<int64_t, bool>> expected = {
        {0, true}, {150, false}, {1000, true}, {1150, false}, {2000, true}, {2150, false}};
    assert(f.pulses == expected && !f.motor && f.starts == 1);
}

void ResumedPollDropsStaleHigh() {
    Fixture f;
    f.Want(true, 0);
    // The host cannot prove a pulse duration during a stalled owner. It can
    // prove that resuming at a later period does not leave the stale HIGH set.
    f.Poll(1000);
    assert(!f.motor);
    assert((f.pulses == std::vector<std::pair<int64_t, bool>>{{0, true}, {1000, false}}));
}

void MissingHooksFailWithoutTouchingForeignOutput() {
    int cancelled = 0;
    AlarmOutput no_start({{}, [&] { ++cancelled; }, [] { return false; }, [] { return 0; }, {}});
    no_start.SetWanted(true, 0);
    assert(no_start.fault() && cancelled == 0);
    AlarmOutput empty({});
    empty.Poll(0);
    assert(empty.fault() && !empty.Retry(1));
}
}  // namespace

int main(int argc, char** argv) {
    const std::pair<const char*, void (*)()> tests[] = {
        {"idle_single_admission", IdleAndSingleAdmission},
        {"complete_drain_and_gap", CompleteDrainAndGap},
        {"cancel_waits_for_tail", CancellationWaitsForPhysicalTail},
        {"cancelled_clip_stays_stopped", CancelledClipNeverRestartsWithoutDemand},
        {"foreign_owner_at_start", ForeignOwnerIsNeverCancelled},
        {"foreign_owner_during_gap", ForeignOwnerDuringGapIsNeverCancelled},
        {"failed_admission_explicit_retry", FailedAdmissionRequiresExplicitRetry},
        {"error_wins_over_drain", ErrorWinsOverDrainAndIsLatched},
        {"error_counter_wrap", CounterWrapStillDetectsFailure},
        {"playback_and_tail_timeout", PlaybackAndTailTimeouts},
        {"stale_time_after_cancel", StaleTimeFailsClosedAfterCancellation},
        {"pulse_pattern_50ms", PulsePatternAtFiftyMilliseconds},
        {"resumed_poll_drops_stale_high", ResumedPollDropsStaleHigh},
        {"missing_hooks", MissingHooksFailWithoutTouchingForeignOutput},
    };
    assert(argc == 2);
    for (const auto& [name, test] : tests) {
        if (std::string(argv[1]) == name) {
            test();
            std::printf("PASS %s\n", name);
            return 0;
        }
    }
    return 2;
}
