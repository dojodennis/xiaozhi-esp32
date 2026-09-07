#ifndef PROVISIONS_VOICE_FEEDBACK_H
#define PROVISIONS_VOICE_FEEDBACK_H

#include <string_view>

namespace provisions::feedback {
extern const char saved_start[] asm("_binary_saved_on_orbit_ogg_start");
extern const char saved_end[] asm("_binary_saved_on_orbit_ogg_end");
extern const char failed_start[] asm("_binary_could_not_save_ogg_start");
extern const char failed_end[] asm("_binary_could_not_save_ogg_end");
inline const std::string_view kSaved{saved_start, static_cast<size_t>(saved_end - saved_start)};
inline const std::string_view kFailed{failed_start, static_cast<size_t>(failed_end - failed_start)};
}  // namespace provisions::feedback
#endif
