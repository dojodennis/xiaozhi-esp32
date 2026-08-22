#ifndef _KITCHEN_HELPER_DISPLAY_H_
#define _KITCHEN_HELPER_DISPLAY_H_

#include "display/lcd_display.h"

class KitchenHelperDisplay : public SpiLcdDisplay {
public:
    using SpiLcdDisplay::SpiLcdDisplay;

    void SetupUI() override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    bool AddTextGlyphs(const std::vector<TextGlyph>& glyphs, uint8_t bpp) override;
};

#endif  // _KITCHEN_HELPER_DISPLAY_H_
