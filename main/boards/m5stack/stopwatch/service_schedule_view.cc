#include "service_schedule_view.h"

#include <algorithm>

namespace orbit::service_schedule {
namespace {
constexpr uint32_t kWhite = 0xffffff;
constexpr uint32_t kAmber = 0xe0a256;
constexpr uint32_t kRed = 0xe07566;
constexpr std::array<uint32_t, 6> kColors = {0xd4b67a, 0x7fbf8f, 0x9fb8d8,
                                             0xc99fd8, 0xe07566, 0x73c7c4};
lv_obj_t* Label(lv_obj_t* parent, const lv_font_t* font, int width, int x, int y) {
    auto* label = lv_label_create(parent);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(kWhite), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, "");
    lv_obj_set_pos(label, x, y);
    return label;
}
}  // namespace

void ScheduleView::Create(lv_obj_t* parent, const lv_font_t* large, const lv_font_t* small) {
    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, 466, 466);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(root_);
    for (int i = 0; i < ProvisionsStopwatchOrbit::kMaximumSlots; ++i) {
        const auto center = ProvisionsStopwatchOrbit::SlotCenter(i);
        arcs_[i] = lv_arc_create(root_);
        lv_obj_set_size(arcs_[i], 104, 104);
        lv_obj_set_pos(arcs_[i], center.x - 52, center.y - 52);
        lv_arc_set_rotation(arcs_[i], 270);
        lv_arc_set_bg_angles(arcs_[i], 0, 360);
        lv_arc_set_range(arcs_[i], 0, 1000);
        lv_obj_remove_style(arcs_[i], nullptr, LV_PART_KNOB);
        lv_obj_remove_flag(arcs_[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(arcs_[i], 6, LV_PART_MAIN);
        lv_obj_set_style_arc_width(arcs_[i], 8, LV_PART_INDICATOR);
        labels_[i] = Label(root_, small, 94, center.x - 47, center.y - 23);
        remaining_[i] = Label(root_, small, 94, center.x - 47, center.y + 4);
    }
    title_ = Label(root_, small, 150, 158, 161);
    service_ = Label(root_, large, 150, 158, 186);
    cue_ = Label(root_, small, 164, 151, 227);
    status_ = Label(root_, small, 176, 145, 252);
    lv_label_set_long_mode(status_, LV_LABEL_LONG_WRAP);
    alarm_layer_ = lv_obj_create(root_);
    lv_obj_remove_style_all(alarm_layer_);
    lv_obj_set_size(alarm_layer_, 466, 466);
    lv_obj_set_style_bg_color(alarm_layer_, lv_color_hex(0), 0);
    lv_obj_set_style_bg_opa(alarm_layer_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(alarm_layer_, LV_OBJ_FLAG_SCROLLABLE);
    alarm_heading_ = Label(alarm_layer_, large, 320, 73, 92);
    lv_label_set_text(alarm_heading_, "TIMER / CUE DUE");
    lv_obj_set_style_text_color(alarm_heading_, lv_color_hex(kRed), 0);
    alarm_ = Label(alarm_layer_, large, 330, 68, 160);
    lv_label_set_long_mode(alarm_, LV_LABEL_LONG_WRAP);
    auto* action = Label(alarm_layer_, small, 300, 83, 306);
    lv_label_set_text(action, "BLUE: ACK ONE ALERT");
    alarm_status_ = Label(alarm_layer_, small, 260, 103, 340);
    lv_label_set_long_mode(alarm_status_, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(alarm_layer_, LV_OBJ_FLAG_HIDDEN);
}

void ScheduleView::Render(const FaceModel& model, const TimeLabel& time_label, bool demo) {
    if (!root_)
        return;
    lv_obj_move_foreground(root_);
    const auto& scheduler = model.scheduler();
    const auto* snapshot = scheduler.snapshot();
    const bool trusted = scheduler.clock_state() == ClockState::Trusted;
    const auto now = scheduler.now_ms();
    const auto& slots = model.dial().slots();
    for (size_t i = 0; i < slots.size(); ++i) {
        const auto& slot = slots[i];
        const bool due =
            slot.occupied && slot.timer.status == ProvisionsTimerSnapshot::TimerStatus::kAttention;
        const uint32_t color = due ? kRed : kColors[i];
        lv_arc_set_value(
            arcs_[i],
            slot.occupied && trusted
                ? static_cast<int>(ProvisionsStopwatchOrbit::RemainingFraction(slot, now) * 1000)
                : 0);
        lv_obj_set_style_arc_color(arcs_[i], lv_color_hex(slot.occupied ? 0x303030 : 0x191919),
                                   LV_PART_MAIN);
        lv_obj_set_style_arc_color(arcs_[i], lv_color_hex(color), LV_PART_INDICATOR);
        lv_label_set_text(labels_[i], slot.occupied ? slot.timer.label.c_str() : "+");
        lv_obj_set_style_text_color(labels_[i], lv_color_hex(slot.occupied ? kWhite : 0x555555), 0);
        const auto text =
            !slot.occupied ? std::string{}
            : due          ? std::string{"DONE"}
            : !trusted     ? std::string{"--:--"}
                           : ProvisionsStopwatchOrbit::FormatRemaining(slot.timer.deadline_ms, now);
        lv_label_set_text(remaining_[i], text.c_str());
        lv_obj_set_style_text_color(remaining_[i], lv_color_hex(color), 0);
    }
    lv_label_set_text(title_, demo ? "DINNER / DEMO" : "SERVICE");
    const auto service = snapshot && time_label ? time_label(snapshot->service_at_ms) : "--:--";
    lv_label_set_text(service_, service.c_str());
    const Cue* next = nullptr;
    if (snapshot) {
        for (const auto& cue : snapshot->cues) {
            const auto state = std::find_if(scheduler.items().begin(), scheduler.items().end(),
                                            [&](const ItemState& s) { return s.key.id == cue.id; });
            if (state != scheduler.items().end() && !state->acknowledged &&
                (!next || (cue.kind == CueKind::ServiceOffset && next->kind != cue.kind) ||
                 (cue.kind == next->kind && cue.deadline_ms < next->deadline_ms)))
                next = &cue;
        }
    }
    const auto cue =
        next ? next->label + " " + (time_label ? time_label(next->deadline_ms) : "--:--")
             : "No cues";
    lv_label_set_text(cue_, cue.c_str());
    std::string status = !trusted ? "CHECK TIME" : scheduler.connected() ? "CONNECTED" : "OFFLINE";
    if (demo)
        status = "DEMO / " + status;
    if (!model.pending().empty())
        status += "\nACK PENDING " + std::to_string(model.pending().size());
    else if (model.receipt_confirmed())
        status += demo ? "\nMOCK ACK RECEIPT" : "\nACK CONFIRMED";
    if (model.dial().overflow_count())
        status += "\n+" + std::to_string(model.dial().overflow_count()) + " TIMERS";
    lv_label_set_text(status_, status.c_str());
    lv_obj_set_style_text_color(status_, lv_color_hex(kAmber), 0);
    std::string alert;
    lv_label_set_text(alarm_heading_, demo ? "DEMO / ALERT" : "TIMER / CUE DUE");
    if (!model.due().empty()) {
        alert = model.due().front().label;
        if (model.due().size() > 1)
            alert += "\n+" + std::to_string(model.due().size() - 1) + " MORE";
        lv_obj_remove_flag(alarm_layer_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(alarm_layer_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(alarm_, alert.c_str());
    lv_label_set_text(alarm_status_, status.c_str());
    lv_obj_set_style_text_color(alarm_status_, lv_color_hex(kAmber), 0);
}
}  // namespace orbit::service_schedule
