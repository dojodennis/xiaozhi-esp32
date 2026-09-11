#include "wifi_board.h"
#include "backlight.h"
#include "display/lcd_display.h"
#include "esp_lcd_co5300.h"
#include "codecs/es8311_audio_codec.h"
#include "application.h"
#include "button.h"
#include "M5IOE1.h"
#include "M5PM1.h"
#include "config.h"
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
#include "crest_asset.h"
#include "crest_audio.h"
#include "crest_motion.h"
#include "orbit_dial.h"
#include "provisions_local_capture_feedback.h"
#include "provisions_timer_snapshot.h"
#include "utf8_ellipsis.h"
#if CONFIG_PROVISIONS_SCHEDULE_HARDWARE_BENCH
#include <esp_pthread.h>
#include "service_schedule_hardware_bench.h"
#endif
#if CONFIG_PROVISIONS_SCHEDULE_BENCH_DEMO
#include "codecs/dummy_audio_codec.h"
#include "service_schedule_demo.h"
#include "service_schedule_view.h"
#endif
#endif
#include "assets/lang_config.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <vector>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <material_symbols.h>
#include <wifi_manager.h>

#define TAG "M5StackStopwatch"
#define LCD_OPCODE_WRITE_CMD (0x02ULL)

#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
constexpr uint16_t kStopwatchInitialClearColor = 0x0000;
#else
constexpr uint16_t kStopwatchInitialClearColor = 0xFFFF;
#endif

#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
namespace {

constexpr char kSignedHardwareIdentity[] = "PROVISIONS_SIGNED_HARDWARE_IDENTITY=" BOARD_NAME;
constexpr int64_t kDisplayIdleTimeoutUs = 45LL * 1000 * 1000;
constexpr int kDefaultOutputVolume = 90;
constexpr int kMaximumOutputVolume = 100;
constexpr int kRoundTopBarWidth = 260;
constexpr int kRoundTopBarOffset = 46;
constexpr int kRoundContentWidth = 330;
constexpr int kRoundBrandWidth = 280;
constexpr int kRoundBrandTopOffset = 78;
constexpr int kRoundTitleTopOffset = 103;
constexpr int kRoundRuleTopOffset = 146;
constexpr int kRoundHeroSize = 124;
constexpr int kRoundHeroOffset = -10;
constexpr int kRoundIconSize = 82;
constexpr int kRoundStatusWidth = 320;
constexpr int kRoundStatusHeight = 54;
constexpr int kRoundStatusOffset = 82;
constexpr int kRoundHintWidth = 304;
constexpr int kRoundHintHeight = 38;
constexpr int kRoundHintOffset = 132;
// Reply surface sized for kitchen glanceability (Dennis 2026-08-29: cream on
// low-opacity blue was unreadable on the real AMOLED). The panel is opaque
// near-black, the text white and large; the header is one short gold word.
constexpr int kReplyHeaderWidth = 280;
constexpr int kReplyHeaderTopOffset = 64;
constexpr int kReplyPanelWidth = 400;
constexpr int kReplyPanelHeight = 300;
constexpr int kReplyPanelOffset = 16;
constexpr int kReplyPlaybackMaximumMs = 35 * 1000;
constexpr int kReplyHoldAfterSpeechMs = 12 * 1000;
constexpr int kReplyScrollIntervalMs = 4 * 1000;
constexpr int kReplyScrollStep = 176;
constexpr int64_t kOrbitTickIntervalUs = 1000LL * 1000;
constexpr int kOrbitArcDiameter = ProvisionsStopwatchOrbit::kSlotRadius * 2;
constexpr int kOrbitLabelWidth = 94;
constexpr int kOrbitAlarmMaximumNames = 2;

constexpr uint32_t kColorGold = 0xD4B67A;
// Pure white on the true-black ground so the UI melts into the device frame
// (Dennis 2026-08-29).
constexpr uint32_t kColorCream = 0xFFFFFF;
constexpr uint32_t kColorGreen = 0x7FBF8F;
constexpr uint32_t kColorBlue = 0x9FB8D8;
constexpr uint32_t kColorAmber = 0xE0A256;
constexpr uint32_t kColorRed = 0xE07566;
constexpr uint32_t kColorTalkButton = 0xF2C84B;
constexpr std::array<uint32_t, ProvisionsStopwatchOrbit::kMaximumSlots> kOrbitColors = {
    0xD4B67A, 0x7FBF8F, 0x9FB8D8, 0xC99FD8, 0xE07566, 0x73C7C4,
};

class ProvisionsStopwatchAudioCodec final : public Es8311AudioCodec {
public:
    using Es8311AudioCodec::Es8311AudioCodec;

    bool InputData(std::vector<int16_t>& data) override {
        const bool captured = Es8311AudioCodec::InputData(data);
        OrbitCrest::input_meter.Observe(data.data(), captured ? data.size() : 0,
                                        static_cast<uint32_t>(esp_timer_get_time() / 1000));
        return captured;
    }

    bool OutputData(std::vector<int16_t>& data) override {
        const bool played = Es8311AudioCodec::OutputData(data);
        OrbitCrest::output_meter.Observe(data.data(), played ? data.size() : 0,
                                         static_cast<uint32_t>(esp_timer_get_time() / 1000));
        return played;
    }
#if CONFIG_PROVISIONS_SCHEDULE_HARDWARE_BENCH
    void EnableInput(bool enable) override {
        (void)enable;
        Es8311AudioCodec::EnableInput(false);
    }
#endif

    void Start() override {
        Es8311AudioCodec::Start();
        if (output_volume() < kDefaultOutputVolume || output_volume() > 100) {
            // Apply the pilot floor after Start() reloads the saved preference.
            // This setter updates codec state only; it never writes NVS.
            SetOutputVolumeForSession(kDefaultOutputVolume);
        }
    }
};

#if CONFIG_PROVISIONS_SCHEDULE_HARDWARE_BENCH
class BenchScheduleStore final : public orbit::service_schedule::WorkerStore {
    orbit::service_schedule::storage::NvsStore store_{
        orbit::service_schedule::HardwareBenchScope(),
        orbit::service_schedule::storage::StoreDomain::Bench};

public:
    orbit::service_schedule::storage::LoadResult Load(
        orbit::service_schedule::FacePersistentState& state,
        orbit::service_schedule::storage::Bytes& bytes) override {
        return store_.Load(state, bytes);
    }
    orbit::service_schedule::storage::SaveResult Transition(
        const orbit::service_schedule::storage::Bytes* expected,
        const orbit::service_schedule::FacePersistentState& state,
        orbit::service_schedule::storage::Bytes& bytes) override {
        return store_.Transition(expected, state, bytes);
    }
};
#endif
}  // namespace

LV_FONT_DECLARE(font_noto_sans_basic_16_4);
LV_FONT_DECLARE(font_noto_sans_basic_30_4);
#endif

// CO5300 AMOLED: initialize at full brightness, then restore the saved setting.
static const co5300_lcd_init_cmd_t vendor_specific_init[] = {
    // {cmd, { data }, data_size, delay_ms}
    {0xFE, (uint8_t []){0x00}, 0, 0},
    {0xC4, (uint8_t []){0x80}, 1, 0},
    {0x3A, (uint8_t []){0x55}, 0, 10}, // RGB565
    {0x35, (uint8_t []){0x00}, 0, 10},
    {0x53, (uint8_t []){0x20}, 1, 10},
    {0x51, (uint8_t []){0xFF}, 1, 10},
    {0x63, (uint8_t []){0xFF}, 1, 10},
    {0x2A, (uint8_t []){0x00, 0x00, 0x01, 0xD1}, 4, 0}, // Column address: 0-465
    {0x2B, (uint8_t []){0x00, 0x00, 0x01, 0xD1}, 4, 0}, // Row address: 0-465
    {0x11, (uint8_t []){0x00}, 0, 120}, // Exit sleep
    {0x29, (uint8_t []){0x00}, 0, 20},  // Display on
};

class RoundLcdDisplay : public SpiLcdDisplay {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
private:
    enum class VisualState : uint8_t {
        kBoot,
        kConnecting,
        kReady,
        kListening,
        kWorking,
        kSpeaking,
        kUnavailable,
        kAdded,
        kSuccess,
        kDraft,
        kRecorded,
        kLocalRecorded,
        kQuestion,
        kWarning,
        kNotice,
    };

    struct StatePresentation {
        const char* title;
        const char* hint;
        const char* icon;
        uint32_t color;
    };

    struct OrbitSlotObjects {
        lv_obj_t* arc = nullptr;
        lv_obj_t* label = nullptr;
        lv_obj_t* remaining = nullptr;
    };

    using AlarmOutputChange = ProvisionsStopwatchOrbit::AlarmOutputChange;

    lv_obj_t* brand_label_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* brand_rule_ = nullptr;
    lv_obj_t* hero_halo_ = nullptr;
    lv_obj_t* brand_mark_label_ = nullptr;
    lv_obj_t* hint_panel_ = nullptr;
    lv_obj_t* hint_label_ = nullptr;
    lv_obj_t* talk_button_dot_ = nullptr;
    lv_obj_t* reply_header_label_ = nullptr;
    lv_obj_t* reply_panel_ = nullptr;
    lv_obj_t* reply_label_ = nullptr;
    lv_obj_t* orbit_layer_ = nullptr;
    std::array<OrbitSlotObjects, ProvisionsStopwatchOrbit::kMaximumSlots> orbit_slot_objects_{};
    lv_obj_t* orbit_center_label_ = nullptr;
    lv_obj_t* orbit_overflow_label_ = nullptr;
    lv_obj_t* alarm_layer_ = nullptr;
    lv_obj_t* alarm_title_label_ = nullptr;
    lv_obj_t* alarm_names_label_ = nullptr;
    lv_obj_t* alarm_hint_label_ = nullptr;
    lv_obj_t* crest_layer_ = nullptr;
    lv_obj_t* crest_band_ = nullptr;
    lv_obj_t* crest_star_ = nullptr;
    lv_obj_t* crest_caption_ = nullptr;
    lv_obj_t* crest_timer_text_ = nullptr;
    lv_obj_t* dictation_panel_ = nullptr;
    lv_obj_t* dictation_status_ = nullptr;
    lv_obj_t* dictation_action_ = nullptr;
    std::array<lv_obj_t*, 3> crest_rings_{};
    lv_timer_t* crest_animation_timer_ = nullptr;
    OrbitCrest::State crest_state_ = OrbitCrest::State::Boot;
    OrbitCrest::Frame crest_frame_;
    OrbitCrest::Frame crest_transition_from_;
    uint32_t crest_transition_ms_ = 0;
    uint32_t crest_reply_started_ms_ = 0;
    uint32_t crest_result_started_ms_ = 0;
    uint32_t crest_result_hold_ms_ = 0;
    const char* crest_result_caption_ = "";
    const char* crest_displayed_caption_ = nullptr;
    bool crest_reply_received_ = false;
    bool crest_speech_seen_ = false;
    bool crest_error_ring_geometry_ = false;
    bool dictation_visible_ = false;
    std::string dictation_status_text_;
    std::string dictation_action_text_;
    std::string crest_timer_text_value_;
    float crest_level_ = 0;
    esp_timer_handle_t visual_reset_timer_ = nullptr;
    esp_timer_handle_t reply_scroll_timer_ = nullptr;
    esp_timer_handle_t orbit_tick_timer_ = nullptr;
    std::atomic<int64_t> visual_reset_deadline_us_{0};
    std::atomic<VisualState> resting_state_{VisualState::kBoot};
    std::atomic<bool> receipt_visible_{false};
    std::atomic<bool> reply_visible_{false};
    std::atomic<bool> power_save_active_{false};
    std::atomic<bool> timer_alarm_active_{false};
    std::atomic<uint32_t> reply_generation_{0};
    // Banner over the reply text: the last receipt's title and state colour
    // ("REPLIED" in green, "NO NEW REPLY" in amber). Written and read under
    // the display lock.
    std::string reply_banner_title_ = "REPLY";
    uint32_t reply_banner_color_ = kColorGold;
    bool orbit_snapshot_received_ = false;
    int64_t snapshot_received_monotonic_ms_ = 0;
    std::string galley_session_id_;
    ProvisionsTimerSnapshot::Snapshot timer_snapshot_;
    ProvisionsStopwatchOrbit::SlotBoard orbit_slot_board_;
    ProvisionsStopwatchOrbit::AlarmState timer_alarm_state_;
    struct DismissedTimer {
        std::string id;
        int64_t deadline_ms = 0;
    };
    std::vector<DismissedTimer> dismissed_timers_;
    std::function<void()> timer_dismiss_callback_;
    std::function<void(bool)> timer_alarm_output_callback_;
#if CONFIG_PROVISIONS_SCHEDULE_HARDWARE_BENCH
    int64_t bench_clock_ms_ = 0;
    bool bench_clock_trusted_ = false;
#endif
#if CONFIG_PROVISIONS_SCHEDULE_BENCH_DEMO
    orbit::service_schedule::ScheduleDemo schedule_demo_;
    orbit::service_schedule::ScheduleView schedule_view_;
    int64_t schedule_last_refresh_ms_ = 0;
#endif

    static uint32_t CrestNowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

    static OrbitCrest::State CrestStateFor(VisualState state) {
        using CrestState = OrbitCrest::State;
        switch (state) {
            case VisualState::kBoot:
                return CrestState::Boot;
            case VisualState::kConnecting:
                return CrestState::Connecting;
            case VisualState::kReady:
                return CrestState::Idle;
            case VisualState::kListening:
                return CrestState::Listening;
            case VisualState::kWorking:
                return CrestState::Thinking;
            case VisualState::kSpeaking:
                return CrestState::Speaking;
            case VisualState::kUnavailable:
                return CrestState::Error;
            default:
                return CrestState::Result;
        }
    }

