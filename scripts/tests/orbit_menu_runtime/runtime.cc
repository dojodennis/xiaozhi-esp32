// Runs the production renderer, assets and fonts against the pinned LVGL.
// ESP time/allocation are the only platform substitutes; no physical proof.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "lvgl.h"
#include "menu_crest.h"
#include "menu_asset.h"
#include "add_face.h"

uint32_t test_time_ms = 0;
constexpr int kSize = 466;
static uint16_t framebuffer[kSize * kSize];
static uint16_t draw_buffer[kSize * kSize];

static void Flush(lv_display_t* display, const lv_area_t* area, uint8_t* data) {
    auto* pixels = reinterpret_cast<uint16_t*>(data);
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x)
            framebuffer[y * kSize + x] = *pixels++;
    lv_display_flush_ready(display);
}

static void Advance(uint32_t duration) {
    for (uint32_t elapsed = 0; elapsed < duration; elapsed += 10) {
        test_time_ms += 10;
        lv_tick_inc(10);
        lv_timer_handler();
    }
    lv_refr_now(nullptr);
}

static void Save(const std::filesystem::path& path) {
    std::ofstream stream(path, std::ios::binary);
    stream << "P6\n" << kSize << ' ' << kSize << "\n255\n";
    for (auto pixel : framebuffer) {
        const char rgb[] = {char(((pixel >> 11) & 31) * 255 / 31),
                            char(((pixel >> 5) & 63) * 255 / 63),
                            char((pixel & 31) * 255 / 31)};
        stream.write(rgb, 3);
    }
}

