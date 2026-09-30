#include "menu_crest.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "crest_asset.h"

LV_FONT_DECLARE(font_noto_sans_basic_16_4);
LV_FONT_DECLARE(font_noto_sans_basic_30_4);

namespace ProvisionsStopWatch {
namespace {
constexpr const char* TAG = "menu_crest";
constexpr int kScreen = 466;
constexpr int kCentre = 233;
constexpr uint32_t kArriveMs = 360;      // the crest transition
constexpr uint32_t kBreathPeriodMs = 2400;  // the listening face's breath
constexpr uint32_t kBreathTickMs = 80;
constexpr int kBreathLeadTenths = 140;   // ivory behind the leading edge
constexpr int kBreathTrailTenths = 50;   // and just ahead of it
constexpr int kCaptionY = 66;
constexpr int kDotY = 90;

struct Bar {
    int dx, dy, width;
    uint32_t color;
};
// Rows around y −4 (the star's centre), as drawn on the design sheet.
constexpr Bar kListBars[3] = {{-32, -38, 96, OrbitMenuCrest::kIvory},
                              {-32, -6, 96, OrbitMenuCrest::kIvory},
                              {-32, 26, 64, OrbitMenuCrest::kIvory}};
constexpr Bar kNotesBars[3] = {{-44, -40, 44, OrbitMenuCrest::kGold},
                               {-44, -10, 84, OrbitMenuCrest::kIvory},
                               {-44, 20, 62, OrbitMenuCrest::kIvory}};
constexpr int kListBulletX = -50;

uint16_t Rgb565(uint32_t rgb, int alpha) {
    const uint32_t r = ((rgb >> 16) & 0xFF) * alpha / 255;
    const uint32_t g = ((rgb >> 8) & 0xFF) * alpha / 255;
    const uint32_t b = (rgb & 0xFF) * alpha / 255;
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

uint32_t Mix(uint32_t from, uint32_t to, float amount) {
    uint32_t out = 0;
    for (int shift : {16, 8, 0}) {
        const int a = static_cast<int>((from >> shift) & 0xFF);
        const int b = static_cast<int>((to >> shift) & 0xFF);
        out |= static_cast<uint32_t>(std::lround(a + (b - a) * amount)) << shift;
    }
    return out;
}

lv_obj_t* Pill(lv_obj_t* parent, int width, int height, uint32_t color) {
    lv_obj_t* pill = lv_obj_create(parent);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, width, height);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    return pill;
}

void SetShown(lv_obj_t* object, bool visible) {
    if (object == nullptr)
        return;
    if (visible)
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}
}  // namespace

void OrbitMenuCrest::Create(lv_obj_t* parent) {
    layer_ = lv_obj_create(parent);
    lv_obj_remove_style_all(layer_);
    lv_obj_set_size(layer_, kScreen, kScreen);
    lv_obj_set_pos(layer_, 0, 0);
    lv_obj_set_style_bg_color(layer_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(layer_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(layer_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(layer_, LV_OBJ_FLAG_CLICKABLE);

    band_ = lv_image_create(layer_);
    lv_image_set_src(band_, &OrbitCrest::kBandImage);
    lv_obj_set_pos(band_, kBandX, kBandY);
    lv_image_set_pivot(band_, kBandSize / 2, kBandSize / 2);
    lv_obj_set_style_image_recolor(band_, lv_color_hex(kGold), 0);
    lv_obj_set_style_image_recolor_opa(band_, LV_OPA_COVER, 0);

    band_timer_ = lv_image_create(layer_);
    lv_obj_set_pos(band_timer_, kBandX, kBandY);
    SetShown(band_timer_, false);

    star_ = lv_image_create(layer_);
    lv_image_set_src(star_, &OrbitCrest::kStarImage);
    lv_obj_set_pos(star_, OrbitCrest::kStarX, OrbitCrest::kStarY);
    lv_obj_set_style_image_recolor(star_, lv_color_hex(kIvory), 0);
    lv_obj_set_style_image_recolor_opa(star_, LV_OPA_COVER, 0);

    for (int i = 0; i < 3; ++i) {
        bullets_[i] = Pill(layer_, 10, 10, kGold);
        bars_[i] = Pill(layer_, 10, 10, kIvory);
        dots_[i] = Pill(layer_, 6, 6, kDim);
        lv_obj_align(dots_[i], LV_ALIGN_CENTER, (i - 1) * 16, kDotY);
    }

    readout_ = lv_label_create(layer_);
    lv_obj_set_style_text_font(readout_, &font_noto_sans_basic_30_4, 0);
    lv_obj_set_style_text_color(readout_, lv_color_hex(kIvory), 0);
    lv_obj_set_style_text_letter_space(readout_, 1, 0);
    lv_obj_set_style_text_align(readout_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(readout_, 200, 40);
    lv_obj_align(readout_, LV_ALIGN_CENTER, 0, -4);

    caption_ = lv_label_create(layer_);
    lv_obj_set_style_text_font(caption_, &font_noto_sans_basic_16_4, 0);
    lv_obj_set_style_text_color(caption_, lv_color_hex(kIvory), 0);
    lv_obj_set_style_text_opa(caption_, LV_OPA_80, 0);
    lv_obj_set_style_text_letter_space(caption_, 2, 0);
    lv_obj_set_style_text_align(caption_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(caption_, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(caption_, 200, 24);
    lv_obj_align(caption_, LV_ALIGN_CENTER, 0, kCaptionY);

    SetShown(layer_, false);
}

void OrbitMenuCrest::SetOpa(void* object, int32_t value) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(object), static_cast<lv_opa_t>(value), 0);
}

void OrbitMenuCrest::SetWidth(void* object, int32_t value) {
    lv_obj_set_width(static_cast<lv_obj_t*>(object), value);
}

void OrbitMenuCrest::SetSweep(void* self, int32_t value) {
    auto* menu = static_cast<OrbitMenuCrest*>(self);
    menu->sweep_permille_ = value;
    menu->ComposeBand(value, 0.0F);
}

void OrbitMenuCrest::Animate(lv_obj_t* object, lv_anim_exec_xcb_t exec, int32_t from,
                             int32_t to, uint32_t duration_ms, uint32_t delay_ms) {
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, object);
    lv_anim_set_exec_cb(&anim, exec);
    lv_anim_set_values(&anim, from, to);
    lv_anim_set_duration(&anim, duration_ms);
    lv_anim_set_delay(&anim, delay_ms);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_start(&anim);
}

void OrbitMenuCrest::AnimateSweep(int32_t from, int32_t to, uint32_t duration_ms) {
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, this);
    lv_anim_set_exec_cb(&anim, SetSweep);
    lv_anim_set_values(&anim, from, to);
    lv_anim_set_duration(&anim, duration_ms);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_start(&anim);
}

void OrbitMenuCrest::StopAnimations() {
    lv_anim_delete(this, nullptr);
    lv_anim_delete(star_, nullptr);
    lv_anim_delete(band_, nullptr);
    for (int i = 0; i < 3; ++i) {
        lv_anim_delete(bullets_[i], nullptr);
        lv_anim_delete(bars_[i], nullptr);
    }
    if (breath_timer_ != nullptr)
        lv_timer_pause(breath_timer_);
}

// The composed band needs two PSRAM tables: the RGB565 picture and, once,
// every pixel's angle from twelve so a frame is a compare, not an atan2.
bool OrbitMenuCrest::EnsureTimerBuffers() {
    constexpr size_t kPixels = size_t(kBandSize) * kBandSize;
    if (timer_pixels_ != nullptr)
        return true;
    auto* pixels = static_cast<uint16_t*>(
        heap_caps_malloc(kPixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* angles = static_cast<uint16_t*>(
        heap_caps_malloc(kPixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pixels == nullptr || angles == nullptr) {
        heap_caps_free(pixels);
        heap_caps_free(angles);
        ESP_LOGW(TAG, "timer band: no PSRAM for %u bytes", unsigned(2 * kPixels * 2));
        return false;
    }
    const auto* alpha = OrbitCrest::kBandImage.data;
    const float half = kBandSize / 2.0F - 0.5F;
    for (int y = 0; y < kBandSize; ++y) {
        for (int x = 0; x < kBandSize; ++x) {
            const size_t i = size_t(y) * kBandSize + x;
            if (alpha[i] == 0) {
                angles[i] = 0;
                continue;
            }
            float degrees = std::atan2(x - half, half - y) * 180.0F / 3.14159265F;
            if (degrees < 0)
                degrees += 360.0F;
            angles[i] = static_cast<uint16_t>(std::min(3599L, std::lround(degrees * 10.0F)));
        }
    }
    timer_pixels_ = pixels;
    angle_tenths_ = angles;
    timer_image_.header.magic = LV_IMAGE_HEADER_MAGIC;
    timer_image_.header.cf = LV_COLOR_FORMAT_RGB565;
    timer_image_.header.w = kBandSize;
    timer_image_.header.h = kBandSize;
    timer_image_.header.stride = kBandSize * sizeof(uint16_t);
    timer_image_.data_size = kPixels * sizeof(uint16_t);
    timer_image_.data = reinterpret_cast<const uint8_t*>(timer_pixels_);
    lv_image_set_src(band_timer_, &timer_image_);
    shown_sweep_permille_ = -1;
    return true;
}

void OrbitMenuCrest::ComposeBand(int sweep_permille, float breath) {
    if (timer_pixels_ == nullptr)
        return;
    sweep_permille = std::clamp(sweep_permille, 0, 1000);
    // With no elapsed sweep there is no leading edge to breathe. Keep the
    // empty timer band dim and avoid repainting it on every breath tick.
    if (sweep_permille == 0)
        breath = 0.0F;
    if (sweep_permille == shown_sweep_permille_ && breath == shown_breath_)
        return;
    shown_sweep_permille_ = sweep_permille;
    shown_breath_ = breath;
    const int sweep_tenths = sweep_permille * 3600 / 1000;
    const int lead_from = sweep_tenths - kBreathLeadTenths;
    const int lead_to = std::min(3600, sweep_tenths + kBreathTrailTenths);
    const uint32_t edge = Mix(kGold, kIvory, 0.45F * breath);
    const auto* alpha = OrbitCrest::kBandImage.data;
    constexpr size_t kPixels = size_t(kBandSize) * kBandSize;
    for (size_t i = 0; i < kPixels; ++i) {
        const int a = alpha[i];
        if (a == 0) {
            timer_pixels_[i] = 0;
            continue;
        }
        const int angle = angle_tenths_[i];
        uint32_t color = angle < sweep_tenths ? kGold : kTrack;
        if (breath > 0.0F && angle >= lead_from && angle < lead_to)
            color = edge;
        timer_pixels_[i] = Rgb565(color, a);
    }
    lv_obj_invalidate(band_timer_);
}

void OrbitMenuCrest::BreathTick(lv_timer_t* timer) {
    auto* menu = static_cast<OrbitMenuCrest*>(lv_timer_get_user_data(timer));
    if (menu == nullptr || !menu->visible_ || menu->page_ != 1)
        return;
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    const float phase = float((now - menu->breath_started_ms_) % kBreathPeriodMs) / kBreathPeriodMs;
    const float breath = 0.5F - 0.5F * std::cos(2.0F * 3.14159265F * phase);
    menu->ComposeBand(menu->sweep_permille_, breath);
}

void OrbitMenuCrest::Show(int page, int sweep_permille, const char* readout) {
    if (layer_ == nullptr)
        return;
    StopAnimations();
    const bool arriving = !visible_ || page != page_;
    page_ = page;
    visible_ = true;
    SetShown(layer_, true);
    lv_obj_move_foreground(layer_);

    static const char* const kCaptions[3] = {"List", "Timers", "Notes"};
    lv_label_set_text(caption_, kCaptions[std::clamp(page, 0, 2)]);
    for (int i = 0; i < 3; ++i)
        lv_obj_set_style_bg_color(dots_[i], lv_color_hex(i == page ? kGold : kDim), 0);

    const bool timers = page == 1;
    const bool notes = page == 2;
    const bool composed = timers && EnsureTimerBuffers();
    SetShown(band_, !composed);
    SetShown(band_timer_, composed);
    lv_image_set_rotation(band_, notes ? kNotesBandTenths : 0);
    lv_obj_set_style_opa(band_, LV_OPA_COVER, 0);

    // The star fades as the page arrives.
    SetShown(star_, true);
    if (arriving)
        Animate(star_, SetOpa, LV_OPA_COVER, LV_OPA_TRANSP, kArriveMs, 0);
    else
        lv_obj_set_style_opa(star_, LV_OPA_TRANSP, 0);

    const Bar* rows = notes ? kNotesBars : kListBars;
    for (int i = 0; i < 3; ++i) {
        const bool used = !timers;
        SetShown(bars_[i], used);
        SetShown(bullets_[i], page == 0);
        if (!used)
            continue;
        lv_obj_set_style_bg_color(bars_[i], lv_color_hex(rows[i].color), 0);
        lv_obj_align(bars_[i], LV_ALIGN_TOP_LEFT, kCentre + rows[i].dx, kCentre + rows[i].dy - 5);
        lv_obj_align(bullets_[i], LV_ALIGN_TOP_LEFT, kCentre + kListBulletX - 5,
                     kCentre + rows[i].dy - 5);
        if (arriving) {
            lv_obj_set_width(bars_[i], 10);
            Animate(bars_[i], SetWidth, 10, rows[i].width, 240, uint32_t(i) * 120);
            lv_obj_set_style_opa(bullets_[i], LV_OPA_TRANSP, 0);
            Animate(bullets_[i], SetOpa, LV_OPA_TRANSP, LV_OPA_COVER, 160, uint32_t(i) * 120);
        } else {
            lv_obj_set_width(bars_[i], rows[i].width);
            lv_obj_set_style_opa(bullets_[i], LV_OPA_COVER, 0);
        }
    }

    SetShown(readout_, timers);
    if (timers) {
        lv_label_set_text(readout_, readout != nullptr ? readout : "--:--");
        sweep_permille_ = std::clamp(sweep_permille, 0, 1000);
        if (composed) {
            if (arriving) {
                ComposeBand(0, 0.0F);
                AnimateSweep(0, sweep_permille_, kArriveMs);
            } else {
                ComposeBand(sweep_permille_, 0.0F);
            }
            breath_started_ms_ = static_cast<uint32_t>(esp_timer_get_time() / 1000) + kArriveMs;
            if (breath_timer_ == nullptr)
                breath_timer_ = lv_timer_create(BreathTick, kBreathTickMs, this);
            lv_timer_resume(breath_timer_);
        }
    }
}

void OrbitMenuCrest::UpdateTimer(int sweep_permille, const char* readout) {
    if (!visible_ || page_ != 1)
        return;
    lv_label_set_text(readout_, readout != nullptr ? readout : "--:--");
    sweep_permille_ = std::clamp(sweep_permille, 0, 1000);
    // The breath tick paints the new sweep on its next pass.
}

void OrbitMenuCrest::Hide() {
    if (layer_ == nullptr || !visible_)
        return;
    StopAnimations();
    visible_ = false;
    SetShown(layer_, false);
}
}  // namespace ProvisionsStopWatch
