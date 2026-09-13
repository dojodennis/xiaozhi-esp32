#ifndef ORBIT_EXHIBITION_DEMO_AUDIO_H_
#define ORBIT_EXHIBITION_DEMO_AUDIO_H_

#include <string_view>

namespace orbit::exhibition_demo {
extern const char sea_bass_start[] asm("_binary_exhibition_sea_bass_ogg_start");
extern const char sea_bass_end[] asm("_binary_exhibition_sea_bass_ogg_end");

inline const std::string_view kSeaBassReply{
    sea_bass_start, static_cast<size_t>(sea_bass_end - sea_bass_start)};
}  // namespace orbit::exhibition_demo

#endif  // ORBIT_EXHIBITION_DEMO_AUDIO_H_