    void ClearCrestResultLocked() {
        crest_result_caption_ = "";
        crest_result_hold_ms_ = 0;
        crest_reply_received_ = false;
        crest_speech_seen_ = false;
    }

    void ChangeCrestStateLocked(OrbitCrest::State state) {
        crest_transition_from_ = crest_frame_;
        crest_transition_ms_ = CrestNowMs();
        crest_state_ = state;
        if (state == OrbitCrest::State::Speaking)
            crest_reply_started_ms_ = crest_transition_ms_;
        crest_level_ = 0;
        if (crest_animation_timer_)
            lv_timer_resume(crest_animation_timer_);
        RenderCrestLocked();
    }

    void RenderCrestLocked() {
        if (!crest_layer_ || power_save_active_.load())
            return;
        const uint32_t now = CrestNowMs();
        if (crest_state_ == OrbitCrest::State::Result && crest_result_hold_ms_ &&
            now - crest_result_started_ms_ >= crest_result_hold_ms_) {
            ClearCrestResultLocked();
            crest_transition_from_ = crest_frame_;
            crest_transition_ms_ = now;
            crest_state_ = OrbitCrest::State::Idle;
        }
        if ((crest_state_ == OrbitCrest::State::Speaking ||
             (crest_reply_received_ && crest_state_ == OrbitCrest::State::Thinking)) &&
            now - crest_reply_started_ms_ >= 35000) {
            ClearCrestResultLocked();
            crest_state_ = OrbitCrest::State::Error;
            crest_transition_from_ = crest_frame_;
            crest_transition_ms_ = now;
        }

        auto& meter = crest_state_ == OrbitCrest::State::Listening ? OrbitCrest::input_meter
                                                                   : OrbitCrest::output_meter;
        const uint32_t age = now - meter.sampled_ms.load(std::memory_order_acquire);
        const float target_level = age > now - crest_transition_ms_
                                       ? 0.0F
                                       : OrbitCrest::AudioLevel(meter.mean_absolute.load(), age);
        const float smoothing = target_level > crest_level_ ? 0.24F : 0.08F;
        crest_level_ += (target_level - crest_level_) * smoothing;
        if (crest_state_ == OrbitCrest::State::Speaking && target_level > 0)
            crest_speech_seen_ = true;

        OrbitCrest::Frame target;
        if (OrbitCrest::UsesRings(crest_state_)) {
            target =
                OrbitCrest::Rings(crest_state_, now - crest_transition_ms_, crest_level_, false);
        } else if (crest_state_ == OrbitCrest::State::Result ||
                   crest_state_ == OrbitCrest::State::Error ||
                   crest_state_ == OrbitCrest::State::Boot ||
                   crest_state_ == OrbitCrest::State::Connecting) {
            target.band_opacity = 0;
            if (crest_state_ == OrbitCrest::State::Error) {
                target.color = OrbitCrest::kAmber;
                target.opacity[1] = 180;
            }
        }
        crest_frame_ = OrbitCrest::Transition(crest_transition_from_, target,
                                              now - crest_transition_ms_, false);
        lv_obj_set_style_image_opa(crest_band_, crest_frame_.band_opacity, 0);
        lv_obj_set_style_image_opa(crest_star_, crest_frame_.star_opacity, 0);
        for (int index = 0; index < 3; ++index) {
            auto* ring = crest_rings_[index];
            const int diameter = crest_frame_.radii[index] * 2;
            if (lv_obj_get_width(ring) != diameter || lv_obj_get_height(ring) != diameter) {
                lv_obj_set_size(ring, diameter, diameter);
                lv_obj_center(ring);
            }
            lv_obj_set_style_arc_color(ring, lv_color_hex(crest_frame_.color), LV_PART_MAIN);
            lv_obj_set_style_arc_opa(ring, crest_frame_.opacity[index], LV_PART_MAIN);
        }
        const bool error_geometry = crest_state_ == OrbitCrest::State::Error;
        if (error_geometry != crest_error_ring_geometry_) {
            for (auto* ring : crest_rings_) {
                lv_arc_set_rotation(ring, error_geometry ? 270 : 0);
                lv_arc_set_bg_angles(ring, error_geometry ? 12 : 0, error_geometry ? 348 : 360);
            }
            crest_error_ring_geometry_ = error_geometry;
        }
        const char* text = crest_state_ == OrbitCrest::State::Result
                               ? crest_result_caption_
                               : OrbitCrest::Caption(crest_state_);
        if (crest_displayed_caption_ != text) {
            lv_label_set_text_static(crest_caption_, text);
            crest_displayed_caption_ = text;
        }
        lv_obj_set_style_text_color(
            crest_caption_,
            lv_color_hex(crest_state_ == OrbitCrest::State::Error ? OrbitCrest::kAmber
                                                                  : OrbitCrest::kIvory),
            0);
        if (crest_animation_timer_ && now - crest_transition_ms_ >= OrbitCrest::kTransitionMs &&
            !OrbitCrest::UsesRings(crest_state_) && crest_state_ != OrbitCrest::State::Result) {
            lv_timer_pause(crest_animation_timer_);
        }
    }

    void SetCrestResultLocked(const char* caption, uint32_t hold_ms) {
        if (!caption || !caption[0])
            return;
        crest_result_caption_ = caption;
        crest_result_started_ms_ = CrestNowMs();
        crest_result_hold_ms_ = std::clamp<uint32_t>(hold_ms, 1000, 6000);
        ChangeCrestStateLocked(OrbitCrest::State::Result);
    }

