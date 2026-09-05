#pragma once

#include "crest_asset.h"
#include "crest_audio.h"
#include "crest_motion.h"

// Only compiled by the Provisions C152 board. Base display objects remain
// alive for shared status/theme code, but hidden behind this opaque face.
class OrbitCrestDisplay final : public SpiLcdDisplay {
    using State = OrbitCrest::State;
    static constexpr bool kReducedMotion = false;  // bench variant; no new button mapping
    lv_obj_t* face_ = nullptr;
    lv_obj_t* band_ = nullptr;
    lv_obj_t* star_ = nullptr;
    lv_obj_t* caption_ = nullptr;
    std::array<lv_obj_t*, 3> rings_{};
    lv_timer_t* animation_timer_ = nullptr;
    State state_ = State::Boot;
    OrbitCrest::Frame frame_;
    OrbitCrest::Frame transition_from_;
    uint32_t transition_ms_ = 0;
    uint32_t last_frame_ms_ = 0;
    uint32_t speech_clock_ms_ = 0;
    uint32_t reply_started_ms_ = 0;
    uint32_t result_started_ms_ = 0;
    uint32_t result_hold_ms_ = 0;
    const char* result_caption_ = "";  // allowlisted literals only, never transcript
    const char* displayed_caption_ = nullptr;
    bool reply_received_ = false;
    bool power_save_ = false;
    bool speech_seen_ = false;
    float level_ = 0;

    static uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

    static bool IsClockStatus(const char* status) {
        return status && std::strlen(status) == 5 && status[2] == ':' && status[0] >= '0' &&
               status[0] <= '9' && status[1] >= '0' && status[1] <= '9' && status[3] >= '0' &&
               status[3] <= '9' && status[4] >= '0' && status[4] <= '9';
    }

    static State StateForStatus(const char* text) {
        if (!text)
            return State::Boot;
        if (!std::strcmp(text, "Ready"))
            return State::Idle;
        if (!std::strcmp(text, "Working"))
            return State::Thinking;
        if (!std::strcmp(text, "Listening") || !std::strcmp(text, Lang::Strings::LISTENING))
            return State::Listening;
        if (!std::strcmp(text, "Speaking") || !std::strcmp(text, Lang::Strings::SPEAKING))
            return State::Speaking;
        if (!std::strcmp(text, "Connecting") || !std::strcmp(text, Lang::Strings::CONNECTING) ||
            !std::strcmp(text, Lang::Strings::REGISTERING_NETWORK) ||
            !std::strcmp(text, Lang::Strings::LOADING_PROTOCOL))
            return State::Connecting;
        if (!std::strcmp(text, "Unavailable") || !std::strcmp(text, Lang::Strings::ERROR) ||
            !std::strcmp(text, Lang::Strings::SERVER_ERROR) ||
            !std::strcmp(text, Lang::Strings::SERVER_NOT_CONNECTED) ||
            !std::strcmp(text, Lang::Strings::SERVER_TIMEOUT) ||
            !std::strcmp(text, Lang::Strings::SERVER_NOT_FOUND))
            return State::Error;
        return State::Boot;
    }

    void ChangeStateLocked(State state) {
        transition_from_ = frame_;
        transition_ms_ = NowMs();
        state_ = state;
        if (state == State::Speaking)
            reply_started_ms_ = transition_ms_;
        if (state != State::Speaking)
            speech_clock_ms_ = 0;
        level_ = 0;
        if (animation_timer_)
            lv_timer_resume(animation_timer_);
        RenderLocked();
    }

    void ClearResultLocked() {
        result_caption_ = "";
        result_hold_ms_ = 0;
        reply_received_ = false;
        speech_seen_ = false;
    }

