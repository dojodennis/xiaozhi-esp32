#ifndef _KITCHEN_HELPER_DISPLAY_H_
#define _KITCHEN_HELPER_DISPLAY_H_

#include "display/lcd_display.h"

#include <atomic>
#include <esp_timer.h>

class KitchenHelperDisplay : public SpiLcdDisplay {
public:
    using SpiLcdDisplay::SpiLcdDisplay;
    ~KitchenHelperDisplay() override;

    void SetupUI() override;
    void SetStatus(const char* status) override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    bool AddTextGlyphs(const std::vector<TextGlyph>& glyphs, uint8_t bpp) override;

private:
    esp_timer_handle_t reply_clear_timer_ = nullptr;
    std::atomic<int64_t> reply_clear_deadline_us_{0};
    std::atomic<bool> reply_visible_{false};
    std::atomic<bool> preserve_next_clear_{false};

    bool ArmReplyClear(int duration_ms);
    void ClearReplyNow();
};

#endif  // _KITCHEN_HELPER_DISPLAY_H_
