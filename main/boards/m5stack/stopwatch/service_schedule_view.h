#ifndef ORBIT_SERVICE_SCHEDULE_VIEW_H_
#define ORBIT_SERVICE_SCHEDULE_VIEW_H_

#include <lvgl.h>
#include "service_schedule_face.h"

#include <array>
#include <functional>

namespace orbit::service_schedule {

// The board and host demo compile this exact LVGL renderer. The caller holds the
// display lock. Formatting local civil time belongs to the admitted time-zone
// adapter; a missing formatter shows an explicit unknown time.
class ScheduleView {
public:
    using TimeLabel = std::function<std::string(int64_t)>;
    void Create(lv_obj_t* parent, const lv_font_t* large, const lv_font_t* small);
    void Render(const FaceModel& model, const TimeLabel& time_label, bool synthetic_demo);

private:
    lv_obj_t* root_ = nullptr;
    lv_obj_t* title_ = nullptr;
    lv_obj_t* service_ = nullptr;
    lv_obj_t* cue_ = nullptr;
    lv_obj_t* status_ = nullptr;
    lv_obj_t* alarm_layer_ = nullptr;
    lv_obj_t* alarm_heading_ = nullptr;
    lv_obj_t* alarm_ = nullptr;
    lv_obj_t* alarm_status_ = nullptr;
    std::array<lv_obj_t*, ProvisionsStopwatchOrbit::kMaximumSlots> arcs_{};
    std::array<lv_obj_t*, ProvisionsStopwatchOrbit::kMaximumSlots> labels_{};
    std::array<lv_obj_t*, ProvisionsStopwatchOrbit::kMaximumSlots> remaining_{};
};
}  // namespace orbit::service_schedule
#endif
