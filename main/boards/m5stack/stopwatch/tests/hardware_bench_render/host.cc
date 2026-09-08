#include "cJSON.h"
#include "exact_board_renderer.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <thread>

using namespace orbit::service_schedule;
using namespace orbit::service_schedule::storage;
using namespace std::chrono_literals;
namespace {
constexpr int kSize = 466;
std::array<uint32_t, kSize * kSize> buffer{}, frame{};
void Flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
    const auto* source = reinterpret_cast<const uint32_t*>(pixels);
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x)
            frame[y * kSize + x] = *source++;
    lv_display_flush_ready(display);
}
void WriteFrame(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "wb");
    assert(file);
    std::fprintf(file, "P6\n%d %d\n255\n", kSize, kSize);
    for (auto pixel : frame) {
        const unsigned char rgb[] = {static_cast<unsigned char>(pixel >> 16),
                                     static_cast<unsigned char>(pixel >> 8),
                                     static_cast<unsigned char>(pixel)};
        assert(std::fwrite(rgb, 1, 3, file) == 3);
    }
    assert(std::fclose(file) == 0);
}
void Labels(lv_obj_t* object, cJSON* records) {
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN))
        return;
    if (lv_obj_check_type(object, &lv_label_class)) {
        auto* record = cJSON_CreateObject();
        lv_area_t bounds;
        lv_obj_get_coords(object, &bounds);
        cJSON_AddStringToObject(record, "text", lv_label_get_text(object));
        auto* rectangle = cJSON_AddArrayToObject(record, "bounds");
        for (int coordinate : {bounds.x1, bounds.y1, bounds.x2, bounds.y2})
            cJSON_AddItemToArray(rectangle, cJSON_CreateNumber(coordinate));
        cJSON_AddItemToArray(records, record);
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
        Labels(lv_obj_get_child(object, i), records);
}
struct Disk {
    std::mutex mutex;
    std::optional<Bytes> bytes;
    bool uncertain = false, io_error = false;
};
class Store : public WorkerStore {
    std::shared_ptr<Disk> disk_;

public:
    explicit Store(std::shared_ptr<Disk> disk) : disk_(std::move(disk)) {}
    LoadResult Load(FacePersistentState& output, Bytes& bytes) override {
        std::lock_guard<std::mutex> lock(disk_->mutex);
        if (disk_->io_error)
            return LoadResult::IoError;
        if (!disk_->bytes)
            return LoadResult::Absent;
        if (Decode(disk_->bytes->data(), disk_->bytes->size(), HardwareBenchScope(), output) !=
            CodecResult::Accepted)
            return LoadResult::Corrupt;
        bytes = *disk_->bytes;
        return LoadResult::Present;
    }
    SaveResult Transition(const Bytes* expected, const FacePersistentState& state,
                          Bytes& verified) override {
        std::lock_guard<std::mutex> lock(disk_->mutex);
        assert(expected ? disk_->bytes && *expected == *disk_->bytes : !disk_->bytes);
        if (disk_->uncertain)
            return SaveResult::Uncertain;
        assert(Encode(state, HardwareBenchScope(), verified) == CodecResult::Accepted);
        disk_->bytes = verified;
        return SaveResult::Saved;
    }
};
AlarmOutputHooks Hooks(bool audio_ok = true) {
    return {[audio_ok] { return audio_ok; }, [] {}, [] { return true; }, [] { return uint32_t{0}; },
            [](bool) {}};
}
template <typename Predicate>
void Pump(HardwareBench& bench, int64_t now, Predicate until) {
    for (unsigned i = 0; i < 5000; ++i) {
        bench.Poll(now);
        if (until())
            return;
        std::this_thread::sleep_for(1ms);
    }
    assert(false);
}
void Emit(ExactBoardRenderer& renderer, HardwareBench& bench, lv_display_t* display,
          const std::string& directory, const std::string& name) {
    renderer.RenderHardwareBench(bench.current(), bench.Status());
    lv_obj_update_layout(lv_screen_active());
    lv_tick_inc(40);
    // Capture must include unchanged widgets after state changes; force an
    // entire-screen redraw instead of relying on accumulated dirty regions.
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(display);
    const auto first_complete_frame = frame;
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(display);
    assert(frame == first_complete_frame);  // No animation/time change between redraws.
    WriteFrame(directory + "/" + name + ".ppm");
    auto* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ppm", (name + ".ppm").c_str());
    cJSON_AddStringToObject(root, "status", bench.Status().c_str());
    cJSON_AddNumberToObject(root, "due", bench.current() ? bench.current()->face->due().size() : 0);
    cJSON_AddNumberToObject(root, "pending",
                            bench.current() ? bench.current()->face->pending().size() : 0);
    Labels(lv_screen_active(), cJSON_AddArrayToObject(root, "visible_labels"));
    char* text = cJSON_PrintUnformatted(root);
    std::cout << text << std::endl;
    cJSON_free(text);
    cJSON_Delete(root);
}
}  // namespace
int main(int argc, char** argv) {
    assert(argc == 2);
    lv_init();
    auto* display = lv_display_create(kSize, kSize);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(display, buffer.data(), nullptr, sizeof(buffer),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, Flush);
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0), 0);
    ExactBoardRenderer renderer;
    renderer.CreateOrbitUiLocked(lv_screen_active());
    auto disk = std::make_shared<Disk>();
    Bytes running_record;
    {
        HardwareBench bench(std::make_unique<Store>(disk), Hooks(), 401);
        Emit(renderer, bench, display, argv[1], "00-reading-storage");
        Pump(bench, 0, [&] { return bench.current() != nullptr; });
        Emit(renderer, bench, display, argv[1], "01-boot-absent");
        bench.Yellow(bench.Observed(), 0);
        Pump(bench, 0, [&] { return bench.current()->face->scheduler().snapshot() != nullptr; });
        Emit(renderer, bench, display, argv[1], "02-six-running");
        {
            std::lock_guard<std::mutex> lock(disk->mutex);
            running_record = *disk->bytes;
        }
        Pump(bench, 15000, [&] { return bench.current()->face->due().size() == 6; });
        Emit(renderer, bench, display, argv[1], "03-six-due");
        bench.Blue(bench.Observed());
        Pump(bench, 15000, [&] { return bench.current()->face->pending().size() == 1; });
        Emit(renderer, bench, display, argv[1], "04-one-ack-five-due");
        {
            std::lock_guard<std::mutex> lock(disk->mutex);
            disk->uncertain = true;
        }
        bench.Blue(bench.Observed());
        Pump(bench, 15000, [&] { return bench.current()->recovery_required; });
        Emit(renderer, bench, display, argv[1], "05-storage-uncertain-due");
    }
    {
        HardwareBench bench(std::make_unique<Store>(disk), Hooks(), 402);
        Pump(bench, 0, [&] { return bench.current() != nullptr; });
        Emit(renderer, bench, display, argv[1], "06-restored-due-needs-sync");
    }
    {
        HardwareBench bench(std::make_unique<Store>(disk), Hooks(false), 403);
        Pump(bench, 0, [&] { return bench.current() != nullptr; });
        Emit(renderer, bench, display, argv[1], "07-audio-fault-due");
    }
    disk->bytes = running_record;
    {
        HardwareBench bench(std::make_unique<Store>(disk), Hooks(), 404);
        Pump(bench, 0, [&] { return bench.current() != nullptr; });
        Emit(renderer, bench, display, argv[1], "08-restored-future-needs-sync");
    }
    disk->bytes = Bytes{1};
    {
        HardwareBench bench(std::make_unique<Store>(disk), Hooks(), 405);
        Pump(bench, 0, [&] { return bench.current() != nullptr; });
        Emit(renderer, bench, display, argv[1], "09-storage-corrupt");
    }
    disk->io_error = true;
    {
        HardwareBench bench(std::make_unique<Store>(disk), Hooks(), 406);
        Pump(bench, 0, [&] { return bench.current() != nullptr; });
        Emit(renderer, bench, display, argv[1], "10-storage-io-error");
    }
    lv_display_delete(display);
    lv_deinit();
}
