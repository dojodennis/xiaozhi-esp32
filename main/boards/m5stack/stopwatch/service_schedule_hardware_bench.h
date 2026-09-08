#ifndef ORBIT_SERVICE_SCHEDULE_HARDWARE_BENCH_H_
#define ORBIT_SERVICE_SCHEDULE_HARDWARE_BENCH_H_

#include "service_schedule_alarm_output.h"
#include "service_schedule_worker.h"

namespace orbit::service_schedule {

// Explicit synthetic identity, usable only with orbit_bench_v1. It is never
// enrollment, an authenticated snapshot, or a business-write capability.
Scope HardwareBenchScope();

// Board-owned bench coordinator. All mutations run on the Application owner;
// only Observed() may be read by a physical callback to bind a queued gesture.
// One pending gesture gets priority over coalesced ticks. No NVS on this owner.
class HardwareBench {
public:
    HardwareBench(std::unique_ptr<WorkerStore> store, AlarmOutputHooks hooks,
                  uint64_t generation = 1);
    void Poll(int64_t now_ms);
    void Yellow(std::shared_ptr<const WorkerPublication> observed, int64_t now_ms);
    void Blue(std::shared_ptr<const WorkerPublication> observed);
    void RejectGesture();
    std::shared_ptr<const WorkerPublication> Observed() const;
    std::string Status() const;
    const std::shared_ptr<const WorkerPublication>& current() const { return current_; }
    bool audio_fault() const { return output_.fault(); }

private:
    uint64_t generation_;
    ServiceScheduleWorker worker_;
    AlarmOutput output_;
    std::shared_ptr<const WorkerPublication> current_, observed_;
    std::optional<WorkerCommand> pending_;
    int64_t next_tick_ms_ = 0;
    bool can_seed_ = false, command_in_flight_ = false;
    std::string notice_;
    bool Queue(WorkerCommand command);
};

}  // namespace orbit::service_schedule
#endif
