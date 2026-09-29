#pragma once
#include <cstdint>

#include "settings.h"

// Which drawing the menu (blue, then swipe) uses. Chosen per ring at runtime
// and kept in NVS, so one firmware serves the Provisions rings (crest) and the
// Nausicaä ring (film pages). Blue held on the menu flips it.
namespace ProvisionsStopWatch {
enum class MenuTheme : int32_t { Crest = 0, Nausicaa = 1 };

inline MenuTheme LoadMenuTheme() {
    Settings settings("orbit_ui", false);
    return settings.GetInt("menu_theme", 0) == 1 ? MenuTheme::Nausicaa : MenuTheme::Crest;
}

inline void SaveMenuTheme(MenuTheme theme) {
    Settings settings("orbit_ui", true);
    settings.SetInt("menu_theme", static_cast<int32_t>(theme));
}

inline MenuTheme OtherMenuTheme(MenuTheme theme) {
    return theme == MenuTheme::Crest ? MenuTheme::Nausicaa : MenuTheme::Crest;
}

inline const char* MenuThemeName(MenuTheme theme) {
    return theme == MenuTheme::Crest ? "crest" : "nausicaa";
}
}  // namespace ProvisionsStopWatch