    void CreateCrestUiLocked(lv_obj_t* screen) {
        crest_layer_ = lv_obj_create(screen);
        lv_obj_remove_style_all(crest_layer_);
        lv_obj_set_size(crest_layer_, 466, 466);
        lv_obj_center(crest_layer_);
        lv_obj_set_style_bg_color(crest_layer_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(crest_layer_, LV_OPA_COVER, 0);
        lv_obj_remove_flag(crest_layer_, LV_OBJ_FLAG_SCROLLABLE);

        crest_band_ = lv_image_create(crest_layer_);
        lv_image_set_src(crest_band_, &OrbitCrest::kBandImage);
        lv_obj_set_pos(crest_band_, OrbitCrest::kBandX, OrbitCrest::kBandY);
        for (int index = 0; index < 3; ++index) {
            auto*& ring = crest_rings_[index];
            ring = lv_arc_create(crest_layer_);
            lv_obj_remove_style_all(ring);
            const int diameter = crest_frame_.radii[index] * 2;
            lv_obj_set_size(ring, diameter, diameter);
            lv_obj_center(ring);
            lv_obj_set_style_arc_width(ring, 3, LV_PART_MAIN);
            lv_obj_set_style_arc_opa(ring, LV_OPA_TRANSP, LV_PART_INDICATOR);
            lv_arc_set_rotation(ring, 0);
            lv_arc_set_bg_angles(ring, 0, 360);
            lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
        }
        crest_star_ = lv_image_create(crest_layer_);
        lv_image_set_src(crest_star_, &OrbitCrest::kStarImage);
        lv_obj_set_pos(crest_star_, OrbitCrest::kStarX, OrbitCrest::kStarY);
        for (auto* mark : {crest_band_, crest_star_}) {
            lv_obj_set_style_image_recolor(mark, lv_color_hex(OrbitCrest::kIvory), 0);
            lv_obj_set_style_image_recolor_opa(mark, LV_OPA_COVER, 0);
        }
        crest_caption_ = lv_label_create(crest_layer_);
        lv_obj_set_size(crest_caption_, 280, 74);
        lv_obj_align(crest_caption_, LV_ALIGN_CENTER, 0, 97);
        lv_obj_set_style_text_font(crest_caption_, &font_noto_sans_basic_30_4, 0);
        lv_obj_set_style_text_align(crest_caption_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(crest_caption_, 3, 0);
        lv_label_set_long_mode(crest_caption_, LV_LABEL_LONG_CLIP);

        crest_timer_text_ = lv_label_create(crest_layer_);
        lv_obj_set_size(crest_timer_text_, 250, 54);
        lv_obj_align(crest_timer_text_, LV_ALIGN_CENTER, 0, 166);
        lv_obj_set_style_text_font(crest_timer_text_, &font_noto_sans_basic_16_4, 0);
        lv_obj_set_style_text_align(crest_timer_text_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(crest_timer_text_, lv_color_hex(0xcbd5e1), 0);
        lv_label_set_long_mode(crest_timer_text_, LV_LABEL_LONG_CLIP);
        lv_label_set_text(crest_timer_text_, "");

        dictation_panel_ = lv_obj_create(crest_layer_);
        lv_obj_remove_style_all(dictation_panel_);
        lv_obj_set_size(dictation_panel_, 466, 466);
        lv_obj_center(dictation_panel_);
        lv_obj_set_style_bg_color(dictation_panel_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(dictation_panel_, LV_OPA_COVER, 0);
        lv_obj_remove_flag(dictation_panel_, LV_OBJ_FLAG_SCROLLABLE);
        auto* title = lv_label_create(dictation_panel_);
        lv_obj_set_style_text_font(title, &font_noto_sans_basic_30_4, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(OrbitCrest::kIvory), 0);
        lv_label_set_text(title, "Dictation");
        lv_obj_align(title, LV_ALIGN_CENTER, 0, -125);
        dictation_status_ = lv_label_create(dictation_panel_);
        lv_obj_set_size(dictation_status_, 310, 115);
        lv_obj_align(dictation_status_, LV_ALIGN_CENTER, 0, -35);
        lv_obj_set_style_text_font(dictation_status_, &font_noto_sans_basic_16_4, 0);
        lv_obj_set_style_text_align(dictation_status_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(dictation_status_, lv_color_hex(0xcbd5e1), 0);
        lv_obj_set_style_text_line_space(dictation_status_, 8, 0);
        dictation_action_ = lv_label_create(dictation_panel_);
        lv_obj_set_style_text_font(dictation_action_, &font_noto_sans_basic_30_4, 0);
        lv_obj_set_style_text_color(dictation_action_, lv_color_hex(0x7bb7ff), 0);
        lv_obj_align(dictation_action_, LV_ALIGN_CENTER, 0, 65);
        auto* help = lv_label_create(dictation_panel_);
        lv_obj_set_style_text_font(help, &font_noto_sans_basic_16_4, 0);
        lv_obj_set_style_text_color(help, lv_color_hex(0xcbd5e1), 0);
        lv_obj_set_style_text_align(help, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(help, "Hold yellow: segment\nDouble blue: back");
        lv_obj_align(help, LV_ALIGN_CENTER, 0, 125);
        lv_obj_add_flag(dictation_panel_, LV_OBJ_FLAG_HIDDEN);

        crest_animation_timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                static_cast<RoundLcdDisplay*>(lv_timer_get_user_data(timer))->RenderCrestLocked();
            },
            33, this);
        crest_transition_ms_ = CrestNowMs();
        RenderCrestLocked();
    }

    static bool IsClockStatus(const char* status) {
        return status != nullptr && std::strlen(status) == 5 && status[2] == ':' &&
               status[0] >= '0' && status[0] <= '9' && status[1] >= '0' && status[1] <= '9' &&
               status[3] >= '0' && status[3] <= '9' && status[4] >= '0' && status[4] <= '9';
    }

    static void SetVisible(lv_obj_t* object, bool visible) {
        if (object == nullptr) {
            return;
        }
        if (visible) {
            lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
        }
    }

    int64_t EffectiveServerNowMs() const {
#if CONFIG_PROVISIONS_SCHEDULE_HARDWARE_BENCH
        return bench_clock_ms_;
#endif
        if (!orbit_snapshot_received_) {
            return 0;
        }
        const int64_t monotonic_ms = esp_timer_get_time() / 1000;
        const int64_t elapsed_ms =
            std::max<int64_t>(0, monotonic_ms - snapshot_received_monotonic_ms_);
        if (elapsed_ms > std::numeric_limits<int64_t>::max() - timer_snapshot_.server_now_ms) {
            return std::numeric_limits<int64_t>::max();
        }
        return timer_snapshot_.server_now_ms + elapsed_ms;
    }

    bool ShouldShowOrbitLocked() const {
        return orbit_snapshot_received_ && !dictation_visible_ && !receipt_visible_.load() &&
               resting_state_.load() == VisualState::kReady;
    }

    // Banner language (Dennis 2026-08-29): short uppercase fragments a chef
    // reads in one glance, the icon carrying the meaning — not sentences.
    static StatePresentation PresentationFor(VisualState state) {
        switch (state) {
            case VisualState::kBoot:
                return {"STARTING", "ONE MOMENT", MATERIAL_SYMBOLS_PROGRESS_ACTIVITY,
                        kColorGold};
            case VisualState::kConnecting:
                return {"CONNECTING", "SECURE LINK", MATERIAL_SYMBOLS_WIFI,
                        kColorBlue};
            case VisualState::kReady:
                return {"READY", "HOLD TO TALK", MATERIAL_SYMBOLS_MIC, kColorGreen};
            case VisualState::kListening:
                return {"LISTENING", "RELEASE TO SEND", MATERIAL_SYMBOLS_MIC, kColorGold};
            case VisualState::kWorking:
                return {"CHECKING", "ONE MOMENT", MATERIAL_SYMBOLS_PROGRESS_ACTIVITY,
                        kColorBlue};
            case VisualState::kSpeaking:
                return {"REPLY", "LISTEN", MATERIAL_SYMBOLS_VOLUME_UP,
                        kColorBlue};
            case VisualState::kUnavailable:
                return {"OFFLINE", "TRY AGAIN", MATERIAL_SYMBOLS_CLOUD_OFF, kColorRed};
            case VisualState::kAdded:
                return {"ADDED", "DRAFT - NOT SENT", MATERIAL_SYMBOLS_CHECK_CIRCLE,
                        kColorGreen};
            case VisualState::kSuccess:
                return {"DONE", "LISTEN", MATERIAL_SYMBOLS_CHECK_CIRCLE,
                        kColorGreen};
            case VisualState::kDraft:
                return {"DRAFT", "NOT SENT", MATERIAL_SYMBOLS_INFO, kColorAmber};
            case VisualState::kRecorded:
                return {"RECORDED", "FROM RECORDS", MATERIAL_SYMBOLS_INFO, kColorAmber};
            case VisualState::kLocalRecorded:
                return {"RECORDED", "ON ORBIT", MATERIAL_SYMBOLS_CHECK_CIRCLE,
                        kColorGreen};
            case VisualState::kQuestion:
                return {"QUESTION", "ANSWER NOW", MATERIAL_SYMBOLS_HELP, kColorGold};
            case VisualState::kWarning:
                return {"NO CHANGE", "NOTHING DONE", MATERIAL_SYMBOLS_WARNING, kColorAmber};
            case VisualState::kNotice:
                return {"NOTICE", "LISTEN", MATERIAL_SYMBOLS_INFO, kColorAmber};
        }
        return {"OFFLINE", "TRY AGAIN", MATERIAL_SYMBOLS_CLOUD_OFF, kColorRed};
    }

    void SetReplyLayoutLocked(bool visible) {
#if CONFIG_PROVISIONS_SCHEDULE_BENCH_DEMO
        (void)visible;
        schedule_view_.Render(schedule_demo_.model(),
                              orbit::service_schedule::ScheduleDemo::FixtureTime, true);
        return;
#endif
        const bool display_awake = !power_save_active_.load();
        const bool show_alarm = display_awake && timer_alarm_active_.load();
        const bool show_reply = display_awake && !show_alarm && visible;
        const bool show_orbit =
            display_awake && !show_alarm && !show_reply && ShouldShowOrbitLocked();
        const bool show_normal = display_awake && !show_alarm && !show_reply && !show_orbit;

        // Keep the legacy objects alive for shared status/timer ownership, but
        // let the opaque Crest surface own the normal and reply presentation.
        SetVisible(top_bar_, false);
        SetVisible(brand_label_, false);
        lv_obj_t* normal[] = {title_label_, brand_rule_, hero_halo_, status_bar_, hint_panel_};
        for (auto* object : normal) {
            SetVisible(object, false);
        }

        lv_obj_t* reply[] = {reply_header_label_, reply_panel_};
        for (auto* object : reply) {
            SetVisible(object, false);
        }
        SetVisible(crest_layer_, show_normal || show_reply);
        SetVisible(dictation_panel_, dictation_visible_ && !receipt_visible_.load());
        SetVisible(orbit_layer_, show_orbit);
        SetVisible(alarm_layer_, show_alarm);
    }

    AlarmOutputChange RefreshOrbitLocked() {
#if CONFIG_PROVISIONS_SCHEDULE_BENCH_DEMO
        const auto real_now = esp_timer_get_time() / 1000;
        if (schedule_last_refresh_ms_ > 0 && real_now >= schedule_last_refresh_ms_)
            schedule_demo_.Elapse(real_now - schedule_last_refresh_ms_);
        schedule_last_refresh_ms_ = real_now;
        schedule_view_.Render(schedule_demo_.model(),
                              orbit::service_schedule::ScheduleDemo::FixtureTime, true);
        timer_alarm_active_.store(schedule_demo_.model().alarm_active());
        return schedule_demo_.model().TakeOutputChange();
#endif
        if (!orbit_snapshot_received_) {
            return AlarmOutputChange::kNone;
        }

        const int64_t now_ms = EffectiveServerNowMs();
#if !CONFIG_PROVISIONS_SCHEDULE_HARDWARE_BENCH
        ProvisionsStopwatchOrbit::LatchDueTimers(timer_snapshot_.timers, now_ms);
#endif
        orbit_slot_board_.Update(timer_snapshot_.timers, now_ms);
        const auto colliding_ids =
            ProvisionsStopwatchOrbit::CollidingIds(timer_snapshot_.timers, now_ms);
        const auto& slots = orbit_slot_board_.slots();
        for (std::size_t index = 0; index < slots.size(); ++index) {
            const auto& slot = slots[index];
            auto& objects = orbit_slot_objects_[index];
            SetVisible(objects.arc, true);
            SetVisible(objects.label, true);
            SetVisible(objects.remaining, slot.occupied);
            if (objects.arc == nullptr || objects.label == nullptr ||
                objects.remaining == nullptr) {
                continue;
            }
            if (!slot.occupied) {
                lv_arc_set_value(objects.arc, 0);
                lv_obj_set_style_arc_color(objects.arc, lv_color_hex(0x252525), LV_PART_MAIN);
                lv_obj_set_style_arc_color(objects.arc, lv_color_hex(0x252525), LV_PART_INDICATOR);
                lv_obj_set_style_arc_width(objects.arc, 6, LV_PART_MAIN);
                lv_label_set_text(objects.label, "+");
                lv_obj_set_style_text_color(objects.label, lv_color_hex(0x555555), 0);
                continue;
            }

            const bool finished =
                slot.timer.status == ProvisionsTimerSnapshot::TimerStatus::kAttention
#if !CONFIG_PROVISIONS_SCHEDULE_HARDWARE_BENCH
                || slot.timer.deadline_ms <= now_ms;
#else
                ;
#endif
            const bool colliding = std::find(colliding_ids.begin(), colliding_ids.end(),
                                             slot.timer.id) != colliding_ids.end();
            const uint32_t color = finished ? kColorRed : kOrbitColors[index];
            const int arc_value = static_cast<int>(
                ProvisionsStopwatchOrbit::RemainingFraction(slot, now_ms) * 1000.0F);
            lv_arc_set_value(objects.arc, arc_value);
            lv_obj_set_style_arc_color(objects.arc, lv_color_hex(color), LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(
                objects.arc, lv_color_hex(colliding ? kColorAmber : 0x303030), LV_PART_MAIN);
            lv_obj_set_style_arc_width(objects.arc, colliding ? 10 : 6, LV_PART_MAIN);
            lv_label_set_text(objects.label, slot.timer.label.c_str());
            lv_obj_set_style_text_color(objects.label, lv_color_hex(kColorCream), 0);
            const std::string remaining =
#if CONFIG_PROVISIONS_SCHEDULE_HARDWARE_BENCH
                !bench_clock_trusted_ && !finished ? "--:--" :
#endif
                finished
                    ? "DONE"
                    : ProvisionsStopwatchOrbit::FormatRemaining(slot.timer.deadline_ms, now_ms);
            lv_label_set_text(objects.remaining, remaining.c_str());
            lv_obj_set_style_text_color(objects.remaining, lv_color_hex(color), 0);
        }

        const auto next_active = std::min_element(
            timer_snapshot_.timers.begin(), timer_snapshot_.timers.end(),
            [now_ms](const auto& left, const auto& right) {
                const bool left_future =
                    left.status == ProvisionsTimerSnapshot::TimerStatus::kActive &&
                    left.deadline_ms > now_ms;
                const bool right_future =
                    right.status == ProvisionsTimerSnapshot::TimerStatus::kActive &&
                    right.deadline_ms > now_ms;
                if (left_future != right_future) {
                    return left_future;
                }
                return left.deadline_ms < right.deadline_ms;
            });
        std::string center_text;
        if (timer_snapshot_.timers.empty()) {
            center_text = "NO TIMERS";
        } else if (next_active != timer_snapshot_.timers.end() &&
                   next_active->status == ProvisionsTimerSnapshot::TimerStatus::kActive &&
                   next_active->deadline_ms > now_ms) {
            center_text =
                ProvisionsStopWatch::EllipsizeUtf8(next_active->label, 18) + "\n" +
                ProvisionsStopwatchOrbit::FormatRemaining(next_active->deadline_ms, now_ms);
        } else {
            center_text = "ATTENTION";
        }
        if (orbit_center_label_ != nullptr) {
            lv_label_set_text(orbit_center_label_, center_text.c_str());
        }
        if (orbit_overflow_label_ != nullptr) {
            const int overflow = orbit_slot_board_.overflow_count();
            if (overflow > 0) {
                const std::string overflow_text = "+" + std::to_string(overflow);
                lv_label_set_text(orbit_overflow_label_, overflow_text.c_str());
                SetVisible(orbit_overflow_label_, true);
            } else {
                SetVisible(orbit_overflow_label_, false);
            }
        }

        const auto finished =
#if CONFIG_PROVISIONS_SCHEDULE_HARDWARE_BENCH
            [&]() {
                std::vector<ProvisionsTimerSnapshot::Timer> due;
                for (const auto& timer : timer_snapshot_.timers)
                    if (timer.status == ProvisionsTimerSnapshot::TimerStatus::kAttention)
                        due.push_back(timer);
                return due;
            }();
#else
            ProvisionsStopwatchOrbit::FinishedTimers(timer_snapshot_.timers, now_ms);
#endif
        const AlarmOutputChange output_change = timer_alarm_state_.Update(finished);
        timer_alarm_active_.store(timer_alarm_state_.active());
        if (!finished.empty()) {
            if (alarm_title_label_ != nullptr) {
                lv_label_set_text(alarm_title_label_,
                                  finished.size() == 1 ? "TIMER DONE" : "TIMERS DONE");
            }
            if (alarm_names_label_ != nullptr) {
                std::string names;
                const std::size_t shown =
                    std::min<std::size_t>(finished.size(), kOrbitAlarmMaximumNames);
                for (std::size_t index = 0; index < shown; ++index) {
                    if (!names.empty()) {
                        names.push_back('\n');
                    }
                    names.append(ProvisionsStopWatch::EllipsizeUtf8(finished[index].label, 20));
                }
                if (finished.size() > shown) {
                    names.append("\n+");
                    names.append(std::to_string(finished.size() - shown));
                    names.append(" MORE");
                }
                lv_label_set_text(alarm_names_label_, names.c_str());
            }
            if (alarm_hint_label_ != nullptr) {
                lv_label_set_text(alarm_hint_label_, timer_alarm_state_.silenced()
                                                         ? "SILENCED\nBLUE CLEARS"
                                                         : "BLUE SILENCES");
            }
        }
        SetReplyLayoutLocked(reply_visible_.load());
        return output_change;
    }

    void ApplyAlarmOutputChange(AlarmOutputChange change) {
        if (timer_alarm_output_callback_ == nullptr || change == AlarmOutputChange::kNone) {
            return;
        }
        timer_alarm_output_callback_(change == AlarmOutputChange::kStart);
    }

    void RefreshOrbit() {
        AlarmOutputChange output_change;
        {
            DisplayLockGuard lock(this);
            output_change = RefreshOrbitLocked();
        }
        ApplyAlarmOutputChange(output_change);
    }

    void CreateOrbitUiLocked(lv_obj_t* screen) {
        orbit_layer_ = lv_obj_create(screen);
        lv_obj_set_size(orbit_layer_, ProvisionsStopwatchOrbit::kDisplaySize,
                        ProvisionsStopwatchOrbit::kDisplaySize);
        lv_obj_set_style_pad_all(orbit_layer_, 0, 0);
        lv_obj_set_style_border_width(orbit_layer_, 0, 0);
        lv_obj_set_style_radius(orbit_layer_, 0, 0);
        lv_obj_set_style_bg_color(orbit_layer_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(orbit_layer_, LV_OPA_COVER, 0);
        lv_obj_clear_flag(orbit_layer_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_center(orbit_layer_);

        for (std::size_t index = 0; index < orbit_slot_objects_.size(); ++index) {
            const auto center = ProvisionsStopwatchOrbit::SlotCenter(static_cast<int>(index));
            auto& objects = orbit_slot_objects_[index];
            objects.arc = lv_arc_create(orbit_layer_);
            lv_obj_set_size(objects.arc, kOrbitArcDiameter, kOrbitArcDiameter);
            lv_obj_set_pos(objects.arc, center.x - ProvisionsStopwatchOrbit::kSlotRadius,
                           center.y - ProvisionsStopwatchOrbit::kSlotRadius);
            lv_arc_set_rotation(objects.arc, 270);
            lv_arc_set_bg_angles(objects.arc, 0, 360);
            lv_arc_set_range(objects.arc, 0, 1000);
            lv_arc_set_value(objects.arc, 1000);
            lv_obj_remove_style(objects.arc, nullptr, LV_PART_KNOB);
            lv_obj_clear_flag(objects.arc, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_arc_width(objects.arc, 6, LV_PART_MAIN);
            lv_obj_set_style_arc_width(objects.arc, 8, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(objects.arc, lv_color_hex(0x303030), LV_PART_MAIN);
            lv_obj_set_style_arc_color(objects.arc, lv_color_hex(kOrbitColors[index]),
                                       LV_PART_INDICATOR);

            objects.label = lv_label_create(orbit_layer_);
            lv_obj_set_width(objects.label, kOrbitLabelWidth);
            lv_obj_set_style_text_font(objects.label, &font_noto_sans_basic_16_4, 0);
            lv_obj_set_style_text_color(objects.label, lv_color_hex(kColorCream), 0);
            lv_obj_set_style_text_align(objects.label, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(objects.label, LV_LABEL_LONG_DOT);
            lv_obj_set_pos(objects.label, center.x - kOrbitLabelWidth / 2, center.y - 21);

            objects.remaining = lv_label_create(orbit_layer_);
            lv_obj_set_width(objects.remaining, kOrbitLabelWidth);
            lv_obj_set_style_text_font(objects.remaining, &font_noto_sans_basic_16_4, 0);
            lv_obj_set_style_text_color(objects.remaining, lv_color_hex(kOrbitColors[index]), 0);
            lv_obj_set_style_text_align(objects.remaining, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(objects.remaining, LV_LABEL_LONG_CLIP);
            lv_obj_set_pos(objects.remaining, center.x - kOrbitLabelWidth / 2, center.y + 5);
        }

        orbit_center_label_ = lv_label_create(orbit_layer_);
        lv_obj_set_width(orbit_center_label_, 150);
        lv_obj_set_style_text_font(orbit_center_label_, &font_noto_sans_basic_30_4, 0);
        lv_obj_set_style_text_color(orbit_center_label_, lv_color_hex(kColorCream), 0);
        lv_obj_set_style_text_align(orbit_center_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(orbit_center_label_, LV_LABEL_LONG_WRAP);
        lv_label_set_text(orbit_center_label_, "NO TIMERS");
        lv_obj_center(orbit_center_label_);

        orbit_overflow_label_ = lv_label_create(orbit_layer_);
        lv_obj_set_width(orbit_overflow_label_, 80);
        lv_obj_set_style_text_font(orbit_overflow_label_, &font_noto_sans_basic_16_4, 0);
        lv_obj_set_style_text_color(orbit_overflow_label_, lv_color_hex(kColorAmber), 0);
        lv_obj_set_style_text_align(orbit_overflow_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(orbit_overflow_label_, "");
        lv_obj_align(orbit_overflow_label_, LV_ALIGN_CENTER, 0, 62);

        alarm_layer_ = lv_obj_create(screen);
        lv_obj_set_size(alarm_layer_, ProvisionsStopwatchOrbit::kDisplaySize,
                        ProvisionsStopwatchOrbit::kDisplaySize);
        lv_obj_set_style_pad_all(alarm_layer_, 0, 0);
        lv_obj_set_style_border_width(alarm_layer_, 0, 0);
        lv_obj_set_style_radius(alarm_layer_, 0, 0);
        lv_obj_set_style_bg_color(alarm_layer_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(alarm_layer_, LV_OPA_COVER, 0);
        lv_obj_clear_flag(alarm_layer_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_center(alarm_layer_);

        alarm_title_label_ = lv_label_create(alarm_layer_);
        lv_obj_set_width(alarm_title_label_, 320);
        lv_obj_set_style_text_font(alarm_title_label_, &font_noto_sans_basic_30_4, 0);
        lv_obj_set_style_text_color(alarm_title_label_, lv_color_hex(kColorRed), 0);
        lv_obj_set_style_text_letter_space(alarm_title_label_, 2, 0);
        lv_obj_set_style_text_align(alarm_title_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(alarm_title_label_, "TIMER DONE");
        lv_obj_align(alarm_title_label_, LV_ALIGN_TOP_MID, 0, 96);

        alarm_names_label_ = lv_label_create(alarm_layer_);
        lv_obj_set_width(alarm_names_label_, 340);
        lv_obj_set_style_text_font(alarm_names_label_, &font_noto_sans_basic_30_4, 0);
        lv_obj_set_style_text_color(alarm_names_label_, lv_color_hex(kColorCream), 0);
        lv_obj_set_style_text_align(alarm_names_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(alarm_names_label_, 8, 0);
        lv_label_set_long_mode(alarm_names_label_, LV_LABEL_LONG_WRAP);
        lv_label_set_text(alarm_names_label_, "");
        lv_obj_align(alarm_names_label_, LV_ALIGN_CENTER, 0, -4);

        alarm_hint_label_ = lv_label_create(alarm_layer_);
        lv_obj_set_width(alarm_hint_label_, 240);
        lv_obj_set_style_text_font(alarm_hint_label_, &font_noto_sans_basic_16_4, 0);
        lv_obj_set_style_text_color(alarm_hint_label_, lv_color_hex(kColorAmber), 0);
        lv_obj_set_style_text_letter_space(alarm_hint_label_, 2, 0);
        lv_obj_set_style_text_align(alarm_hint_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(alarm_hint_label_, "BLUE SILENCES");
        lv_obj_align(alarm_hint_label_, LV_ALIGN_BOTTOM_MID, 0, -92);

        SetVisible(orbit_layer_, false);
        SetVisible(alarm_layer_, false);
    }

    void CancelReplyScroll() {
        if (reply_scroll_timer_ == nullptr) {
            return;
        }
        const esp_err_t result = esp_timer_stop(reply_scroll_timer_);
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Failed to stop reply scroll timer: %s", esp_err_to_name(result));
        }
    }

    void StartReplyScroll() {
        if (reply_scroll_timer_ == nullptr) {
            return;
        }
        CancelReplyScroll();
        const esp_err_t result = esp_timer_start_periodic(
            reply_scroll_timer_, static_cast<uint64_t>(kReplyScrollIntervalMs) * 1000);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start reply scroll timer: %s", esp_err_to_name(result));
        }
    }

    void RestartReplyFromTop() {
        CancelReplyScroll();
        {
            DisplayLockGuard lock(this);
            if (!reply_visible_.load() || reply_panel_ == nullptr) {
                return;
            }
            reply_generation_.fetch_add(1);
            lv_obj_scroll_to_y(reply_panel_, 0, LV_ANIM_OFF);
        }
        StartReplyScroll();
    }

    void ClearReplyLocked() {
        reply_visible_.store(false);
        reply_generation_.fetch_add(1);
        CancelReplyScroll();
        if (reply_label_ != nullptr) {
            lv_label_set_text(reply_label_, "");
        }
        if (reply_panel_ != nullptr) {
            lv_obj_scroll_to_y(reply_panel_, 0, LV_ANIM_OFF);
        }
        SetReplyLayoutLocked(false);
    }

    void ApplyChromeLocked(VisualState state, const StatePresentation& presentation) {
        if (hero_halo_ == nullptr || brand_mark_label_ == nullptr || emoji_label_ == nullptr ||
            emoji_box_ == nullptr || status_bar_ == nullptr || hint_panel_ == nullptr ||
            talk_button_dot_ == nullptr) {
            return;
        }

        const lv_color_t color = lv_color_hex(presentation.color);
        lv_obj_set_style_bg_color(hero_halo_, color, 0);
        lv_obj_set_style_border_color(hero_halo_, color, 0);
        lv_obj_set_style_bg_color(emoji_box_, color, 0);
        lv_obj_set_style_border_color(emoji_box_, color, 0);
        lv_obj_set_style_bg_color(status_bar_, color, 0);
        lv_obj_set_style_border_color(status_bar_, color, 0);
        lv_obj_set_style_bg_color(hint_panel_, color, 0);
        lv_obj_set_style_border_color(hint_panel_, color, 0);

        if (state == VisualState::kBoot) {
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(brand_mark_label_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(brand_mark_label_, LV_OBJ_FLAG_HIDDEN);
        }

        if (state == VisualState::kReady || state == VisualState::kListening) {
            lv_obj_remove_flag(talk_button_dot_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(talk_button_dot_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    static VisualState StateForStatus(const char* status) {
        if (status == nullptr || status[0] == '\0' || std::strcmp(status, "Boot") == 0 ||
            std::strcmp(status, Lang::Strings::INITIALIZING) == 0 ||
            std::strcmp(status, Lang::Strings::DETECTING_MODULE) == 0 ||
            std::strcmp(status, Lang::Strings::CHECKING_NEW_VERSION) == 0 ||
            std::strcmp(status, Lang::Strings::ACTIVATION) == 0 ||
            std::strcmp(status, Lang::Strings::LOADING_ASSETS) == 0 ||
            std::strcmp(status, Lang::Strings::OTA_UPGRADE) == 0 ||
            std::strcmp(status, Lang::Strings::UPGRADING) == 0) {
            return VisualState::kBoot;
        }
        if (std::strcmp(status, "Connecting") == 0 ||
            std::strcmp(status, Lang::Strings::CONNECTING) == 0 ||
            std::strcmp(status, Lang::Strings::REGISTERING_NETWORK) == 0 ||
            std::strcmp(status, Lang::Strings::LOADING_PROTOCOL) == 0 ||
            std::strcmp(status, Lang::Strings::SERVER_NOT_FOUND) == 0) {
            return VisualState::kConnecting;
        }
        if (std::strcmp(status, "Ready") == 0 || std::strcmp(status, "Saved on Orbit") == 0) {
            return VisualState::kReady;
        }
        if (std::strcmp(status, "Working") == 0 || std::strcmp(status, "Saving") == 0 ||
            std::strcmp(status, "Retry queued") == 0 ||
            std::strcmp(status, "Preparing microphone") == 0) {
            return VisualState::kWorking;
        }
        if (std::strcmp(status, "Listening") == 0 ||
            std::strcmp(status, Lang::Strings::LISTENING) == 0) {
            return VisualState::kListening;
        }
        if (std::strcmp(status, "Speaking") == 0 ||
            std::strcmp(status, Lang::Strings::SPEAKING) == 0) {
            return VisualState::kSpeaking;
        }
        if (std::strcmp(status, "Unavailable") == 0 || std::strcmp(status, "Couldn't save") == 0 ||
            std::strcmp(status, "Capture unavailable") == 0 ||
            std::strcmp(status, "Hold blue to retry") == 0 ||
            std::strcmp(status, "Recording kept") == 0 ||
            std::strcmp(status, Lang::Strings::ERROR) == 0 ||
            std::strcmp(status, Lang::Strings::SERVER_ERROR) == 0 ||
            std::strcmp(status, Lang::Strings::SERVER_NOT_CONNECTED) == 0 ||
            std::strcmp(status, Lang::Strings::SERVER_TIMEOUT) == 0) {
            return VisualState::kUnavailable;
        }
        return VisualState::kBoot;
    }

    // Inbound strings are the gateway's display labels — matching must track
    // the gateway's _DISPLAY_TEXT_BY_TOOL sets. Output titles are banners.
    static VisualState StateForNotification(const char* notification, const char** title) {
        if (notification == nullptr) {
            *title = "NOTICE";
            return VisualState::kNotice;
        }
        if (std::strcmp(notification, "Added") == 0) {
            *title = "ADDED";
            return VisualState::kAdded;
        }
        if (std::strcmp(notification, "Undone") == 0) {
            *title = "REMOVED";
            return VisualState::kAdded;
        }
        if (std::strcmp(notification, "Found") == 0) {
            *title = "FOUND";
            return VisualState::kSuccess;
        }
        if (std::strcmp(notification, "Delivered") == 0) {
            *title = "DELIVERED";
            return VisualState::kSuccess;
        }
        if (std::strcmp(notification, "On the way") == 0) {
            *title = "ON THE WAY";
            return VisualState::kSuccess;
        }
        if (std::strcmp(notification, "Replied") == 0) {
            *title = "REPLIED";
            return VisualState::kSuccess;
        }
        if (std::strcmp(notification, "No new reply") == 0) {
            *title = "NO NEW REPLY";
            return VisualState::kWarning;
        }
        if (std::strcmp(notification, "Reply waiting") == 0) {
            *title = "REPLY WAITING";
            return VisualState::kNotice;
        }
        if (std::strcmp(notification, "No thread") == 0) {
            *title = "NO THREAD";
            return VisualState::kWarning;
        }
        if (std::strcmp(notification, "Draft only") == 0) {
            *title = "DRAFT";
            return VisualState::kDraft;
        }
        if (std::strcmp(notification, "Recorded") == 0) {
            *title = "RECORDED";
            return VisualState::kRecorded;
        }
        if (std::strcmp(notification, "Choose one") == 0) {
            *title = "CHOOSE ONE";
            return VisualState::kQuestion;
        }
        if (std::strcmp(notification, "Need unit") == 0) {
            *title = "WHICH UNIT?";
            return VisualState::kQuestion;
        }
        if (std::strcmp(notification, "Ready to add") == 0) {
            *title = "READY TO ADD";
            return VisualState::kQuestion;
        }
        if (std::strcmp(notification, "No match") == 0) {
            *title = "NO MATCH";
            return VisualState::kWarning;
        }
        if (std::strcmp(notification, "Cancelled") == 0) {
            *title = "CANCELLED";
            return VisualState::kWarning;
        }
        if (std::strcmp(notification, "Check app") == 0) {
            *title = "CHECK APP";
            return VisualState::kWarning;
        }
        if (std::strcmp(notification, "Not changed") == 0) {
            *title = "NO CHANGE";
            return VisualState::kWarning;
        }
        *title = notification;
        return VisualState::kNotice;
    }

    void ApplyVisualStateLocked(VisualState state) {
        if (status_label_ == nullptr || notification_label_ == nullptr || emoji_label_ == nullptr ||
            emoji_box_ == nullptr || hint_label_ == nullptr || hero_halo_ == nullptr ||
            hint_panel_ == nullptr || reply_header_label_ == nullptr ||
            reply_panel_ == nullptr || reply_label_ == nullptr) {
            return;
        }
        ClearReplyLocked();
        const auto presentation = PresentationFor(state);
        const lv_color_t color = lv_color_hex(presentation.color);

        lv_label_set_text(status_label_, presentation.title);
        lv_obj_set_style_text_color(status_label_, lv_color_hex(kColorCream), 0);
        lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

        lv_label_set_text(emoji_label_, presentation.icon);
        lv_obj_set_style_text_color(emoji_label_, color, 0);

        lv_label_set_text(hint_label_, presentation.hint);
        lv_obj_set_style_text_color(hint_label_, color, 0);
        ApplyChromeLocked(state, presentation);
        const auto crest_state = CrestStateFor(state);
        if (crest_state != OrbitCrest::State::Result) {
            if (crest_state != OrbitCrest::State::Speaking &&
                crest_state != OrbitCrest::State::Thinking &&
                crest_state != OrbitCrest::State::Idle) {
                ClearCrestResultLocked();
            }
            if (crest_state_ != crest_state)
                ChangeCrestStateLocked(crest_state);
        }
        SetReplyLayoutLocked(false);
        last_status_update_time_ = std::chrono::system_clock::now();
    }

    void ApplyVisualState(VisualState state) {
        DisplayLockGuard lock(this);
        ApplyVisualStateLocked(state);
    }

    void ApplyRestingVisualState() {
        // Load the state only after taking the LVGL lock so an expiring receipt
        // cannot overwrite a newer Ready/Listening transition with a stale snapshot.
        DisplayLockGuard lock(this);
        ApplyVisualStateLocked(resting_state_.load());
    }

    void CancelVisualReset() {
        visual_reset_deadline_us_.store(0);
        if (visual_reset_timer_ != nullptr) {
            const esp_err_t result = esp_timer_stop(visual_reset_timer_);
            if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "Failed to stop screen reset timer: %s", esp_err_to_name(result));
            }
        }
    }

    bool ScheduleVisualReset(int duration_ms) {
        if (visual_reset_timer_ == nullptr) {
            return false;
        }
        const int64_t deadline =
            esp_timer_get_time() + static_cast<int64_t>(duration_ms) * 1000;
        visual_reset_deadline_us_.store(deadline);
        const esp_err_t stop_result = esp_timer_stop(visual_reset_timer_);
        if (stop_result != ESP_OK && stop_result != ESP_ERR_INVALID_STATE) {
            visual_reset_deadline_us_.store(0);
            ESP_LOGE(TAG, "Failed to stop screen reset timer: %s", esp_err_to_name(stop_result));
            return false;
        }
        const esp_err_t result =
            esp_timer_start_once(visual_reset_timer_, static_cast<uint64_t>(duration_ms) * 1000);
        if (result != ESP_OK) {
            visual_reset_deadline_us_.store(0);
            ESP_LOGE(TAG, "Failed to start screen reset timer: %s", esp_err_to_name(result));
            return false;
        }
        return true;
    }

#endif

public:
    static void rounder_event_cb(lv_event_t* e) {
        lv_area_t* area = static_cast<lv_area_t*>(lv_event_get_param(e));
        area->x1 = (area->x1 >> 1) << 1;
        area->y1 = (area->y1 >> 1) << 1;
        area->x2 = ((area->x2 >> 1) << 1) + 1;
        area->y2 = ((area->y2 >> 1) << 1) + 1;
    }

    RoundLcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                    esp_lcd_panel_handle_t panel_handle,
                    int width,
                    int height,
                    int offset_x,
                    int offset_y,
                    bool mirror_x,
                    bool mirror_y,
                    bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle, width, height, offset_x, offset_y, mirror_x,
                        mirror_y, swap_xy, kStopwatchInitialClearColor) {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                auto* self = static_cast<RoundLcdDisplay*>(arg);
                const int64_t deadline = self->visual_reset_deadline_us_.load();
                if (deadline <= 0 || esp_timer_get_time() < deadline) {
                    return;
                }
                Application::GetInstance().Schedule([self, deadline]() {
                    if (self->visual_reset_deadline_us_.load() != deadline ||
                        esp_timer_get_time() < deadline) {
                        return;
                    }
                    self->visual_reset_deadline_us_.store(0);
                    self->receipt_visible_.store(false);
                    if (self->resting_state_.load() == VisualState::kSpeaking) {
                        // A reply timer expiring while still Speaking means the
                        // gateway stop frame was lost. Never restore a stuck
                        // "Replying" screen; the application watchdog closes
                        // the channel and reconnects independently.
                        self->resting_state_.store(VisualState::kUnavailable);
                    }
                    self->ApplyRestingVisualState();
                });
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "stopwatch_visual_reset",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &visual_reset_timer_));

        esp_timer_create_args_t scroll_timer_args = {
            .callback = [](void* arg) {
                auto* self = static_cast<RoundLcdDisplay*>(arg);
                const uint32_t generation = self->reply_generation_.load();
                if (!self->reply_visible_.load() || self->power_save_active_.load()) {
                    return;
                }
                Application::GetInstance().Schedule([self, generation]() {
                    if (!self->reply_visible_.load() || self->power_save_active_.load() ||
                        self->reply_generation_.load() != generation) {
                        return;
                    }
                    DisplayLockGuard lock(self);
                    if (!self->reply_visible_.load() || self->power_save_active_.load() ||
                        self->reply_generation_.load() != generation ||
                        self->reply_panel_ == nullptr) {
                        return;
                    }
                    lv_obj_update_layout(self->reply_panel_);
                    const int32_t remaining = lv_obj_get_scroll_bottom(self->reply_panel_);
                    if (remaining <= 0) {
                        if (lv_obj_get_scroll_y(self->reply_panel_) > 0) {
                            lv_obj_scroll_to_y(self->reply_panel_, 0, LV_ANIM_ON);
                        } else {
                            // The whole reply fits on one page. Stop periodic
                            // wakeups until another reply is shown.
                            self->CancelReplyScroll();
                        }
                        return;
                    }
                    const int32_t step =
                        remaining < kReplyScrollStep ? remaining : kReplyScrollStep;
                    lv_obj_scroll_to_y(self->reply_panel_,
                                       lv_obj_get_scroll_y(self->reply_panel_) + step,
                                       LV_ANIM_ON);
                });
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "stopwatch_reply_scroll",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&scroll_timer_args, &reply_scroll_timer_));

        esp_timer_create_args_t orbit_timer_args = {
            .callback = [](void* arg) {
                auto* self = static_cast<RoundLcdDisplay*>(arg);
                Application::GetInstance().Schedule([self]() { self->RefreshOrbit(); });
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "stopwatch_orbit_tick",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&orbit_timer_args, &orbit_tick_timer_));
#endif
    }

    ~RoundLcdDisplay() override {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        {
            DisplayLockGuard lock(this);
            if (crest_animation_timer_ != nullptr) {
                lv_timer_delete(crest_animation_timer_);
                crest_animation_timer_ = nullptr;
            }
        }
        if (visual_reset_timer_ != nullptr) {
            esp_timer_stop(visual_reset_timer_);
            esp_timer_delete(visual_reset_timer_);
        }
        if (reply_scroll_timer_ != nullptr) {
            esp_timer_stop(reply_scroll_timer_);
            esp_timer_delete(reply_scroll_timer_);
        }
        if (orbit_tick_timer_ != nullptr) {
            esp_timer_stop(orbit_tick_timer_);
            esp_timer_delete(orbit_tick_timer_);
        }
#endif
    }

    void SetupUI() override {
        SpiLcdDisplay::SetupUI();
        DisplayLockGuard lock(this);

        lv_display_add_event_cb(display_, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);

#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        auto* screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_color(container_, lv_color_hex(0x000000), 0);

        // Keep network and battery indicators inside the circular display's safe arc.
        lv_obj_set_width(top_bar_, kRoundTopBarWidth);
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(top_bar_, 0, 0);
        lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, kRoundTopBarOffset);
        lv_obj_set_style_text_color(network_label_, lv_color_hex(kColorCream), 0);
        lv_obj_set_style_text_color(mute_label_, lv_color_hex(kColorCream), 0);
        lv_obj_set_style_text_color(battery_label_, lv_color_hex(kColorCream), 0);

        // Brand and product title use separate type scales so the opening frame
        // reads as a product, not two equal lines of status copy.
        brand_label_ = lv_label_create(screen);
        lv_obj_set_width(brand_label_, kRoundBrandWidth);
        lv_obj_set_style_text_font(brand_label_, &font_noto_sans_basic_16_4, 0);
        lv_obj_set_style_text_color(brand_label_, lv_color_hex(kColorGold), 0);
        lv_obj_set_style_text_letter_space(brand_label_, 4, 0);
        lv_obj_set_style_text_align(brand_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(brand_label_, LV_LABEL_LONG_CLIP);
        lv_label_set_text(brand_label_, "PROVISIONS");
        lv_obj_align(brand_label_, LV_ALIGN_TOP_MID, 0, kRoundBrandTopOffset);

        title_label_ = lv_label_create(screen);
        lv_obj_set_width(title_label_, kRoundContentWidth);
        lv_obj_set_style_text_font(title_label_, &font_noto_sans_basic_30_4, 0);
        lv_obj_set_style_text_color(title_label_, lv_color_hex(kColorCream), 0);
        lv_obj_set_style_text_letter_space(title_label_, 1, 0);
        lv_obj_set_style_text_align(title_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(title_label_, LV_LABEL_LONG_CLIP);
        lv_label_set_text(title_label_, "KITCHEN HELPER");
        lv_obj_align(title_label_, LV_ALIGN_TOP_MID, 0, kRoundTitleTopOffset);

        brand_rule_ = lv_obj_create(screen);
        lv_obj_set_size(brand_rule_, 68, 3);
        lv_obj_set_style_radius(brand_rule_, 2, 0);
        lv_obj_set_style_pad_all(brand_rule_, 0, 0);
        lv_obj_set_style_border_width(brand_rule_, 0, 0);
        lv_obj_set_style_bg_color(brand_rule_, lv_color_hex(kColorGold), 0);
        lv_obj_set_style_bg_opa(brand_rule_, LV_OPA_40, 0);
        lv_obj_clear_flag(brand_rule_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(brand_rule_, LV_ALIGN_TOP_MID, 0, kRoundRuleTopOffset);

        // A restrained two-ring hero gives every state a clear focal point while
        // keeping most AMOLED pixels black. The inner box remains the shared
        // state-icon owner used by the existing display implementation.
        hero_halo_ = lv_obj_create(screen);
        lv_obj_set_size(hero_halo_, kRoundHeroSize, kRoundHeroSize);
        lv_obj_set_style_radius(hero_halo_, kRoundHeroSize / 2, 0);
        lv_obj_set_style_pad_all(hero_halo_, 0, 0);
        lv_obj_set_style_bg_opa(hero_halo_, LV_OPA_10, 0);
        lv_obj_set_style_border_width(hero_halo_, 2, 0);
        lv_obj_set_style_border_opa(hero_halo_, LV_OPA_20, 0);
        lv_obj_clear_flag(hero_halo_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(hero_halo_, LV_ALIGN_CENTER, 0, kRoundHeroOffset);

        lv_obj_set_parent(emoji_box_, hero_halo_);
        lv_obj_set_size(emoji_box_, kRoundIconSize, kRoundIconSize);
        lv_obj_set_style_radius(emoji_box_, kRoundIconSize / 2, 0);
        lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_20, 0);
        lv_obj_set_style_border_width(emoji_box_, 2, 0);
        lv_obj_set_style_border_opa(emoji_box_, LV_OPA_40, 0);
        lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_center(emoji_label_);

        brand_mark_label_ = lv_label_create(emoji_box_);
        lv_obj_set_style_text_font(brand_mark_label_, &font_noto_sans_basic_30_4, 0);
        lv_obj_set_style_text_color(brand_mark_label_, lv_color_hex(kColorGold), 0);
        lv_obj_set_style_text_letter_space(brand_mark_label_, 1, 0);
        lv_label_set_text(brand_mark_label_, "P");
        lv_obj_center(brand_mark_label_);

        // The status and instruction capsules separate live state from action.
        // Their low-opacity fills are state-colored but OLED/power conservative.
        lv_obj_set_size(status_bar_, kRoundStatusWidth, kRoundStatusHeight);
        lv_obj_set_style_radius(status_bar_, kRoundStatusHeight / 2, 0);
        lv_obj_set_style_pad_all(status_bar_, 0, 0);
        lv_obj_set_style_bg_opa(status_bar_, LV_OPA_10, 0);
        lv_obj_set_style_border_width(status_bar_, 1, 0);
        lv_obj_set_style_border_opa(status_bar_, LV_OPA_20, 0);
        lv_obj_align(status_bar_, LV_ALIGN_CENTER, 0, kRoundStatusOffset);
        lv_obj_set_width(status_label_, kRoundStatusWidth - 24);
        lv_obj_set_width(notification_label_, kRoundStatusWidth - 24);
        lv_obj_set_style_text_font(status_label_, &font_noto_sans_basic_30_4, 0);
        lv_obj_set_style_text_font(notification_label_, &font_noto_sans_basic_30_4, 0);
        lv_label_set_long_mode(status_label_, LV_LABEL_LONG_CLIP);
        lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_CLIP);
        lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);

        hint_panel_ = lv_obj_create(screen);
        lv_obj_set_size(hint_panel_, kRoundHintWidth, kRoundHintHeight);
        lv_obj_set_style_radius(hint_panel_, kRoundHintHeight / 2, 0);
        lv_obj_set_style_pad_all(hint_panel_, 0, 0);
        lv_obj_set_style_bg_opa(hint_panel_, LV_OPA_10, 0);
        lv_obj_set_style_border_width(hint_panel_, 1, 0);
        lv_obj_set_style_border_opa(hint_panel_, LV_OPA_20, 0);
        lv_obj_clear_flag(hint_panel_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(hint_panel_, LV_ALIGN_CENTER, 0, kRoundHintOffset);

        talk_button_dot_ = lv_obj_create(hint_panel_);
        lv_obj_set_size(talk_button_dot_, 8, 8);
        lv_obj_set_style_radius(talk_button_dot_, 4, 0);
        lv_obj_set_style_pad_all(talk_button_dot_, 0, 0);
        lv_obj_set_style_border_width(talk_button_dot_, 0, 0);
        lv_obj_set_style_bg_color(talk_button_dot_, lv_color_hex(kColorTalkButton), 0);
        lv_obj_set_style_bg_opa(talk_button_dot_, LV_OPA_COVER, 0);
        lv_obj_clear_flag(talk_button_dot_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(talk_button_dot_, LV_ALIGN_LEFT_MID, 16, 0);

        hint_label_ = lv_label_create(hint_panel_);
        lv_obj_set_width(hint_label_, kRoundHintWidth - 32);
        lv_obj_set_style_text_font(hint_label_, &font_noto_sans_basic_16_4, 0);
        lv_obj_set_style_text_align(hint_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(hint_label_, LV_LABEL_LONG_CLIP);
        lv_obj_align(hint_label_, LV_ALIGN_CENTER, 5, 0);

        // Assistant replies arrive in the authenticated TTS sentence frame.
        // Give the full text a dedicated, readable surface instead of trying
        // to squeeze private order detail into the one-line receipt capsule.
        reply_header_label_ = lv_label_create(screen);
        lv_obj_set_width(reply_header_label_, kReplyHeaderWidth);
        // Banner-sized: the verdict ("REPLIED", "NO NEW REPLY") reads first,
        // in its state colour; the sentence below is detail.
        lv_obj_set_style_text_font(reply_header_label_, &font_noto_sans_basic_30_4, 0);
        lv_obj_set_style_text_color(reply_header_label_, lv_color_hex(kColorGold), 0);
        lv_obj_set_style_text_letter_space(reply_header_label_, 2, 0);
        lv_obj_set_style_text_align(reply_header_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(reply_header_label_, LV_LABEL_LONG_CLIP);
        lv_label_set_text(reply_header_label_, "REPLY");
        lv_obj_align(reply_header_label_, LV_ALIGN_TOP_MID, 0, kReplyHeaderTopOffset);
        lv_obj_add_flag(reply_header_label_, LV_OBJ_FLAG_HIDDEN);

        reply_panel_ = lv_obj_create(screen);
        lv_obj_set_size(reply_panel_, kReplyPanelWidth, kReplyPanelHeight);
        lv_obj_set_style_radius(reply_panel_, 32, 0);
        lv_obj_set_style_pad_all(reply_panel_, 20, 0);
        // No box at all: banner and text float on the same true black as the
        // bezel, so the screen melts into the device frame.
        lv_obj_set_style_bg_color(reply_panel_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(reply_panel_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(reply_panel_, 0, 0);
        lv_obj_set_scroll_dir(reply_panel_, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(reply_panel_, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_align(reply_panel_, LV_ALIGN_CENTER, 0, kReplyPanelOffset);
        lv_obj_add_flag(reply_panel_, LV_OBJ_FLAG_HIDDEN);

        reply_label_ = lv_label_create(reply_panel_);
        lv_obj_set_width(reply_label_, kReplyPanelWidth - 40);
        lv_obj_set_height(reply_label_, LV_SIZE_CONTENT);
        lv_obj_set_style_text_font(reply_label_, &font_noto_sans_basic_30_4, 0);
        lv_obj_set_style_text_color(reply_label_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_align(reply_label_, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_line_space(reply_label_, 8, 0);
        lv_label_set_long_mode(reply_label_, LV_LABEL_LONG_WRAP);
        lv_label_set_text(reply_label_, "");
        lv_obj_align(reply_label_, LV_ALIGN_TOP_MID, 0, 0);

        CreateCrestUiLocked(screen);
        CreateOrbitUiLocked(screen);
#if CONFIG_PROVISIONS_SCHEDULE_BENCH_DEMO
        schedule_view_.Create(screen, &font_noto_sans_basic_30_4, &font_noto_sans_basic_16_4);
        schedule_view_.Render(schedule_demo_.model(),
                              orbit::service_schedule::ScheduleDemo::FixtureTime, true);
#endif

        hide_subtitle_ = true;
        if (bottom_bar_ != nullptr) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        ApplyVisualStateLocked(VisualState::kBoot);
#if !CONFIG_PROVISIONS_SCHEDULE_HARDWARE_BENCH
        ESP_ERROR_CHECK(esp_timer_start_periodic(orbit_tick_timer_, kOrbitTickIntervalUs));
#endif
#else
        // Generic StopWatch layout remains unchanged.
        lv_obj_set_style_pad_left(status_bar_, LV_HOR_RES * 0.2, 0);
        lv_obj_set_style_pad_right(status_bar_, LV_HOR_RES * 0.2, 0);
        lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, DISPLAY_STATUS_BAR_TOP_OFF);
        lv_obj_set_width(status_label_, LV_HOR_RES * 0.6);
        lv_obj_set_width(notification_label_, LV_HOR_RES * 0.6);
        if (bottom_bar_ != nullptr) {
            lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -DISPLAY_CHAT_BAR_BOTTOM_OFF);
            lv_obj_set_width(chat_message_label_, LV_HOR_RES * 0.75);
        }
        if (top_bar_ != nullptr) {
            lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, DISPLAY_ROUND_EDGE_INSET / 2);
            lv_obj_set_style_pad_top(top_bar_, DISPLAY_ROUND_EDGE_INSET / 4, 0);
        }
#endif
    }

#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
#if CONFIG_PROVISIONS_SCHEDULE_BENCH_DEMO
    void AdvanceScheduleDemo() {
        AlarmOutputChange change;
        {
            DisplayLockGuard lock(this);
            schedule_demo_.Advance();
            ESP_LOGI(TAG, "SYNTHETIC DEMO: %s", schedule_demo_.stage());
            change = RefreshOrbitLocked();
        }
        ApplyAlarmOutputChange(change);
    }
#endif
#if CONFIG_PROVISIONS_SCHEDULE_HARDWARE_BENCH
    void RenderHardwareBench(
        const std::shared_ptr<const orbit::service_schedule::WorkerPublication>& publication,
        const std::string& status) {
        DisplayLockGuard lock(this);
        timer_snapshot_.timers.clear();
        bench_clock_trusted_ = false;
        bench_clock_ms_ = 0;
        if (publication && publication->face) {
            const auto& model = *publication->face;
            bench_clock_ms_ = model.scheduler().now_ms();
            bench_clock_trusted_ =
                model.scheduler().clock_state() == orbit::service_schedule::ClockState::Trusted;
            if (const auto* snapshot = model.scheduler().snapshot()) {
                for (const auto& item : model.scheduler().items()) {
                    if (item.acknowledged)
                        continue;
                    ProvisionsTimerSnapshot::Timer timer;
                    timer.id = item.key.id + "/" + std::to_string(item.key.revision);
                    timer.status = item.due ? ProvisionsTimerSnapshot::TimerStatus::kAttention
                                            : ProvisionsTimerSnapshot::TimerStatus::kActive;
                    if (item.key.kind == orbit::service_schedule::ItemKind::Timer) {
                        for (const auto& source : snapshot->timers)
                            if (source.id == item.key.id) {
                                timer.label = source.label;
                                timer.deadline_ms = source.deadline_ms;
                            }
                    } else {
                        for (const auto& source : snapshot->cues)
                            if (source.id == item.key.id) {
                                timer.label = source.label;
                                timer.deadline_ms = source.deadline_ms;
                            }
                    }
                    timer_snapshot_.timers.push_back(std::move(timer));
                }
            }
        }
        orbit_snapshot_received_ = true;
        resting_state_.store(VisualState::kReady);
        receipt_visible_.store(false);
        reply_visible_.store(false);
        // Reuse the existing dial geometry/labels; only the worker authors due
        // and ACK state. No ordinary alarm-output callback is registered here.
        RefreshOrbitLocked();
        lv_label_set_text(orbit_center_label_, "BENCH");
        lv_obj_align(orbit_center_label_, LV_ALIGN_CENTER, 0, -25);
        lv_obj_set_width(orbit_overflow_label_, 150);
        lv_label_set_long_mode(orbit_overflow_label_, LV_LABEL_LONG_WRAP);
        lv_obj_align(orbit_overflow_label_, LV_ALIGN_CENTER, 0, 32);
        lv_label_set_text(orbit_overflow_label_, status.c_str());
        SetVisible(orbit_overflow_label_, true);
        lv_label_set_text(alarm_title_label_, "BENCH ALERT");
        const std::string hint = "BLUE ACKS ONE\n" + status;
        lv_label_set_text(alarm_hint_label_, hint.c_str());
    }
#endif
    void SetTimerAlarmOutputCallback(std::function<void(bool)> callback) {
        timer_alarm_output_callback_ = std::move(callback);
    }

    // Runs on the Application task after a dismiss gesture cleared the takeover.
    void SetTimerDismissCallback(std::function<void()> callback) {
        timer_dismiss_callback_ = std::move(callback);
    }

    void ApplyTimerSnapshot(const ProvisionsTimerSnapshot::Snapshot& snapshot) {
        AlarmOutputChange output_change;
        {
            DisplayLockGuard lock(this);
            const bool same_galley_session =
                orbit_snapshot_received_ && snapshot.session_id == galley_session_id_;
            const auto previous_timers = timer_snapshot_.timers;
            if (!same_galley_session) {
                orbit_slot_board_ = ProvisionsStopwatchOrbit::SlotBoard{};
                timer_alarm_state_.BeginNewSession();
            }
            orbit_snapshot_received_ = true;
            galley_session_id_ = snapshot.session_id;
            timer_snapshot_ = snapshot;
            HideDismissedTimersLocked();
            if (same_galley_session) {
                ProvisionsStopwatchOrbit::PreserveAttention(previous_timers,
                                                            timer_snapshot_.timers);
            }
            snapshot_received_monotonic_ms_ = esp_timer_get_time() / 1000;
            output_change = RefreshOrbitLocked();
        }
        ApplyAlarmOutputChange(output_change);
    }

    void ResetTimerSnapshot() {
        AlarmOutputChange output_change;
        {
            DisplayLockGuard lock(this);
            orbit_snapshot_received_ = false;
            timer_alarm_active_.store(false);
            snapshot_received_monotonic_ms_ = 0;
            galley_session_id_.clear();
            timer_snapshot_ = ProvisionsTimerSnapshot::Snapshot{};
            orbit_slot_board_ = ProvisionsStopwatchOrbit::SlotBoard{};
            output_change = timer_alarm_state_.Reset();
            SetReplyLayoutLocked(reply_visible_.load());
        }
        ApplyAlarmOutputChange(output_change);
    }

    // First blue gesture on a ringing takeover silences it; the next blue
    // gesture on the silenced takeover dismisses every due timer: the takeover
    // clears and the motor stops at once, and the gateway is told afterwards.
    bool SilenceTimerAlarm() {
        AlarmOutputChange output_change = AlarmOutputChange::kNone;
        bool dismissed = false;
        {
            DisplayLockGuard lock(this);
#if CONFIG_PROVISIONS_SCHEDULE_BENCH_DEMO
            if (!schedule_demo_.Acknowledge())
                return false;
            output_change = RefreshOrbitLocked();
#else
            if (!timer_alarm_active_.load()) {
                return false;
            }
            if (timer_alarm_state_.silenced()) {
                // Dismissal removes the takeover, so a silenced timer can no
                // longer consume every later blue gesture and trap retry/dictation.
                output_change = DismissDueTimersLocked();
                dismissed = true;
            } else {
                output_change = timer_alarm_state_.Silence();
                if (output_change != AlarmOutputChange::kStop)
                    return false;
                if (alarm_hint_label_ != nullptr) {
                    lv_label_set_text(alarm_hint_label_, "SILENCED\nBLUE CLEARS");
                }
            }
#endif
        }
        ApplyAlarmOutputChange(output_change);
        if (dismissed && timer_dismiss_callback_)
            timer_dismiss_callback_();
        return true;
    }

    // Removes the due timers from the face and remembers them so a snapshot
    // still in flight cannot bring the takeover back. Caller holds the lock.
    AlarmOutputChange DismissDueTimersLocked() {
        const auto due = ProvisionsStopwatchOrbit::FinishedTimers(timer_snapshot_.timers,
                                                                  EffectiveServerNowMs());
        for (const auto& timer : due) {
            if (dismissed_timers_.size() >= ProvisionsTimerSnapshot::kMaximumTimers)
                dismissed_timers_.erase(dismissed_timers_.begin());
            dismissed_timers_.push_back({timer.id, timer.deadline_ms});
        }
        HideDismissedTimersLocked();
        return RefreshOrbitLocked();
    }

    // A dismissed timer is forgotten once a snapshot omits it; one that comes
    // back with a new deadline (extended or re-armed) is shown again.
    void HideDismissedTimersLocked() {
        auto& timers = timer_snapshot_.timers;
        dismissed_timers_.erase(
            std::remove_if(dismissed_timers_.begin(), dismissed_timers_.end(),
                           [&timers](const DismissedTimer& gone) {
                               return std::none_of(timers.begin(), timers.end(),
                                                   [&gone](const auto& timer) {
                                                       return timer.id == gone.id;
                                                   });
                           }),
            dismissed_timers_.end());
        timers.erase(std::remove_if(timers.begin(), timers.end(),
                                    [this](const auto& timer) {
                                        return std::any_of(
                                            dismissed_timers_.begin(), dismissed_timers_.end(),
                                            [&timer](const DismissedTimer& gone) {
                                                return gone.id == timer.id &&
                                                       gone.deadline_ms == timer.deadline_ms;
                                            });
                                    }),
                     timers.end());
    }

    bool HasTimerAlarm() const { return timer_alarm_active_.load(); }

    void SetEmotion(const char* emotion) override { (void)emotion; }

    void SetChatMessage(const char* role, const char* content) override {
        if (role == nullptr || content == nullptr || content[0] == '\0' ||
            std::strcmp(role, "assistant") != 0) {
            return;
        }

        CancelVisualReset();
        if (notification_timer_ != nullptr) {
            esp_timer_stop(notification_timer_);
        }
        receipt_visible_.store(true);
        reply_visible_.store(true);
        {
            DisplayLockGuard lock(this);
            if (crest_layer_ == nullptr) {
                receipt_visible_.store(false);
                reply_visible_.store(false);
                return;
            }
            // No transcript text is retained or rendered; the watch face
            // acknowledges only that a reply arrived.
            crest_reply_received_ = true;
            crest_reply_started_ms_ = CrestNowMs();
            SetCrestResultLocked("Reply received", kReplyPlaybackMaximumMs);
            SetReplyLayoutLocked(true);
        }
        if (!ScheduleVisualReset(kReplyPlaybackMaximumMs)) {
            receipt_visible_.store(false);
            ApplyRestingVisualState();
        }
    }

    void SetPowerSaveMode(bool on) override {
        power_save_active_.store(on);
        DisplayLockGuard lock(this);
        if (crest_animation_timer_ != nullptr) {
            if (on)
                lv_timer_pause(crest_animation_timer_);
            else
                lv_timer_resume(crest_animation_timer_);
        }
        lv_obj_t* chrome[] = {top_bar_,     brand_label_, title_label_, brand_rule_,
                              hero_halo_,   status_bar_,  hint_panel_,  reply_header_label_,
                              reply_panel_, crest_layer_, orbit_layer_, alarm_layer_};
        for (auto* object : chrome) {
            if (object != nullptr) {
                lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (!on) {
            if (top_bar_ != nullptr) {
                lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
            }
            if (brand_label_ != nullptr) {
                lv_obj_remove_flag(brand_label_, LV_OBJ_FLAG_HIDDEN);
            }
            SetReplyLayoutLocked(reply_visible_.load());
            RenderCrestLocked();
        }
    }

    void SetStatus(const char* status) override {
        if (IsClockStatus(status)) {
            return;
        }
        const VisualState state = StateForStatus(status);
        resting_state_.store(state);
        if (receipt_visible_.load() && state != VisualState::kListening &&
            state != VisualState::kConnecting && state != VisualState::kUnavailable) {
            if (reply_visible_.load() && state == VisualState::kReady) {
                RestartReplyFromTop();
                if (!ScheduleVisualReset(kReplyHoldAfterSpeechMs)) {
                    receipt_visible_.store(false);
                    ApplyRestingVisualState();
                }
            }
            return;
        }
        receipt_visible_.store(false);
        CancelVisualReset();
        if (notification_timer_ != nullptr) {
            esp_timer_stop(notification_timer_);
        }
        ApplyVisualState(state);
    }

    void ShowNotification(const char* notification, int duration_ms = 3000) override {
        const char* title = nullptr;
        const VisualState state = StateForNotification(notification, &title);
        const char* caption = OrbitCrest::ResultCaption(notification);
        if (!caption[0])
            return;
        ShowReceipt(title, state, caption, duration_ms);
    }

    void ShowLocalCaptureReceipt(int duration_ms = 1800) override {
        ShowReceipt("RECORDED", VisualState::kLocalRecorded, "Recorded\non Orbit", duration_ms);
    }

private:
    void ShowReceipt(const char* title, VisualState state, const char* crest_caption,
                     int duration_ms) {
        receipt_visible_.store(true);
        {
            DisplayLockGuard lock(this);
            if (status_label_ == nullptr || notification_label_ == nullptr ||
                emoji_label_ == nullptr || emoji_box_ == nullptr || hint_label_ == nullptr ||
                hero_halo_ == nullptr || hint_panel_ == nullptr) {
                receipt_visible_.store(false);
                return;
            }
            ClearReplyLocked();
            lv_label_set_text(notification_label_, title);
            lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);

            const auto presentation = PresentationFor(state);
            const lv_color_t color = lv_color_hex(presentation.color);
            lv_label_set_text(emoji_label_, presentation.icon);
            lv_obj_set_style_text_color(emoji_label_, color, 0);
            lv_label_set_text(hint_label_, presentation.hint);
            lv_obj_set_style_text_color(hint_label_, color, 0);
            // The receipt banner also heads the reply text that follows.
            reply_banner_title_ = title;
            reply_banner_color_ = presentation.color;
            ApplyChromeLocked(state, presentation);
            SetCrestResultLocked(crest_caption, static_cast<uint32_t>(duration_ms));
            SetReplyLayoutLocked(true);
        }
        if (!ScheduleVisualReset(duration_ms)) {
            receipt_visible_.store(false);
            ApplyRestingVisualState();
        }
    }

public:
    void ShowNotification(const std::string& notification, int duration_ms = 3000) override {
        ShowNotification(notification.c_str(), duration_ms);
    }

    void SetTimerText(const std::string& text) override {
        DisplayLockGuard lock(this);
        if (crest_timer_text_ == nullptr || crest_timer_text_value_ == text)
            return;
        crest_timer_text_value_ = text;
        lv_label_set_text(crest_timer_text_, crest_timer_text_value_.c_str());
    }

    void SetDictationScreen(bool visible, const std::string& status,
                            const std::string& action) override {
        DisplayLockGuard lock(this);
        if (dictation_panel_ == nullptr)
            return;
        dictation_visible_ = visible;
        SetVisible(dictation_panel_, visible);
        if (dictation_status_text_ != status) {
            dictation_status_text_ = status;
            lv_label_set_text(dictation_status_, status.c_str());
        }
        if (dictation_action_text_ != action) {
            dictation_action_text_ = action;
            const std::string label = "Blue: " + action;
            lv_label_set_text(dictation_action_, label.c_str());
        }
        SetReplyLayoutLocked(false);
    }

    void ClearChatMessages() override {}

    void SetPreviewImage(std::unique_ptr<LvglImage> image) override { (void)image; }

    bool AddTextGlyphs(const std::vector<TextGlyph>& glyphs, uint8_t bpp) override {
        (void)glyphs;
        (void)bpp;
        return false;
    }
#endif
};

class StopwatchBacklight : public Backlight {
public:
    StopwatchBacklight(esp_lcd_panel_io_handle_t panel_io, Display* display)
        : panel_io_(panel_io), display_(display) {}

protected:
    void SetBrightnessImpl(uint8_t brightness) override {
        DisplayLockGuard lock(display_);
        uint8_t data[] = {static_cast<uint8_t>((255 * brightness) / 100)};
        int lcd_cmd = 0x51;
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;

        esp_err_t err = esp_lcd_panel_io_tx_param(panel_io_, lcd_cmd, data, sizeof(data));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set display brightness: %s", esp_err_to_name(err));
        }
    }

private:
    esp_lcd_panel_io_handle_t panel_io_;
    Display* display_;
};

class M5StackStopwatchBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    M5PM1 pmic_;
    M5IOE1 ioe_;
    Button button1_;
    Button button2_;
    RoundLcdDisplay* display_;
    StopwatchBacklight* backlight_;
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    esp_timer_handle_t display_idle_timer_ = nullptr;
    esp_timer_handle_t capture_haptic_timer_ = nullptr;
    std::atomic<int64_t> display_idle_deadline_us_{0};
    provisions::LocalCapturePulse capture_haptic_pulse_;
    bool display_dimmed_ = false;
#if CONFIG_PROVISIONS_SCHEDULE_HARDWARE_BENCH
    std::unique_ptr<orbit::service_schedule::HardwareBench> bench_;
    std::atomic<orbit::service_schedule::HardwareBench*> bench_published_{nullptr};
    std::shared_ptr<const orbit::service_schedule::WorkerPublication> bench_rendered_;
    esp_timer_handle_t bench_timer_ = nullptr;
    std::atomic<bool> bench_tick_queued_{false}, bench_gesture_queued_{false},
        bench_gesture_rejected_{false};
    bool bench_motor_fault_ = false;
    uint64_t bench_display_sequence_ = 0;
    std::string bench_display_status_;

    void PollHardwareBench() {
        if (!bench_)
            return;
        if (bench_gesture_rejected_.exchange(false))
            bench_->RejectGesture();
        bench_->Poll(esp_timer_get_time() / 1000);
        const auto& publication = bench_->current();
        const auto status = bench_motor_fault_ ? std::string("HAPTIC FAULT") : bench_->Status();
        const auto sequence = publication ? publication->sequence : 0;
        if (sequence != bench_display_sequence_ || status != bench_display_status_) {
            bench_display_sequence_ = sequence;
            bench_display_status_ = status;
            display_->RenderHardwareBench(publication, status);
            // A physical press binds only after the new state is rendered, not
            // in the gap between worker publication and the display mutation.
            std::atomic_store(&bench_rendered_, publication);
        }
    }
    void QueueBenchGesture(bool blue) {
        auto* bench = bench_published_.load();
        if (!bench)
            return;
        if (bench_gesture_queued_.exchange(true)) {
            bench_gesture_rejected_.store(true);
            return;
        }
        auto observed = std::atomic_load(&bench_rendered_);
        Application::GetInstance().Schedule([this, bench, observed = std::move(observed), blue]() {
            bench_gesture_queued_.store(false);
            if (bench_published_.load() != bench)
                return;
            if (blue)
                bench->Blue(observed);
            else
                bench->Yellow(observed, esp_timer_get_time() / 1000);
            PollHardwareBench();
        });
    }
    void StartHardwareBench() {
        auto prior = esp_pthread_get_default_config();
        esp_pthread_get_cfg(&prior);
        auto config = esp_pthread_get_default_config();
        config.stack_size = 40 * 1024;
        config.prio = 2;
        config.thread_name = "orbit_schedule";
        config.inherit_cfg = false;
        if (esp_pthread_set_cfg(&config) != ESP_OK) {
            display_->RenderHardwareBench({}, "WORKER CONFIG FAILED");
            return;
        }
        auto& audio = Application::GetInstance().GetAudioService();
        orbit::service_schedule::AlarmOutputHooks hooks{
            [&audio]() { return audio.PlayLocalFeedback(Lang::Sounds::OGG_EXCLAMATION); },
            [&audio]() { audio.CancelLocalFeedback(); },
            [&audio]() { return audio.IsPlaybackIdle(); },
            [&audio]() { return audio.LocalFeedbackErrors(); },
            [this](bool active) {
                // After any I2C fault, later pulse requests may only attempt
                // LOW. A failed haptic channel must not keep reasserting HIGH.
                active = active && !bench_motor_fault_;
                m5ioe1_err_t error = M5IOE1_OK;
                ioe_.digitalWriteWithRes(IOE_PIN_MOTOR, active ? HIGH : LOW, &error);
                if (error != M5IOE1_OK)
                    bench_motor_fault_ = true;
            }};
        bench_ = std::make_unique<orbit::service_schedule::HardwareBench>(
            std::make_unique<BenchScheduleStore>(), std::move(hooks));
        esp_pthread_set_cfg(&prior);
        bench_published_.store(bench_.get());
        esp_timer_create_args_t args = {.callback =
                                            [](void* raw) {
                                                auto* self =
                                                    static_cast<M5StackStopwatchBoard*>(raw);
                                                if (self->bench_tick_queued_.exchange(true))
                                                    return;
                                                Application::GetInstance().Schedule([self]() {
                                                    self->bench_tick_queued_.store(false);
                                                    self->PollHardwareBench();
                                                });
                                            },
                                        .arg = this,
                                        .dispatch_method = ESP_TIMER_TASK,
                                        .name = "orbit_bench",
                                        .skip_unhandled_events = true};
        ESP_ERROR_CHECK(esp_timer_create(&args, &bench_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(bench_timer_, 50000));
        PollHardwareBench();
    }
#endif
#endif

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {.enable_internal_pullup = 1},
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        if (ioe_.begin(i2c_bus_, M5IOE1_I2C_ADDR, M5IOE1_I2C_FREQ_100K, M5IOE1_INT_MODE_POLLING) != M5IOE1_OK) {
            ESP_LOGE(TAG, "M5IOE1 begin failed");
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
#endif
            return;
        }

        if (pmic_.begin(i2c_bus_, M5PM1_DEFAULT_ADDR, M5PM1_I2C_FREQ_100K) != M5PM1_OK) {
            ESP_LOGE(TAG, "M5PM1 begin failed");
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
#endif
            return;
        }

        pmic_.setChargeEnable(true);
        pmic_.setBoostEnable(true);
        pmic_.pinMode(PMIC_PIN_CHARGE_STATE, INPUT);
        pmic_.pinMode(PMIC_PIN_CHARGE_PROG, OUTPUT);
        pmic_.digitalWrite(PMIC_PIN_CHARGE_PROG, LOW); // Set charge current to 425mA

        ioe_.pinMode(IOE_PIN_LCD_POWER, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_LCD_POWER, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_LCD_POWER, HIGH);

        ioe_.pinMode(IOE_PIN_LCD_RST, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_LCD_RST, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_LCD_RST, HIGH);

        ioe_.pinMode(IOE_PIN_CODEC_POWER, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_CODEC_POWER, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_CODEC_POWER, HIGH);

        ioe_.pinMode(IOE_PIN_PA_EN, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_PA_EN, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_PA_EN, HIGH);

        ioe_.pinMode(IOE_PIN_MOTOR, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_MOTOR, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_MOTOR, LOW);

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = DISPLAY_QSPI_SCK;
        buscfg.data0_io_num = DISPLAY_QSPI_D0;
        buscfg.data1_io_num = DISPLAY_QSPI_D1;
        buscfg.data2_io_num = DISPLAY_QSPI_D2;
        buscfg.data3_io_num = DISPLAY_QSPI_D3;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_QSPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeDisplay() {
        ioe_.digitalWrite(IOE_PIN_LCD_RST, LOW);
        vTaskDelay(pdMS_TO_TICKS(10));
        ioe_.digitalWrite(IOE_PIN_LCD_RST, HIGH);
        vTaskDelay(pdMS_TO_TICKS(120));

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_QSPI_CS;
        io_config.dc_gpio_num = GPIO_NUM_NC;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 32;
        io_config.lcd_param_bits = 8;
        io_config.flags.quad_mode = true;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_QSPI_HOST, &io_config, &panel_io));

        co5300_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(co5300_lcd_init_cmd_t),
            .flags = {.use_qspi_interface = 1},
        };

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = &vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(panel_io, &panel_config, &panel));

        esp_lcd_panel_set_gap(panel, 0x06, 0);
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);

        display_ = new RoundLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                       DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                       DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        backlight_ = new StopwatchBacklight(panel_io, display_);
        backlight_->RestoreBrightness();
    }

    void InitializeButtons() {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
#if CONFIG_PROVISIONS_SCHEDULE_HARDWARE_BENCH
        button1_.OnClick([this]() { QueueBenchGesture(false); });
        button2_.OnClick([this]() { QueueBenchGesture(true); });
#elif CONFIG_PROVISIONS_SCHEDULE_BENCH_DEMO
        // Physical callbacks only enqueue onto the same Application owner as ticks.
        // No Talk/microphone, volume mutation or business confirmation in this demo.
        button1_.OnClick([this]() {
            Application::GetInstance().Schedule([this]() { display_->AdvanceScheduleDemo(); });
        });
        button2_.OnClick([this]() {
            Application::GetInstance().Schedule([this]() { display_->SilenceTimerAlarm(); });
        });
#else
        button1_.OnPressDown([this]() {
            ResetDisplayIdleTimer();
            Application::GetInstance().StartListening();
        });
        button1_.OnPressUp([]() { Application::GetInstance().StopListening(); });

#if CONFIG_PROVISIONS_LOCAL_CAPTURE
        // Retain capture controls when the timer surface owns the display. Check
        // alarm and dictation state on the application task, when the action runs.
        button2_.OnDoubleClick([this]() {
            ResetDisplayIdleTimer();
            Application::GetInstance().Schedule([this]() {
                if (display_->SilenceTimerAlarm())
                    return;
                Application::GetInstance().ToggleDictationScreen();
            });
        });
        button2_.OnLongPress([this]() {
            ResetDisplayIdleTimer();
            Application::GetInstance().Schedule([this]() {
                if (display_->SilenceTimerAlarm())
                    return;
                auto& app = Application::GetInstance();
                if (!app.IsDictationScreen())
                    app.RetrySavedVoiceRecording();
            });
        });
#endif

        // Keep the second button useful without adding a menu or allowing an
        // accidental mute. It toggles only between the pilot floor and max.
        button2_.OnClick([this]() {
            ResetDisplayIdleTimer();
            Application::GetInstance().Schedule([this]() {
                if (display_->SilenceTimerAlarm()) {
                    return;
                }
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
                if (Application::GetInstance().IsDictationScreen()) {
                    Application::GetInstance().DictationButton();
                    return;
                }
#endif
                auto* codec = GetAudioCodec();
                const bool maximum = codec->output_volume() >= kMaximumOutputVolume;
                codec->SetOutputVolume(maximum ? kDefaultOutputVolume : kMaximumOutputVolume);
            });
        });
#endif
#else
        // Button1: wake / toggle conversation
        button1_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting &&
                !WifiManager::GetInstance().IsConnected()) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        // Button2: volume 0 -> 10 -> ... -> 100 -> 0
        button2_.OnClick([this]() {
            auto* codec = GetAudioCodec();
            int volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(std::string(Lang::Strings::VOLUME) + ":" + std::to_string(volume) + "%");
        });
#endif
    }

#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    void InitializeDisplayIdleTimer() {
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                auto self = static_cast<M5StackStopwatchBoard*>(arg);
                const int64_t deadline = self->display_idle_deadline_us_.load();
                if (deadline <= 0 || esp_timer_get_time() < deadline) {
                    return;
                }
                Application::GetInstance().Schedule([self, deadline]() {
                    if (self->display_idle_deadline_us_.load() != deadline ||
                        esp_timer_get_time() < deadline) {
                        return;
                    }
                    if (self->display_->HasTimerAlarm()) {
                        self->ResetDisplayIdleTimer();
                        return;
                    }
                    self->display_dimmed_ = true;
                    self->GetDisplay()->SetPowerSaveMode(true);
                    self->GetBacklight()->SetBrightness(5);
                });
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "stopwatch_display_idle",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &display_idle_timer_));
        display_idle_deadline_us_.store(
            esp_timer_get_time() + kDisplayIdleTimeoutUs);
        ESP_ERROR_CHECK(
            esp_timer_start_once(display_idle_timer_, kDisplayIdleTimeoutUs));
    }

    void InitializeCaptureHapticTimer() {
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                auto* self = static_cast<M5StackStopwatchBoard*>(arg);
                const int64_t deadline = self->capture_haptic_pulse_.deadline();
                Application::GetInstance().Schedule([self, deadline]() {
                    if (!self->capture_haptic_pulse_.IsExpired(deadline,
                                                               esp_timer_get_time())) {
                        return;
                    }
                    self->capture_haptic_pulse_.Clear(deadline);
                    if (self->display_->HasTimerAlarm()) {
                        return;
                    }
                    m5ioe1_err_t motor_error = M5IOE1_OK;
                    self->ioe_.digitalWriteWithRes(IOE_PIN_MOTOR, LOW, &motor_error);
                    if (motor_error != M5IOE1_OK) {
                        ESP_LOGE(TAG, "Capture haptic motor stop failed");
                    }
                });
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "stopwatch_capture_haptic",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &capture_haptic_timer_));
    }

    void ResetDisplayIdleTimer() {
        if (display_idle_timer_ == nullptr) {
            return;
        }
        const int64_t deadline = esp_timer_get_time() + kDisplayIdleTimeoutUs;
        display_idle_deadline_us_.store(deadline);
        Application::GetInstance().Schedule([this, deadline]() {
            if (display_idle_deadline_us_.load() != deadline) {
                return;
            }
            const esp_err_t stop_result = esp_timer_stop(display_idle_timer_);
            if (stop_result != ESP_OK && stop_result != ESP_ERR_INVALID_STATE) {
                ESP_LOGE(TAG, "Display idle timer stop failed: %s",
                         esp_err_to_name(stop_result));
                return;
            }
            const esp_err_t start_result =
                esp_timer_start_once(display_idle_timer_, kDisplayIdleTimeoutUs);
            if (start_result != ESP_OK) {
                ESP_LOGE(TAG, "Display idle timer restart failed: %s",
                         esp_err_to_name(start_result));
                return;
            }
            if (!display_dimmed_) {
                return;
            }
            display_dimmed_ = false;
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
    }
#endif

public:
    M5StackStopwatchBoard()
        : i2c_bus_(nullptr),
          button1_(BUTTON1_GPIO),
          button2_(BUTTON2_GPIO),
          display_(nullptr),
          backlight_(nullptr) {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        ESP_LOGI(TAG, "%s hardware_profile=%s nominal_battery=%dmAh talk_gpio=%d",
                 kSignedHardwareIdentity, PROVISIONS_HARDWARE_PROFILE,
                 PROVISIONS_NOMINAL_BATTERY_MAH, BUTTON1_GPIO);
#endif
        InitializeI2c();
        InitializeSpi();
        InitializeDisplay();
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
#if !CONFIG_PROVISIONS_SCHEDULE_BENCH_DEMO && !CONFIG_PROVISIONS_SCHEDULE_HARDWARE_BENCH
        InitializeDisplayIdleTimer();
        InitializeCaptureHapticTimer();
#endif
#if !CONFIG_PROVISIONS_SCHEDULE_HARDWARE_BENCH
        display_->SetTimerAlarmOutputCallback([this](bool active) {
            ioe_.digitalWrite(IOE_PIN_MOTOR, active ? HIGH : LOW);
            if (active) {
                ResetDisplayIdleTimer();
            }
        });
#endif
#if !CONFIG_PROVISIONS_SCHEDULE_BENCH_DEMO && !CONFIG_PROVISIONS_SCHEDULE_HARDWARE_BENCH
        display_->SetTimerDismissCallback([]() { Application::GetInstance().DismissDueTimers(); });
        Application::GetInstance().RegisterProvisionsTimerSnapshotCallback(
            [this](const ProvisionsTimerSnapshot::Update& update) {
                if (update.kind == ProvisionsTimerSnapshot::Update::Kind::kReset) {
                    display_->ResetTimerSnapshot();
                } else {
                    display_->ApplyTimerSnapshot(update.snapshot);
                }
            });
#endif
#endif
        InitializeButtons();
    }

    AudioCodec* GetAudioCodec() override {
#if CONFIG_PROVISIONS_SCHEDULE_BENCH_DEMO
        static DummyAudioCodec audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE);
        return &audio_codec;
#else
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        static ProvisionsStopwatchAudioCodec audio_codec(
#else
        static Es8311AudioCodec audio_codec(
#endif
            i2c_bus_,
            I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_GPIO_PA,
            AUDIO_CODEC_ES8311_ADDR,
            false);
        return &audio_codec;
#endif
    }

    Display* GetDisplay() override {
        return display_;
    }

#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    void PulseLocalCaptureHaptic(uint32_t duration_ms) override {
        if (capture_haptic_timer_ == nullptr || duration_ms == 0 || display_->HasTimerAlarm()) {
            return;
        }
        const esp_err_t stop_result = esp_timer_stop(capture_haptic_timer_);
        if (stop_result != ESP_OK && stop_result != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Capture haptic timer stop failed: %s", esp_err_to_name(stop_result));
            return;
        }
        const int64_t duration_us = static_cast<int64_t>(duration_ms) * 1000;
        capture_haptic_pulse_.Arm(esp_timer_get_time(), duration_ms);
        m5ioe1_err_t motor_error = M5IOE1_OK;
        ioe_.digitalWriteWithRes(IOE_PIN_MOTOR, HIGH, &motor_error);
        if (motor_error != M5IOE1_OK) {
            capture_haptic_pulse_.Cancel();
            ioe_.digitalWriteWithRes(IOE_PIN_MOTOR, LOW, &motor_error);
            ESP_LOGE(TAG, "Capture haptic motor start failed");
            return;
        }
        const esp_err_t start_result = esp_timer_start_once(capture_haptic_timer_, duration_us);
        if (start_result != ESP_OK) {
            capture_haptic_pulse_.Cancel();
            ioe_.digitalWriteWithRes(IOE_PIN_MOTOR, LOW, &motor_error);
            ESP_LOGE(TAG, "Capture haptic timer start failed: %s", esp_err_to_name(start_result));
            return;
        }
        ResetDisplayIdleTimer();
    }
#endif

#if CONFIG_PROVISIONS_SCHEDULE_HARDWARE_BENCH
    void StartNetwork() override {
        ESP_LOGI(TAG,
                 "Isolated schedule BENCH: real speaker, no network/capture; orbit_bench_v1 only");
        StartHardwareBench();
    }
#elif CONFIG_PROVISIONS_SCHEDULE_BENCH_DEMO
    void StartNetwork() override {
        // No Wi-Fi, bootstrap, OTA, credential use or gateway connection. The
        // Application event loop still services the two local fixture buttons.
        ESP_LOGI(TAG, "Synthetic schedule demo: network and microphone disabled");
    }
#endif

    Backlight* GetBacklight() override {
        return backlight_;
    }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        uint16_t voltage_mv = 0;
        if (pmic_.readVbat(&voltage_mv) != M5PM1_OK) {
            return false;
        }

        int charge_state_level = pmic_.digitalRead(PMIC_PIN_CHARGE_STATE);
        if (charge_state_level < 0) {
            // Charge status read failed; leave charging/discharging unknown
            charging = false;
            discharging = false;
        } else {
            // M5PM1 charge status is active low: 0 means charging, 1 means not charging
            charging = (charge_state_level == 0);
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
            m5pm1_pwr_src_t power_source = M5PM1_PWR_SRC_UNKNOWN;
            if (pmic_.getPowerSource(&power_source) == M5PM1_OK) {
                discharging = (power_source == M5PM1_PWR_SRC_BAT);
            } else {
                discharging = false;
            }
#else
            discharging = !charging;
#endif
        }

        const int BATTERY_MIN_VOLTAGE = 3400;
        const int BATTERY_MAX_VOLTAGE = 4200;
        if (voltage_mv < BATTERY_MIN_VOLTAGE) {
            level = 0;
        } else if (voltage_mv > BATTERY_MAX_VOLTAGE) {
            level = 100;
        } else {
            level = ((voltage_mv - BATTERY_MIN_VOLTAGE) * 100) / (BATTERY_MAX_VOLTAGE - BATTERY_MIN_VOLTAGE);
        }
        return true;
    }

#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
    void SetPowerSaveLevel(PowerSaveLevel level) override {
        ResetDisplayIdleTimer();
        WifiBoard::SetPowerSaveLevel(level);
    }
#endif
};

DECLARE_BOARD(M5StackStopwatchBoard);
