#ifndef ORBIT_SERVICE_SCHEDULE_DEMO_H_
#define ORBIT_SERVICE_SCHEDULE_DEMO_H_

#include "service_schedule_face.h"

namespace orbit::service_schedule {
// Compile-time bench fixture, never a transport entrypoint. The synthetic epoch
// advances with elapsed time and explicit checkpoints; it measures no real latency.
class ScheduleDemo {
public:
    ScheduleDemo();
    bool Advance();
    void Elapse(int64_t delta_ms);
    bool Acknowledge() { return model_.AcknowledgeNext(); }
    const FaceModel& model() const { return model_; }
    FaceModel& model() { return model_; }
    const char* stage() const { return stage_; }
    bool valid() const { return valid_; }
    static std::string FixtureTime(int64_t epoch_ms);

private:
    FaceModel model_;
    int step_ = 0;
    int64_t monotonic_ms_ = 0;
    bool valid_ = false;
    const char* stage_ = "Fixture unavailable";
    bool ApplyFixture(int index, int64_t monotonic_ms);
    void MoveTo(int64_t checkpoint_ms);
};
}  // namespace orbit::service_schedule
#endif
