#include "kitchen_helper_display.h"

#include <cstring>
#include <string>

namespace {
constexpr size_t kMaximumResultCharacters = 48;
}

void KitchenHelperDisplay::SetupUI() {
    SpiLcdDisplay::SetupUI();
    SetHideSubtitle(true);
    SetStatus("Boot");
}

void KitchenHelperDisplay::SetEmotion(const char* emotion) { (void)emotion; }

void KitchenHelperDisplay::SetChatMessage(const char* role, const char* content) {
    if (role == nullptr || content == nullptr || content[0] == '\0' ||
        std::strcmp(role, "assistant") != 0) {
        return;
    }

    std::string result(content);
    if (result.size() > kMaximumResultCharacters) {
        result.resize(kMaximumResultCharacters - 3);
        result.append("...");
    }
    ShowNotification(result, 3000);
}

void KitchenHelperDisplay::ClearChatMessages() {}

void KitchenHelperDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    (void)image;
}

bool KitchenHelperDisplay::AddTextGlyphs(const std::vector<TextGlyph>& glyphs, uint8_t bpp) {
    (void)glyphs;
    (void)bpp;
    return false;
}
