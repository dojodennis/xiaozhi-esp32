#pragma once
#include <cstdint>

#include "lvgl.h"

// Menu drawing K (DESIGN-orbit-menu-crest-2026-09-29.md): the crest stays
// where the idle face draws it and the centre of the crest becomes the page.
//   List   – the star gives way to a checklist (gold bullets, ivory bars).
//   Timers – the band is the timer: gold over the elapsed sweep, dim for the
//            rest; the readout sits where the star was.
//   Notes  – a gold title line and two ragged ivory lines; the band sits half
//            a lobe off.
// Every part is an LVGL object over the two crest images already in flash.
// The timer band is composed per pixel into a PSRAM buffer from the band's
// alpha. All calls run under the display lock.
namespace ProvisionsStopWatch {
class OrbitMenuCrest {
public:
    static constexpr int kBandSize = 348;
    static constexpr int kBandX = 59;
    static constexpr int kBandY = 59;
    static constexpr uint32_t kGold = 0xC5A46D;
    static constexpr uint32_t kIvory = 0xE8E0D2;
    static constexpr uint32_t kTrack = 0x2A261C;
    static constexpr uint32_t kDim = 0x4A4437;
    // Notes: half a lobe, in LVGL's tenths of a degree. 0 keeps the band still.
    static constexpr int kNotesBandTenths = 150;

    void Create(lv_obj_t* parent);
    // page: 0 List, 1 Timers, 2 Notes. sweep_permille: elapsed share of the
    // focus timer (0 when there is none). readout: the remaining time or "--:--".
    void Show(int page, int sweep_permille, const char* readout);
    // Live timer update while the Timers page is on show; no arrival animation.
    void UpdateTimer(int sweep_permille, const char* readout);
    void Hide();
    bool Visible() const { return visible_; }

private:
    static void SetOpa(void* object, int32_t value);
    static void SetWidth(void* object, int32_t value);
    static void SetSweep(void* self, int32_t value);
    static void BreathTick(lv_timer_t* timer);
    void Animate(lv_obj_t* object, lv_anim_exec_xcb_t exec, int32_t from, int32_t to,
                 uint32_t duration_ms, uint32_t delay_ms);
    void AnimateSweep(int32_t from, int32_t to, uint32_t duration_ms);
    void StopAnimations();
    bool EnsureTimerBuffers();
    void ComposeBand(int sweep_permille, float breath);

    lv_obj_t* layer_ = nullptr;
    lv_obj_t* band_ = nullptr;
    lv_obj_t* band_timer_ = nullptr;
    lv_obj_t* star_ = nullptr;
    lv_obj_t* bullets_[3]{};
    lv_obj_t* bars_[3]{};
    lv_obj_t* readout_ = nullptr;
    lv_obj_t* caption_ = nullptr;
    lv_obj_t* dots_[3]{};
    lv_timer_t* breath_timer_ = nullptr;
    uint16_t* timer_pixels_ = nullptr;   // PSRAM, kBandSize² RGB565
    uint16_t* angle_tenths_ = nullptr;   // PSRAM, kBandSize² angle from twelve, 0..3599
    lv_image_dsc_t timer_image_{};
    int page_ = -1;
    int sweep_permille_ = 0;
    int shown_sweep_permille_ = -1;
    float shown_breath_ = -1.0F;
    uint32_t breath_started_ms_ = 0;
    bool visible_ = false;
};
}  // namespace ProvisionsStopWatch
