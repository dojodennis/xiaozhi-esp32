#ifndef PROVISIONS_TTS_TEXT_H_
#define PROVISIONS_TTS_TEXT_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ProvisionsTtsText {

constexpr std::size_t kMaximumBytes = 2000;
constexpr std::size_t kMaximumScalars = 500;

inline bool IsForbiddenCodePoint(uint32_t code_point) {
    // Mirror the gateway's key privacy-safe constraints without shipping a
    // Unicode category database: reject controls, known format controls,
    // private-use ranges, and Unicode noncharacters. Invalid scalar values are
    // rejected by the decoder below.
    if (code_point <= 0x1f || (code_point >= 0x7f && code_point <= 0x9f)) {
        return true;
    }
    if (code_point == 0x00ad || (code_point >= 0x0600 && code_point <= 0x0605) ||
        code_point == 0x061c || code_point == 0x06dd || code_point == 0x070f ||
        (code_point >= 0x0890 && code_point <= 0x0891) || code_point == 0x08e2 ||
        (code_point >= 0x17b4 && code_point <= 0x17b5) || code_point == 0x180e ||
        (code_point >= 0x200b && code_point <= 0x200f) ||
        (code_point >= 0x202a && code_point <= 0x202e) ||
        (code_point >= 0x2060 && code_point <= 0x2064) ||
        (code_point >= 0x2066 && code_point <= 0x206f) || code_point == 0xfeff ||
        (code_point >= 0xfff9 && code_point <= 0xfffb) || code_point == 0x110bd ||
        code_point == 0x110cd || (code_point >= 0x13430 && code_point <= 0x1345f) ||
        (code_point >= 0x1bca0 && code_point <= 0x1bca3) ||
        (code_point >= 0x1d173 && code_point <= 0x1d17a) || code_point == 0xe0001 ||
        (code_point >= 0xe0020 && code_point <= 0xe007f)) {
        return true;
    }
    if ((code_point >= 0xe000 && code_point <= 0xf8ff) ||
        (code_point >= 0xf0000 && code_point <= 0xffffd) ||
        (code_point >= 0x100000 && code_point <= 0x10fffd)) {
        return true;
    }
    return (code_point >= 0xfdd0 && code_point <= 0xfdef) ||
           (code_point & 0xffff) == 0xfffe || (code_point & 0xffff) == 0xffff;
}

inline bool IsValid(std::string_view text) {
    if (text.empty() || text.size() > kMaximumBytes) {
        return false;
    }

    std::size_t scalar_count = 0;
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<uint8_t>(text[index]);
        uint32_t code_point = 0;
        std::size_t sequence_length = 0;

        if (first <= 0x7f) {
            code_point = first;
            sequence_length = 1;
        } else if (first >= 0xc2 && first <= 0xdf) {
            code_point = first & 0x1f;
            sequence_length = 2;
        } else if (first >= 0xe0 && first <= 0xef) {
            code_point = first & 0x0f;
            sequence_length = 3;
        } else if (first >= 0xf0 && first <= 0xf4) {
            code_point = first & 0x07;
            sequence_length = 4;
        } else {
            return false;
        }

        if (index + sequence_length > text.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset < sequence_length; ++offset) {
            const auto continuation = static_cast<uint8_t>(text[index + offset]);
            if ((continuation & 0xc0) != 0x80) {
                return false;
            }
            code_point = (code_point << 6) | (continuation & 0x3f);
        }

        if ((sequence_length == 3 && first == 0xe0 &&
             static_cast<uint8_t>(text[index + 1]) < 0xa0) ||
            (sequence_length == 3 && first == 0xed &&
             static_cast<uint8_t>(text[index + 1]) > 0x9f) ||
            (sequence_length == 4 && first == 0xf0 &&
             static_cast<uint8_t>(text[index + 1]) < 0x90) ||
            (sequence_length == 4 && first == 0xf4 &&
             static_cast<uint8_t>(text[index + 1]) > 0x8f) ||
            code_point > 0x10ffff ||
            (code_point >= 0xd800 && code_point <= 0xdfff) ||
            IsForbiddenCodePoint(code_point)) {
            return false;
        }

        ++scalar_count;
        if (scalar_count > kMaximumScalars) {
            return false;
        }
        index += sequence_length;
    }
    return true;
}

}  // namespace ProvisionsTtsText

#endif  // PROVISIONS_TTS_TEXT_H_
