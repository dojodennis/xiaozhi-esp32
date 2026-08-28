#pragma once

#include <cstddef>
#include <string>

namespace ProvisionsStopWatch {

inline std::string EllipsizeUtf8(const std::string& text, std::size_t max_bytes) {
    constexpr std::size_t kEllipsisBytes = 3;

    if (text.size() <= max_bytes) {
        return text;
    }
    if (max_bytes <= kEllipsisBytes) {
        return std::string(max_bytes, '.');
    }

    std::size_t prefix_bytes = max_bytes - kEllipsisBytes;
    while (prefix_bytes > 0 &&
           (static_cast<unsigned char>(text[prefix_bytes]) & 0xC0U) == 0x80U) {
        --prefix_bytes;
    }

    return text.substr(0, prefix_bytes) + "...";
}

}  // namespace ProvisionsStopWatch