static std::vector<uint16_t> Frame() {
    return {std::begin(framebuffer), std::end(framebuffer)};
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path output(argv[1]);
    std::filesystem::create_directories(output);
    lv_init();
    auto* display = lv_display_create(kSize, kSize);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, nullptr, sizeof(draw_buffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, Flush);
    auto* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    // Actual empty-screen renderer uses the same objects/assets as Home.
    auto* band = lv_image_create(screen);
    lv_image_set_src(band, &OrbitCrest::kBandImage);
    lv_obj_set_pos(band, OrbitCrest::kBandX, OrbitCrest::kBandY);
    auto* star = lv_image_create(screen);
    lv_image_set_src(star, &OrbitCrest::kStarImage);
    lv_obj_set_pos(star, OrbitCrest::kStarX, OrbitCrest::kStarY);
    for (auto* mark : {band, star}) {
        lv_obj_set_style_image_recolor(mark, lv_color_hex(OrbitCrest::kIvory), 0);
        lv_obj_set_style_image_recolor_opa(mark, LV_OPA_COVER, 0);
    }
    Advance(10);
    auto home = Frame();
    Save(output / "home.ppm");
    auto* title = lv_label_create(screen);
    auto* hint = lv_label_create(screen);
    for (const char* destination : {"List", "Notes", "List"}) {
        ProvisionsStopWatch::PaintAddFace(band, star, title, hint, destination);
        Advance(10);
        assert(lv_obj_get_x(band) == OrbitCrest::kBandX);
        assert(lv_obj_get_y(star) == OrbitCrest::kStarY);
        auto ready = Frame();
        // Only the two labels below the star may differ from Home.
        for (int y = 0; y < kSize; ++y)
            for (int x = 0; x < kSize; ++x)
                if (y < 287 || y > 336)
                    assert(ready[y * kSize + x] == home[y * kSize + x]);
        Save(output / (std::string("add-") + destination + ".ppm"));
        // The same crest objects can transition into existing listening motion.
        const auto listening = OrbitCrest::Rings(OrbitCrest::State::Listening, 500, .5F, false);
        const auto start = OrbitCrest::Transition(OrbitCrest::Frame{}, listening, 0, false);
        assert(start.band_opacity == 255 && start.star_opacity == 255);
        // Re-entering the empty view restores a previously dimmed crest.
        lv_obj_set_style_image_opa(band, 20, 0);
        lv_obj_set_style_image_opa(star, 20, 0);
    }
    lv_obj_add_flag(title, LV_OBJ_FLAG_HIDDEN);
    ProvisionsStopWatch::StyleVoiceCaption(hint, true);
    lv_label_set_text(hint, ProvisionsStopWatch::kStockReadyCaption);
    lv_obj_set_style_text_opa(hint, LV_OPA_COVER, 0);
    for (auto* mark : {band, star})
        lv_obj_set_style_image_opa(mark, LV_OPA_COVER, 0);
    Advance(10);
    Save(output / "stock-ready.ppm");
    auto stock_ready = Frame();
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x)
            if (y < 287 || y > 336)
                assert(stock_ready[y * kSize + x] == home[y * kSize + x]);
    ProvisionsStopWatch::StyleVoiceCaption(hint, false);
    Advance(10);
    assert(lv_obj_get_height(hint) == 74);  // Voice/error state restores full caption space.
    // Real production font/style must fit every allowlisted progress caption.
    for (const char* status : {"Working", "Saving", "Retry queued", "Preparing microphone"}) {
        const auto* caption =
            OrbitCrest::Caption(OrbitCrest::State::Thinking, OrbitCrest::ProgressForStatus(status));
        lv_label_set_text(hint, caption);
        lv_point_t text_size{};
        lv_text_get_size(&text_size, caption, &font_noto_sans_basic_30_4, 0, 3, 280,
                         LV_TEXT_FLAG_NONE);
        assert(text_size.x <= 280 && text_size.y <= 74);
        Advance(10);
        assert(!std::strcmp(lv_label_get_text(hint), caption));
        Save(output / (std::string("progress-") + status + ".ppm"));
    }
    lv_label_set_text(hint, ProvisionsStopWatch::kStockReadyCaption);
    ProvisionsStopWatch::StyleVoiceCaption(hint, true);
    Advance(10);
    assert(Frame() == stock_ready);  // Returning from a reply restores Stock.
    for (auto* obj : {band, star, title, hint}) lv_obj_delete(obj);
    ProvisionsStopWatch::OrbitMenuCrest crest;
    crest.Create(screen);
    crest.Show(0, 0, "--:--");
    Advance(900);
    auto list = Frame();
    Save(output / "k-list.ppm");
    crest.Show(2, 0, "--:--");
    Advance(900);
    auto notes = Frame();
    Save(output / "k-notes.ppm");
    assert(list != notes);
    crest.Show(3, 0, "--:--");
    Advance(900);
    auto stock = Frame();
    assert(stock != list && stock != notes);
    Save(output / "k-stock.ppm");
    crest.Show(1, 0, "--:--");
    Advance(900);
    auto empty = Frame();
    Save(output / "k-timers-empty.ppm");
    // With no running timer, the entire frame must stay still (no gold pulse).
    for (int i = 0; i < 30; ++i) {
        Advance(80);
        if (Frame() != empty) {
            Save(output / "k-timers-empty-unexpected-pulse.ppm");
            std::fprintf(stderr, "FAIL: empty timer display animates without a timer\n");
            return 1;
        }
    }
    crest.UpdateTimer(500, "00:30");
    Advance(800);
    auto half = Frame();
    Save(output / "k-timers-half.ppm");
    assert(half != empty);
    Advance(800);
    assert(Frame() != half);  // A running timer's leading edge breathes.
    crest.UpdateTimer(750, "00:15");
    Advance(800);
    assert(Frame() != half);
    Save(output / "k-timers-live.ppm");
    crest.UpdateTimer(0, "--:--");
    Advance(800);
    assert(Frame() == empty);  // Last timer removed while menu remains open.
    crest.Hide();
    assert(!crest.Visible());
    Advance(900);
    for (auto pixel : framebuffer) assert(pixel == 0);

    std::vector<uint16_t> v_pixels(kSize * kSize);
    lv_image_dsc_t v_image{};
    v_image.header.magic = LV_IMAGE_HEADER_MAGIC;
    v_image.header.cf = LV_COLOR_FORMAT_RGB565;
    v_image.header.w = kSize;
    v_image.header.h = kSize;
    v_image.header.stride = kSize * 2;
    v_image.data_size = v_pixels.size() * 2;
    v_image.data = reinterpret_cast<const uint8_t*>(v_pixels.data());
    auto* image = lv_image_create(screen);
    const char* names[] = {"list", "timers", "notes"};
    for (int cycle = 0; cycle < 4; ++cycle) {
        for (int page = 0; page < 3; ++page) {
            crest.Hide();
            for (size_t i = 0; i < v_pixels.size(); ++i)
                v_pixels[i] = OrbitMenu::kPages[page].palette[OrbitMenu::kPages[page].pixels[i]];
            lv_image_set_src(image, &v_image);
            lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(image);
            Advance(800);
            if (!cycle) Save(output / (std::string("v-") + names[page] + ".ppm"));
            lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
            crest.Show(page, page == 1 ? 500 : 0, "00:30");
            Advance(900);
            assert(crest.Visible());
            if (page == 0) assert(Frame() == list);
            if (page == 2) assert(Frame() == notes);
        }
    }
    crest.Hide();
    std::puts("PASS: add-face Home geometry and restoration; K/V pages, idle timer, live progress, animation, removal and 12 switches");
}