    void RenderLocked() {
        if (!face_ || power_save_)
            return;
        const uint32_t now = NowMs();
        const uint32_t delta = std::min<uint32_t>(now - last_frame_ms_, 60);
        last_frame_ms_ = now;
        if (state_ == State::Result && result_hold_ms_ &&
            now - result_started_ms_ >= result_hold_ms_) {
            ClearResultLocked();
            transition_from_ = frame_;
            transition_ms_ = now;
            state_ = State::Idle;
        }
        if ((state_ == State::Speaking || (reply_received_ && state_ == State::Thinking)) &&
            now - reply_started_ms_ >= 35000) {
            // A lost terminal frame must not leave an endless speech animation.
            ClearResultLocked();
            state_ = State::Error;
            transition_from_ = frame_;
            transition_ms_ = now;
        }

        auto& meter =
            state_ == State::Listening ? OrbitCrest::input_meter : OrbitCrest::output_meter;
        const uint32_t age = now - meter.sampled_ms.load(std::memory_order_acquire);
        const float target_level = age > now - transition_ms_
                                       ? 0.0F
                                       : OrbitCrest::AudioLevel(meter.mean_absolute.load(), age);
        const float smoothing = target_level > level_ ? 0.65F : 0.2F;
        level_ += (target_level - level_) * smoothing;
        if (state_ == State::Speaking && target_level > 0) {
            speech_clock_ms_ += delta;
            speech_seen_ = true;
        }

        OrbitCrest::Frame target;
        if (OrbitCrest::UsesRings(state_)) {
            target = OrbitCrest::Rings(state_, state_ == State::Speaking ? speech_clock_ms_ : now,
                                       level_, kReducedMotion);
        } else if (state_ == State::Result || state_ == State::Error || state_ == State::Boot ||
                   state_ == State::Connecting) {
            target.band_opacity = 0;
            if (state_ == State::Error) {
                target.color = OrbitCrest::kAmber;
                target.opacity[1] = 180;
            }
        }
        frame_ =
            OrbitCrest::Transition(transition_from_, target, now - transition_ms_, kReducedMotion);
        lv_obj_set_style_image_opa(band_, frame_.band_opacity, 0);
        lv_obj_set_style_image_opa(star_, frame_.star_opacity, 0);
        for (int index = 0; index < 3; ++index) {
            auto* ring = rings_[index];
            const int diameter = frame_.radii[index] * 2;
            lv_obj_set_size(ring, diameter, diameter);
            lv_obj_center(ring);
            lv_obj_set_style_arc_color(ring, lv_color_hex(frame_.color), LV_PART_MAIN);
            lv_obj_set_style_arc_opa(ring, frame_.opacity[index], LV_PART_MAIN);
            lv_arc_set_rotation(ring, state_ == State::Error ? 270 : 0);
            lv_arc_set_bg_angles(ring, state_ == State::Error ? 12 : 0,
                                 state_ == State::Error ? 348 : 360);
        }
        const char* text = state_ == State::Result ? result_caption_ : OrbitCrest::Caption(state_);
        if (kReducedMotion && state_ == State::Listening)
            text = "Listening";
        if (kReducedMotion && state_ == State::Speaking)
            text = "Speaking";
        if (displayed_caption_ != text) {
            lv_label_set_text_static(caption_, text);
            displayed_caption_ = text;
        }
        lv_obj_set_style_text_color(
            caption_,
            lv_color_hex(state_ == State::Error ? OrbitCrest::kAmber : OrbitCrest::kIvory), 0);
        // Only the active motion/receipt owns a timer. Idle has zero animation
        // wakeups; power-save pauses it even during an interrupted transition.
        if (animation_timer_ && now - transition_ms_ >= OrbitCrest::kTransitionMs &&
            !OrbitCrest::UsesRings(state_) && state_ != State::Result) {
            lv_timer_pause(animation_timer_);
        }
    }

public:
    OrbitCrestDisplay(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel, int width,
                      int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                      bool swap_xy)
        : SpiLcdDisplay(io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy,
                        kStopwatchInitialClearColor) {}

    ~OrbitCrestDisplay() override {
        DisplayLockGuard lock(this);
        if (animation_timer_)
            lv_timer_delete(animation_timer_);
        if (face_)
            lv_obj_delete(face_);
    }

