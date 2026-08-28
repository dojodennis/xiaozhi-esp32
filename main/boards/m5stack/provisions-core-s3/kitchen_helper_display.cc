#include "kitchen_helper_display.h"
#include "application.h"

#include <cstring>
#include <esp_err.h>
#include <esp_log.h>

namespace {
constexpr int kReplyPlaybackMaximumMs = 35 * 1000;
constexpr int kReplyHoldAfterSpeechMs = 12 * 1000;
constexpr char kTag[] = "KitchenHelperDisplay";

bool IsReplyClearingStatus(const char* status) {
    if (status == nullptr) {
        return true;
    }
    return std::strcmp(status, "Boot") == 0 ||
           std::strcmp(status, "Connecting") == 0 ||
           std::strcmp(status, "Listening") == 0 ||
           std::strcmp(status, "Working") == 0 ||
           std::strcmp(status, "Unavailable") == 0;
}
}

KitchenHelperDisplay::~KitchenHelperDisplay() {
    if (reply_clear_timer_ != nullptr) {
        esp_timer_stop(reply_clear_timer_);
        esp_timer_delete(reply_clear_timer_);
    }
}

void KitchenHelperDisplay::SetupUI() {
    SpiLcdDisplay::SetupUI();
    // The direct gateway reply is shown in the existing scrolling subtitle
    // surface. It is cleared explicitly instead of passing through the generic
    // notification timer, whose callback only hides plaintext label content.
    SetHideSubtitle(false);
    if (reply_clear_timer_ == nullptr) {
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                auto* self = static_cast<KitchenHelperDisplay*>(arg);
                const int64_t deadline = self->reply_clear_deadline_us_.load();
                if (deadline <= 0 || esp_timer_get_time() < deadline) {
                    return;
                }
                Application::GetInstance().Schedule([self, deadline]() {
                    if (self->reply_clear_deadline_us_.load() != deadline ||
                        esp_timer_get_time() < deadline) {
                        return;
                    }
                    self->ClearReplyNow();
                });
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "cores3_reply_clear",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &reply_clear_timer_));
    }
    SetStatus("Boot");
}

void KitchenHelperDisplay::SetStatus(const char* status) {
    if (reply_visible_.load() && status != nullptr && std::strcmp(status, "Ready") == 0) {
        preserve_next_clear_.store(true);
        if (!ArmReplyClear(kReplyHoldAfterSpeechMs)) {
            ClearReplyNow();
        }
    } else if (reply_visible_.load() && IsReplyClearingStatus(status)) {
        ClearReplyNow();
    }
    SpiLcdDisplay::SetStatus(status);
}

void KitchenHelperDisplay::SetEmotion(const char* emotion) { (void)emotion; }

void KitchenHelperDisplay::SetChatMessage(const char* role, const char* content) {
    if (role == nullptr || content == nullptr || content[0] == '\0' ||
        std::strcmp(role, "assistant") != 0) {
        return;
    }

    reply_visible_.store(true);
    preserve_next_clear_.store(false);
    {
        DisplayLockGuard lock(this);
        if (chat_message_label_ == nullptr || bottom_bar_ == nullptr) {
            reply_visible_.store(false);
            return;
        }
        // LVGL copies the validated UTF-8 string. Do not truncate by bytes and
        // do not retain reply text in board state, logs, preferences, or flash.
        lv_anim_delete(chat_message_label_, nullptr);
        lv_label_set_text(chat_message_label_, content);
        lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    if (!ArmReplyClear(kReplyPlaybackMaximumMs)) {
        ClearReplyNow();
    }
}

void KitchenHelperDisplay::ClearChatMessages() {
    if (preserve_next_clear_.exchange(false)) {
        return;
    }
    ClearReplyNow();
}

bool KitchenHelperDisplay::ArmReplyClear(int duration_ms) {
    if (reply_clear_timer_ == nullptr) {
        return false;
    }
    const int64_t deadline =
        esp_timer_get_time() + static_cast<int64_t>(duration_ms) * 1000;
    reply_clear_deadline_us_.store(deadline);
    const esp_err_t stop_result = esp_timer_stop(reply_clear_timer_);
    if (stop_result != ESP_OK && stop_result != ESP_ERR_INVALID_STATE) {
        reply_clear_deadline_us_.store(0);
        ESP_LOGE(kTag, "Failed to stop reply clear timer: %s", esp_err_to_name(stop_result));
        return false;
    }
    const esp_err_t start_result =
        esp_timer_start_once(reply_clear_timer_, static_cast<uint64_t>(duration_ms) * 1000);
    if (start_result != ESP_OK) {
        reply_clear_deadline_us_.store(0);
        ESP_LOGE(kTag, "Failed to start reply clear timer: %s", esp_err_to_name(start_result));
        return false;
    }
    return true;
}

void KitchenHelperDisplay::ClearReplyNow() {
    reply_clear_deadline_us_.store(0);
    preserve_next_clear_.store(false);
    reply_visible_.store(false);
    if (reply_clear_timer_ != nullptr) {
        const esp_err_t stop_result = esp_timer_stop(reply_clear_timer_);
        if (stop_result != ESP_OK && stop_result != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(kTag, "Failed to stop reply clear timer: %s",
                     esp_err_to_name(stop_result));
        }
    }
    DisplayLockGuard lock(this);
    if (chat_message_label_ != nullptr) {
        lv_anim_delete(chat_message_label_, nullptr);
        lv_label_set_text(chat_message_label_, "");
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}

void KitchenHelperDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    (void)image;
}

bool KitchenHelperDisplay::AddTextGlyphs(const std::vector<TextGlyph>& glyphs, uint8_t bpp) {
    (void)glyphs;
    (void)bpp;
    return false;
}
