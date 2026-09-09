// Local stdin driver for the exact board demo model, alarm engine and LVGL view.
// All dates, enrollment identifiers and receipt events are synthetic fixtures.
#include "cJSON.h"
#include "service_schedule_demo.h"
#include "service_schedule_view.h"

#include <array>
#include <charconv>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>

LV_FONT_DECLARE(font_noto_sans_basic_16_4);
LV_FONT_DECLARE(font_noto_sans_basic_30_4);

namespace {
constexpr int kSize = 466;
std::array<uint32_t, kSize * kSize> draw_buffer{};
std::array<uint32_t, kSize * kSize> frame{};
unsigned flushes = 0;

void Flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
    const auto* source = reinterpret_cast<const uint32_t*>(pixels);
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x)
            frame[y * kSize + x] = *source++;
    ++flushes;
    lv_display_flush_ready(display);
}

void Labels(lv_obj_t* object, cJSON* output) {
    if (lv_obj_check_type(object, &lv_label_class))
        cJSON_AddItemToArray(output, cJSON_CreateString(lv_label_get_text(object)));
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
        Labels(lv_obj_get_child(object, i), output);
}

bool WriteFrame(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file)
        return false;
    std::fprintf(file, "P6\n%d %d\n255\n", kSize, kSize);
    for (const auto pixel : frame) {
        const unsigned char rgb[] = {static_cast<unsigned char>(pixel >> 16),
                                     static_cast<unsigned char>(pixel >> 8),
                                     static_cast<unsigned char>(pixel)};
        if (std::fwrite(rgb, 1, 3, file) != 3) {
            std::fclose(file);
            return false;
        }
    }
    return std::fclose(file) == 0;
}

void Emit(orbit::service_schedule::ScheduleDemo& demo, bool accepted, lv_display_t* display,
          orbit::service_schedule::ScheduleView& view, const std::string& frame_path) {
    unsigned zero_time_calls = 0;
    view.Render(
        demo.model(),
        [&](int64_t time) {
            if (time == 0) {
                ++zero_time_calls;
                return std::string{"INVALID ZERO TIME"};
            }
            return demo.FixtureTime(time);
        },
        true);
    lv_obj_update_layout(lv_screen_active());
    lv_tick_inc(40);
    lv_refr_now(display);
    const auto& model = demo.model();
    const auto& scheduler = model.scheduler();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "accepted", accepted);
    cJSON_AddNumberToObject(root, "zero_time_calls", zero_time_calls);
    cJSON_AddBoolToObject(root, "valid", demo.valid());
    cJSON_AddStringToObject(root, "stage", demo.stage());
    cJSON_AddBoolToObject(root, "connected", scheduler.connected());
    cJSON_AddBoolToObject(root, "clock_trusted",
                          scheduler.clock_state() == orbit::service_schedule::ClockState::Trusted);
    cJSON_AddNumberToObject(root, "now_ms", static_cast<double>(scheduler.now_ms()));
    cJSON_AddBoolToObject(root, "alarm_active", model.alarm_active());
    cJSON_AddBoolToObject(root, "receipt_confirmed", model.receipt_confirmed());
    const auto change = demo.model().TakeOutputChange();
    cJSON_AddStringToObject(root, "alarm_change",
                            change == ProvisionsStopwatchOrbit::AlarmOutputChange::kStart ? "start"
                            : change == ProvisionsStopwatchOrbit::AlarmOutputChange::kStop
                                ? "stop"
                                : "none");
    cJSON_AddNumberToObject(root, "flushes", flushes);
    cJSON_AddBoolToObject(root, "frame_written", WriteFrame(frame_path));
    auto* labels = cJSON_AddArrayToObject(root, "labels");
    Labels(lv_screen_active(), labels);
    auto* due = cJSON_AddArrayToObject(root, "due");
    for (const auto& timer : model.due())
        cJSON_AddItemToArray(due, cJSON_CreateString(timer.label.c_str()));
    auto* pending = cJSON_AddArrayToObject(root, "pending");
    for (const auto& key : model.pending()) {
        auto* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", key.id.c_str());
        cJSON_AddNumberToObject(item, "revision", static_cast<double>(key.revision));
        cJSON_AddItemToArray(pending, item);
    }
    if (const auto* snapshot = scheduler.snapshot()) {
        cJSON_AddNumberToObject(root, "snapshot_revision",
                                static_cast<double>(snapshot->snapshot_revision));
        cJSON_AddNumberToObject(root, "service_at_ms",
                                static_cast<double>(snapshot->service_at_ms));
        auto* deadlines = cJSON_AddObjectToObject(root, "deadlines");
        for (const auto& timer : snapshot->timers)
            cJSON_AddNumberToObject(deadlines, timer.label.c_str(),
                                    static_cast<double>(timer.deadline_ms));
        for (const auto& cue : snapshot->cues)
            cJSON_AddNumberToObject(deadlines, cue.label.c_str(),
                                    static_cast<double>(cue.deadline_ms));
    }
    char* json = cJSON_PrintUnformatted(root);
    std::cout << json << std::endl;
    cJSON_free(json);
    cJSON_Delete(root);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: service_schedule_demo_host FRAME.ppm\n";
        return 2;
    }
    lv_init();
    auto* display = lv_display_create(kSize, kSize);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(display, draw_buffer.data(), nullptr, sizeof(draw_buffer),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, Flush);
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0), 0);
    orbit::service_schedule::ScheduleView view;
    view.Create(lv_screen_active(), &font_noto_sans_basic_30_4, &font_noto_sans_basic_16_4);
    auto demo = std::make_unique<orbit::service_schedule::ScheduleDemo>();
    Emit(*demo, demo->valid(), display, view, argv[1]);
    std::string command;
    while (std::getline(std::cin, command)) {
        if (command == "quit")
            break;
        bool accepted = false;
        if (command == "next")
            accepted = demo->Advance();
        else if (command.rfind("tick ", 0) == 0) {
            int64_t delta = 0;
            const auto* end = command.data() + command.size();
            const auto parsed = std::from_chars(command.data() + 5, end, delta);
            if (parsed.ec == std::errc{} && parsed.ptr == end && delta >= 0 && delta <= 3'600'000) {
                demo->Elapse(delta);
                accepted = true;
            }
        } else if (command == "absent_service") {
            // Host-only fixture transition through the actual model and view.
            auto snapshot = *demo->model().scheduler().snapshot();
            snapshot.version = 2;
            snapshot.server_now_ms = 0;
            snapshot.service_occurrence_id.clear();
            snapshot.service_revision = 0;
            snapshot.service_at_ms = 0;
            snapshot.timezone.clear();
            snapshot.cues.clear();
            ++snapshot.snapshot_revision;
            accepted = demo->model().ApplyVerified(snapshot, 0) ==
                       orbit::service_schedule::ApplyResult::Applied;
        } else if (command == "ack")
            accepted = demo->Acknowledge();
        else if (command == "reset") {
            demo = std::make_unique<orbit::service_schedule::ScheduleDemo>();
            accepted = demo->valid();
        } else if (command == "wrong_receipt" && !demo->model().pending().empty()) {
            auto key = demo->model().pending().front();
            ++key.revision;
            accepted = demo->model().AcceptReceipt(key);
        } else if (command == "receipt" && !demo->model().pending().empty()) {
            const auto key = demo->model().pending().front();
            accepted = demo->model().AcceptReceipt(key);
        }
        Emit(*demo, accepted, display, view, argv[1]);
    }
    lv_display_delete(display);
    lv_deinit();
}
