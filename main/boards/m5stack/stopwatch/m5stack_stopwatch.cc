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
#include "assets/lang_config.h"
#include <atomic>
#include <cstring>
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

constexpr uint32_t kColorGold = 0xD4B67A;
constexpr uint32_t kColorCream = 0xF5F2EB;
constexpr uint32_t kColorGreen = 0x7FBF8F;
constexpr uint32_t kColorBlue = 0x9FB8D8;
constexpr uint32_t kColorAmber = 0xE0A256;
constexpr uint32_t kColorRed = 0xE07566;
constexpr uint32_t kColorTalkButton = 0xF2C84B;

class ProvisionsStopwatchAudioCodec final : public Es8311AudioCodec {
public:
    using Es8311AudioCodec::Es8311AudioCodec;

    void Start() override {
        Es8311AudioCodec::Start();
        if (output_volume() < kDefaultOutputVolume || output_volume() > 100) {
            // Apply the pilot floor after Start() reloads the saved preference.
            // This setter updates codec state only; it never writes NVS.
            SetOutputVolumeForSession(kDefaultOutputVolume);
        }
    }
};

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
    esp_timer_handle_t visual_reset_timer_ = nullptr;
    esp_timer_handle_t reply_scroll_timer_ = nullptr;
    std::atomic<int64_t> visual_reset_deadline_us_{0};
    std::atomic<VisualState> resting_state_{VisualState::kBoot};
    std::atomic<bool> receipt_visible_{false};
    std::atomic<bool> reply_visible_{false};
    std::atomic<bool> power_save_active_{false};
    std::atomic<uint32_t> reply_generation_{0};

    static bool IsClockStatus(const char* status) {
        return status != nullptr && std::strlen(status) == 5 && status[2] == ':' &&
               status[0] >= '0' && status[0] <= '9' && status[1] >= '0' && status[1] <= '9' &&
               status[3] >= '0' && status[3] <= '9' && status[4] >= '0' && status[4] <= '9';
    }

    static StatePresentation PresentationFor(VisualState state) {
        switch (state) {
            case VisualState::kBoot:
                return {"Starting", "Preparing your helper", MATERIAL_SYMBOLS_PROGRESS_ACTIVITY,
                        kColorGold};
            case VisualState::kConnecting:
                return {"Connecting", "Joining Provisions securely", MATERIAL_SYMBOLS_WIFI,
                        kColorBlue};
            case VisualState::kReady:
                return {"Ready", "Hold yellow button to talk", MATERIAL_SYMBOLS_MIC, kColorGreen};
            case VisualState::kListening:
                return {"Listening", "Release when finished", MATERIAL_SYMBOLS_MIC, kColorGold};
            case VisualState::kWorking:
                return {"Working", "Checking your Provisions", MATERIAL_SYMBOLS_PROGRESS_ACTIVITY,
                        kColorBlue};
            case VisualState::kSpeaking:
                return {"Replying", "Listen for your answer", MATERIAL_SYMBOLS_VOLUME_UP,
                        kColorBlue};
            case VisualState::kUnavailable:
                return {"Unavailable", "Please try again", MATERIAL_SYMBOLS_CLOUD_OFF, kColorRed};
            case VisualState::kAdded:
                return {"Added to draft", "Draft only - not sent", MATERIAL_SYMBOLS_CHECK_CIRCLE,
                        kColorGreen};
            case VisualState::kSuccess:
                return {"Done", "Listen for the result", MATERIAL_SYMBOLS_CHECK_CIRCLE,
                        kColorGreen};
            case VisualState::kDraft:
                return {"Draft only", "Unsent - not submitted", MATERIAL_SYMBOLS_INFO, kColorAmber};
            case VisualState::kRecorded:
                return {"Recorded", "From Provisions records", MATERIAL_SYMBOLS_INFO, kColorAmber};
            case VisualState::kQuestion:
                return {"One question", "Listen and answer", MATERIAL_SYMBOLS_HELP, kColorGold};
            case VisualState::kWarning:
                return {"Not changed", "No change made", MATERIAL_SYMBOLS_WARNING, kColorAmber};
            case VisualState::kNotice:
                return {"Notice", "Listen for details", MATERIAL_SYMBOLS_INFO, kColorAmber};
        }
        return {"Unavailable", "Please try again", MATERIAL_SYMBOLS_CLOUD_OFF, kColorRed};
    }

    void SetReplyLayoutLocked(bool visible) {
        const bool display_awake = !power_save_active_.load();
        lv_obj_t* normal[] = {
            title_label_, brand_rule_, hero_halo_, status_bar_, hint_panel_
        };
        for (auto* object : normal) {
            if (object == nullptr) {
                continue;
            }
            if (display_awake && !visible) {
                lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
            }
        }

        lv_obj_t* reply[] = {reply_header_label_, reply_panel_};
        for (auto* object : reply) {
            if (object == nullptr) {
                continue;
            }
            if (display_awake && visible) {
                lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
            }
        }
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
        if (std::strcmp(status, "Ready") == 0) {
            return VisualState::kReady;
        }
        if (std::strcmp(status, "Working") == 0) {
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
        if (std::strcmp(status, "Unavailable") == 0 ||
            std::strcmp(status, Lang::Strings::ERROR) == 0 ||
            std::strcmp(status, Lang::Strings::SERVER_ERROR) == 0 ||
            std::strcmp(status, Lang::Strings::SERVER_NOT_CONNECTED) == 0 ||
            std::strcmp(status, Lang::Strings::SERVER_TIMEOUT) == 0) {
            return VisualState::kUnavailable;
        }
        return VisualState::kBoot;
    }

    static VisualState StateForNotification(const char* notification, const char** title) {
        if (notification == nullptr) {
            *title = "Notice";
            return VisualState::kNotice;
        }
        if (std::strcmp(notification, "Added") == 0) {
            *title = "Added to draft";
            return VisualState::kAdded;
        }
        if (std::strcmp(notification, "Undone") == 0) {
            *title = "Removed from draft";
            return VisualState::kAdded;
        }
        if (std::strcmp(notification, "Found") == 0 ||
            std::strcmp(notification, "Delivered") == 0 ||
            std::strcmp(notification, "On the way") == 0) {
            *title = notification;
            return VisualState::kSuccess;
        }
        if (std::strcmp(notification, "Draft only") == 0) {
            *title = notification;
            return VisualState::kDraft;
        }
        if (std::strcmp(notification, "Recorded") == 0) {
            *title = notification;
            return VisualState::kRecorded;
        }
        if (std::strcmp(notification, "Choose one") == 0 ||
            std::strcmp(notification, "Need unit") == 0 ||
            std::strcmp(notification, "Ready to add") == 0) {
            *title = notification;
            return VisualState::kQuestion;
        }
        if (std::strcmp(notification, "No match") == 0 ||
            std::strcmp(notification, "Cancelled") == 0 ||
            std::strcmp(notification, "Check app") == 0 ||
            std::strcmp(notification, "Not changed") == 0) {
            *title = notification;
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
#endif
    }

    ~RoundLcdDisplay() override {
#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
        if (visual_reset_timer_ != nullptr) {
            esp_timer_stop(visual_reset_timer_);
            esp_timer_delete(visual_reset_timer_);
        }
        if (reply_scroll_timer_ != nullptr) {
            esp_timer_stop(reply_scroll_timer_);
            esp_timer_delete(reply_scroll_timer_);
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
        lv_obj_set_style_text_font(reply_header_label_, &font_noto_sans_basic_16_4, 0);
        lv_obj_set_style_text_color(reply_header_label_, lv_color_hex(kColorGold), 0);
        lv_obj_set_style_text_letter_space(reply_header_label_, 3, 0);
        lv_obj_set_style_text_align(reply_header_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(reply_header_label_, LV_LABEL_LONG_CLIP);
        lv_label_set_text(reply_header_label_, "REPLY");
        lv_obj_align(reply_header_label_, LV_ALIGN_TOP_MID, 0, kReplyHeaderTopOffset);
        lv_obj_add_flag(reply_header_label_, LV_OBJ_FLAG_HIDDEN);

        reply_panel_ = lv_obj_create(screen);
        lv_obj_set_size(reply_panel_, kReplyPanelWidth, kReplyPanelHeight);
        lv_obj_set_style_radius(reply_panel_, 32, 0);
        lv_obj_set_style_pad_all(reply_panel_, 20, 0);
        // Opaque near-black ground: the text carries the screen; no tinted
        // wash competing with it, only a faint gold hairline for edge
        // definition against the true-black bezel.
        lv_obj_set_style_bg_color(reply_panel_, lv_color_hex(0x0E0D0B), 0);
        lv_obj_set_style_bg_opa(reply_panel_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(reply_panel_, lv_color_hex(kColorGold), 0);
        lv_obj_set_style_border_width(reply_panel_, 1, 0);
        lv_obj_set_style_border_opa(reply_panel_, LV_OPA_30, 0);
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

        hide_subtitle_ = true;
        if (bottom_bar_ != nullptr) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        ApplyVisualStateLocked(VisualState::kBoot);
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
            if (reply_header_label_ == nullptr || reply_panel_ == nullptr ||
                reply_label_ == nullptr) {
                receipt_visible_.store(false);
                reply_visible_.store(false);
                return;
            }
            reply_generation_.fetch_add(1);
            // LVGL copies the string. No transcript text is retained in board
            // state, logs, preferences, or flash.
            lv_label_set_text(reply_label_, content);
            lv_obj_scroll_to_y(reply_panel_, 0, LV_ANIM_OFF);
            SetReplyLayoutLocked(true);
        }
        StartReplyScroll();
        // The gateway caps audio at 30 seconds. This safety timeout covers the
        // whole playback if a terminal frame is lost; normal completion resets
        // the timer to the shorter post-speech reading window in SetStatus().
        if (!ScheduleVisualReset(kReplyPlaybackMaximumMs)) {
            receipt_visible_.store(false);
            ApplyRestingVisualState();
        }
    }

    void SetPowerSaveMode(bool on) override {
        power_save_active_.store(on);
        DisplayLockGuard lock(this);
        lv_obj_t* chrome[] = {
            top_bar_, brand_label_, title_label_, brand_rule_, hero_halo_,
            status_bar_, hint_panel_, reply_header_label_, reply_panel_
        };
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
            ApplyChromeLocked(state, presentation);
        }
        if (!ScheduleVisualReset(duration_ms)) {
            receipt_visible_.store(false);
            ApplyRestingVisualState();
        }
    }

    void ShowNotification(const std::string& notification, int duration_ms = 3000) override {
        ShowNotification(notification.c_str(), duration_ms);
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
    std::atomic<int64_t> display_idle_deadline_us_{0};
    bool display_dimmed_ = false;
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
        button1_.OnPressDown([this]() {
            ResetDisplayIdleTimer();
            Application::GetInstance().StartListening();
        });
        button1_.OnPressUp([]() { Application::GetInstance().StopListening(); });

        // Keep the second button useful without adding a menu or allowing an
        // accidental mute. It toggles only between the pilot floor and max.
        button2_.OnClick([this]() {
            ResetDisplayIdleTimer();
            Application::GetInstance().Schedule([this]() {
                auto* codec = GetAudioCodec();
                const bool maximum = codec->output_volume() >= kMaximumOutputVolume;
                codec->SetOutputVolume(maximum ? kDefaultOutputVolume : kMaximumOutputVolume);
            });
        });
#else
        // Button1: wake / toggle conversation
        button1_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiManager::GetInstance().IsConnected()) {
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
        InitializeDisplayIdleTimer();
#endif
        InitializeButtons();
    }

    AudioCodec* GetAudioCodec() override {
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
    }

    Display* GetDisplay() override {
        return display_;
    }

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
