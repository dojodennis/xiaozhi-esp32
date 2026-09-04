#ifndef PROVISIONS_JSON_GUARD_H_
#define PROVISIONS_JSON_GUARD_H_

#include <cstddef>
#include <string_view>

namespace ProvisionsJsonGuard {

inline bool ContainsEmbeddedNul(std::string_view raw_json) {
    std::size_t preceding_backslashes = 0;
    for (std::size_t index = 0; index < raw_json.size(); ++index) {
        const char character = raw_json[index];
        if (character == '\0') {
            return true;
        }
        if (character == '\\') {
            ++preceding_backslashes;
            continue;
        }

        const bool is_nul_escape = character == 'u' && (preceding_backslashes % 2) == 1 &&
                                   raw_json.size() - index >= 5 &&
                                   raw_json.compare(index, 5, "u0000") == 0;
        preceding_backslashes = 0;
        if (is_nul_escape) {
            return true;
        }
    }
    return false;
}

}  // namespace ProvisionsJsonGuard

#endif  // PROVISIONS_JSON_GUARD_H_