    void SetupUI() override {
        if (IsSetupUICalled())
            return;
        SpiLcdDisplay::SetupUI();
        DisplayLockGuard lock(this);
        lv_display_add_event_cb(
            display_,
            [](lv_event_t* event) {
                auto* area = static_cast<lv_area_t*>(lv_event_get_param(event));
                area->x1 = (area->x1 >> 1) << 1;
                area->y1 = (area->y1 >> 1) << 1;
                area->x2 = ((area->x2 >> 1) << 1) + 1;
                area->y2 = ((area->y2 >> 1) << 1) + 1;
            },
            LV_EVENT_INVALIDATE_AREA, nullptr);
        hide_subtitle_ = true;
        for (auto* object :
             {container_, emoji_box_, preview_image_, top_bar_, status_bar_, bottom_bar_}) {
            if (object)
                lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
        }
        face_ = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(face_);
        lv_obj_set_size(face_, 466, 466);
        lv_obj_center(face_);
        lv_obj_set_style_bg_color(face_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(face_, LV_OPA_COVER, 0);
        lv_obj_remove_flag(face_, LV_OBJ_FLAG_SCROLLABLE);
        for (auto*& ring : rings_) {
            ring = lv_arc_create(face_);
            lv_obj_remove_style_all(ring);
            lv_obj_set_style_arc_width(ring, 3, LV_PART_MAIN);
            lv_obj_set_style_arc_opa(ring, LV_OPA_TRANSP, LV_PART_INDICATOR);
            lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
        }
        band_ = lv_image_create(face_);
        lv_image_set_src(band_, &OrbitCrest::kBandImage);
        lv_obj_set_pos(band_, OrbitCrest::kBandX, OrbitCrest::kBandY);
        star_ = lv_image_create(face_);
        lv_image_set_src(star_, &OrbitCrest::kStarImage);
        lv_obj_set_pos(star_, OrbitCrest::kStarX, OrbitCrest::kStarY);
        for (auto* mark : {band_, star_}) {
            lv_obj_set_style_image_recolor(mark, lv_color_hex(OrbitCrest::kIvory), 0);
            lv_obj_set_style_image_recolor_opa(mark, LV_OPA_COVER, 0);
        }
        caption_ = lv_label_create(face_);
        lv_obj_set_size(caption_, 280, 74);
        lv_obj_align(caption_, LV_ALIGN_CENTER, 0, 97);
        lv_obj_set_style_text_font(caption_, &font_noto_sans_basic_30_4, 0);
        lv_obj_set_style_text_align(caption_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(caption_, 3, 0);
        lv_label_set_long_mode(caption_, LV_LABEL_LONG_CLIP);
        animation_timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                static_cast<OrbitCrestDisplay*>(lv_timer_get_user_data(timer))->RenderLocked();
            },
            33, this);
        transition_ms_ = last_frame_ms_ = NowMs();
        RenderLocked();
    }

    void SetStatus(const char* status) override {
        if (IsClockStatus(status))
            return;
        DisplayLockGuard lock(this);
        State state = StateForStatus(status);
        if (state == State::Idle && (reply_received_ || result_caption_[0])) {
            if (!result_caption_[0])
                result_caption_ = speech_seen_ ? "Answered" : "Reply received";
            result_started_ms_ = NowMs();
            result_hold_ms_ = OrbitCrest::kResultHoldMs;
            state = State::Result;
        } else if (state != State::Speaking && state != State::Thinking && state != State::Idle) {
            ClearResultLocked();
        }
        if (state_ == state)
            return;
        ChangeStateLocked(state);
        last_status_update_time_ = std::chrono::system_clock::now();
    }

    void ShowNotification(const char* notification, int duration_ms = 3000) override {
        const char* caption = OrbitCrest::ResultCaption(notification);
        if (!caption[0])
            return;
        DisplayLockGuard lock(this);
        result_caption_ = caption;
        result_started_ms_ = NowMs();
        result_hold_ms_ = static_cast<uint32_t>(std::clamp(duration_ms, 2000, 6000));
        if (state_ == State::Idle || state_ == State::Result)
            ChangeStateLocked(State::Result);
    }

    void ShowNotification(const std::string& notification, int duration_ms = 3000) override {
        ShowNotification(notification.c_str(), duration_ms);
    }

    void SetChatMessage(const char* role, const char* content) override {
        if (!role || std::strcmp(role, "assistant") || !content || !content[0])
            return;
        DisplayLockGuard lock(this);
        // No transcript text is retained, rendered, logged or written to flash.
        reply_received_ = true;
        reply_started_ms_ = NowMs();
        if (state_ == State::Idle || state_ == State::Result) {
            if (!result_caption_[0])
                result_caption_ = "Reply received";
            result_started_ms_ = NowMs();
            result_hold_ms_ = OrbitCrest::kResultHoldMs;
            ChangeStateLocked(State::Result);
        }
        if (animation_timer_)
            lv_timer_resume(animation_timer_);
    }

    void SetPowerSaveMode(bool on) override {
        DisplayLockGuard lock(this);
        power_save_ = on;
        if (!face_)
            return;
        if (on) {
            lv_obj_add_flag(face_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x000000), 0);
            if (animation_timer_)
                lv_timer_pause(animation_timer_);
        } else {
            lv_obj_remove_flag(face_, LV_OBJ_FLAG_HIDDEN);
            last_frame_ms_ = NowMs();
            if (animation_timer_)
                lv_timer_resume(animation_timer_);
            RenderLocked();
        }
    }

    void SetTheme(Theme* theme) override {
        SpiLcdDisplay::SetTheme(theme);
        DisplayLockGuard lock(this);
        // Shared theme refresh must never turn the Orbit face white.
        lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x000000), 0);
    }

    void SetEmotion(const char*) override {}
    void ClearChatMessages() override {}
    void SetPreviewImage(std::unique_ptr<LvglImage>) override {}
    bool AddTextGlyphs(const std::vector<TextGlyph>&, uint8_t) override { return false; }
};
