#include "provisions_endpoint_policy.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <iterator>
#include <string_view>
#include <vector>

#ifndef CONFIG_PROVISIONS_PREVIEW_HOST
#error "The Provisions preview host must be compiled into the firmware"
#endif
#ifndef CONFIG_PROVISIONS_PREVIEW_PATH_PREFIX
#error "The Provisions preview path must be compiled into the firmware"
#endif
#ifndef CONFIG_PROVISIONS_PREVIEW_WEBSOCKET_URL
#error "The Provisions preview WebSocket URL must be compiled into the firmware"
#endif
#ifndef CONFIG_OTA_URL
#error "The Provisions bootstrap URL must be compiled into the firmware"
#endif
#ifndef BOARD_NAME
#error "The Provisions firmware identity must be compiled into the firmware"
#endif

namespace {

constexpr std::string_view kExpectedHost = "app.provisions-app.com";
constexpr std::string_view kExpectedPathPrefix = "/kitchen-helper/preview/v1/";
constexpr std::string_view kExpectedBootstrapUrl =
    "https://app.provisions-app.com/kitchen-helper/preview/v1/bootstrap";
constexpr std::string_view kExpectedWebsocketUrl =
    "wss://app.provisions-app.com/kitchen-helper/preview/v1/device";
constexpr std::string_view kExpectedOtaManifestUrl =
    "https://app.provisions-app.com/kitchen-helper/preview/v1/firmware/manifest.json";
constexpr std::string_view kExpectedHealthUrl =
    "https://app.provisions-app.com/kitchen-helper/preview/v1/health";
constexpr std::string_view kExpectedFirmwareIdentity = BOARD_NAME;

#if (defined(CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_CORE_S3) + \
     defined(CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_CORE_S3_LITE) + \
     defined(CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_STOPWATCH)) != 1
#error "Select exactly one Provisions hardware profile"
#elif defined(CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_CORE_S3)
constexpr std::string_view kSelectedHardwareIdentity = "provisions-kitchen-helper-core-s3";
#elif defined(CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_CORE_S3_LITE)
constexpr std::string_view kSelectedHardwareIdentity = "provisions-kitchen-helper-core-s3-lite";
#elif defined(CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_STOPWATCH)
constexpr std::string_view kSelectedHardwareIdentity = "provisions-kitchen-helper-stopwatch";
#endif

#if (defined(CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_CORE_S3) || \
     defined(CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_CORE_S3_LITE)) && \
    (!defined(CONFIG_SPIRAM_MODE_QUAD) || defined(CONFIG_SPIRAM_MODE_OCT))
#error "Provisions CoreS3 profiles require quad PSRAM only"
#elif defined(CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_STOPWATCH) && \
    (!defined(CONFIG_SPIRAM_MODE_OCT) || defined(CONFIG_SPIRAM_MODE_QUAD))
#error "The Provisions StopWatch profile requires octal PSRAM only"
#endif

static_assert(std::string_view(CONFIG_PROVISIONS_PREVIEW_HOST) == kExpectedHost,
              "Gate 1 must use the approved Provisions preview host");
static_assert(std::string_view(CONFIG_PROVISIONS_PREVIEW_PATH_PREFIX) == kExpectedPathPrefix,
              "Gate 1 must use the approved Provisions preview path");
static_assert(std::string_view(CONFIG_PROVISIONS_PREVIEW_WEBSOCKET_URL) == kExpectedWebsocketUrl,
              "Gate 1 must use the approved Provisions device WebSocket");
static_assert(std::string_view(CONFIG_OTA_URL) == kExpectedBootstrapUrl,
              "Gate 1 must use the approved Provisions bootstrap URL");
static_assert(kExpectedFirmwareIdentity == kSelectedHardwareIdentity,
              "The Provisions hardware and OTA identities must match");

struct ParsedUrl {
    std::string scheme;
    std::string host;
    uint16_t port = 0;
    std::string path;
    bool has_query = false;
};

std::string Lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return std::tolower(character); });
    return value;
}

bool ParsePort(std::string_view text, uint16_t& port) {
    if (text.empty()) {
        return false;
    }

    uint32_t value = 0;
    for (char character : text) {
        if (!std::isdigit(static_cast<unsigned char>(character))) {
            return false;
        }
        value = value * 10 + static_cast<uint32_t>(character - '0');
        if (value > UINT16_MAX) {
            return false;
        }
    }
    if (value == 0) {
        return false;
    }
    port = static_cast<uint16_t>(value);
    return true;
}

bool HasUnsafePathSyntax(std::string_view path) {
    if (path.empty() || path.front() != '/' || path.find('\\') != std::string_view::npos ||
        path.find('%') != std::string_view::npos || path.find("//") != std::string_view::npos) {
        return true;
    }

    size_t segment_start = 1;
    while (segment_start <= path.size()) {
        size_t segment_end = path.find('/', segment_start);
        if (segment_end == std::string_view::npos) {
            segment_end = path.size();
        }
        auto segment = path.substr(segment_start, segment_end - segment_start);
        if (segment == "." || segment == "..") {
            return true;
        }
        if (segment_end == path.size()) {
            break;
        }
        segment_start = segment_end + 1;
    }
    return false;
}

bool ParseUrl(const std::string& value, ParsedUrl& parsed) {
    const size_t scheme_end = value.find("://");
    if (scheme_end == std::string::npos || scheme_end == 0) {
        return false;
    }
    parsed.scheme = Lowercase(value.substr(0, scheme_end));
    if (parsed.scheme != "https" && parsed.scheme != "wss") {
        return false;
    }

    const size_t authority_start = scheme_end + 3;
    const size_t suffix_start = value.find_first_of("/?#", authority_start);
    const std::string authority = value.substr(authority_start, suffix_start - authority_start);
    if (authority.empty() || authority.find('@') != std::string::npos ||
        authority.front() == '[') {
        return false;
    }

    const size_t port_separator = authority.rfind(':');
    if (port_separator == std::string::npos) {
        parsed.host = Lowercase(authority);
        parsed.port = 443;
    } else {
        parsed.host = Lowercase(authority.substr(0, port_separator));
        if (!ParsePort(std::string_view(authority).substr(port_separator + 1), parsed.port)) {
            return false;
        }
    }
    if (parsed.host.empty() || parsed.host.back() == '.') {
        return false;
    }

    if (suffix_start == std::string::npos) {
        parsed.path = "/";
        return true;
    }
    if (value[suffix_start] == '#') {
        return false;
    }

    const size_t fragment_start = value.find('#', suffix_start);
    if (fragment_start != std::string::npos) {
        return false;
    }
    const size_t query_start = value.find('?', suffix_start);
    parsed.has_query = query_start != std::string::npos;
    const size_t path_end = parsed.has_query ? query_start : value.size();
    parsed.path = value[suffix_start] == '?' ? "/" : value.substr(suffix_start, path_end - suffix_start);
    return !HasUnsafePathSyntax(parsed.path);
}

bool HasApprovedOrigin(const ParsedUrl& parsed, std::string_view scheme) {
    return parsed.scheme == scheme && parsed.host == kExpectedHost && parsed.port == 443;
}

bool IsExactEndpoint(const std::string& url, std::string_view scheme, std::string_view path) {
    ParsedUrl parsed;
    return ParseUrl(url, parsed) && HasApprovedOrigin(parsed, scheme) && !parsed.has_query &&
           parsed.path == path;
}

std::vector<std::string_view> SplitPath(std::string_view path) {
    std::vector<std::string_view> segments;
    size_t start = 1;
    while (start < path.size()) {
        const size_t end = path.find('/', start);
        if (end == std::string_view::npos) {
            segments.push_back(path.substr(start));
            break;
        }
        segments.push_back(path.substr(start, end - start));
        start = end + 1;
    }
    return segments;
}

bool IsSafeVersionSegment(std::string_view version) {
    if (version.empty() || version.size() > 31 || version.front() == '.' ||
        version.back() == '.') {
        return false;
    }

    size_t component_start = 0;
    size_t component_count = 0;
    while (component_start < version.size()) {
        const size_t component_end = version.find('.', component_start);
        const auto component = version.substr(
            component_start,
            component_end == std::string_view::npos ? version.size() - component_start
                                                    : component_end - component_start);
        if (component.empty() || component.size() > 5 ||
            (component.size() > 1 && component.front() == '0') ||
            !std::all_of(component.begin(), component.end(), [](unsigned char character) {
                return std::isdigit(character);
            })) {
            return false;
        }
        uint32_t value = 0;
        for (char character : component) {
            value = value * 10 + static_cast<uint32_t>(character - '0');
        }
        if (value > UINT16_MAX || ++component_count > 3) {
            return false;
        }
        if (component_end == std::string_view::npos) {
            break;
        }
        component_start = component_end + 1;
    }
    return component_count == 3;
}

bool ParseVersionComponents(std::string_view version,
                            std::array<uint16_t, 3>& components) {
    if (!IsSafeVersionSegment(version)) {
        return false;
    }
    size_t start = 0;
    for (size_t index = 0; index < components.size(); ++index) {
        const size_t end = version.find('.', start);
        const auto component = version.substr(
            start, end == std::string_view::npos ? version.size() - start : end - start);
        uint32_t value = 0;
        for (char character : component) {
            value = value * 10 + static_cast<uint32_t>(character - '0');
        }
        components[index] = static_cast<uint16_t>(value);
        start = end == std::string_view::npos ? version.size() : end + 1;
    }
    return true;
}

bool IsSha256BinaryName(std::string_view filename) {
    constexpr size_t kSha256HexLength = 64;
    constexpr std::string_view kSuffix = ".bin";
    if (filename.size() != kSha256HexLength + kSuffix.size() ||
        filename.substr(kSha256HexLength) != kSuffix) {
        return false;
    }
    return std::all_of(filename.begin(), filename.begin() + kSha256HexLength,
                       [](unsigned char character) { return std::isxdigit(character); });
}

bool IsHexCharacter(char character) {
    return std::isxdigit(static_cast<unsigned char>(character));
}

uint8_t HexValue(char character) {
    if (character >= '0' && character <= '9') {
        return static_cast<uint8_t>(character - '0');
    }
    return static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(character)) - 'a' + 10);
}

bool ParseFirmwareUrl(const std::string& url, std::string& version, std::string& filename) {
    ParsedUrl parsed;
    if (!ParseUrl(url, parsed) || !HasApprovedOrigin(parsed, "https") || parsed.has_query) {
        return false;
    }

    const auto segments = SplitPath(parsed.path);
    const std::string_view kExpectedSegments[] = {"kitchen-helper", "preview", "v1", "firmware",
                                                  kExpectedFirmwareIdentity};
    if (segments.size() != 7) {
        return false;
    }
    for (size_t index = 0; index < std::size(kExpectedSegments); ++index) {
        if (segments[index] != kExpectedSegments[index]) {
            return false;
        }
    }
    if (!IsSafeVersionSegment(segments[5]) || !IsSha256BinaryName(segments[6])) {
        return false;
    }
    version.assign(segments[5]);
    filename.assign(segments[6]);
    return true;
}

}  // namespace

namespace ProvisionsEndpointPolicy {

const char* BootstrapUrl() { return CONFIG_OTA_URL; }

const char* WebsocketUrl() { return CONFIG_PROVISIONS_PREVIEW_WEBSOCKET_URL; }

const char* OtaManifestUrl() { return kExpectedOtaManifestUrl.data(); }

const char* HealthUrl() { return kExpectedHealthUrl.data(); }

bool IsAllowedBootstrapUrl(const std::string& url) {
    return IsExactEndpoint(url, "https", "/kitchen-helper/preview/v1/bootstrap");
}

bool IsAllowedWebsocketUrl(const std::string& url) {
    return IsExactEndpoint(url, "wss", "/kitchen-helper/preview/v1/device");
}

bool IsAllowedOtaManifestUrl(const std::string& url) {
    return IsExactEndpoint(url, "https", "/kitchen-helper/preview/v1/firmware/manifest.json");
}

bool IsAllowedFirmwareUrl(const std::string& url) {
    std::string version;
    std::string filename;
    return ParseFirmwareUrl(url, version, filename);
}

bool FirmwareUrlMatchesVersion(const std::string& url, const std::string& version) {
    std::string url_version;
    std::string filename;
    return ParseFirmwareUrl(url, url_version, filename) && url_version == version;
}

bool ExtractFirmwareVersion(const std::string& url, std::string& version) {
    std::string filename;
    return ParseFirmwareUrl(url, version, filename);
}

bool ExtractFirmwareSha256(const std::string& url, std::array<uint8_t, 32>& digest) {
    std::string version;
    std::string filename;
    if (!ParseFirmwareUrl(url, version, filename)) {
        return false;
    }
    for (size_t index = 0; index < digest.size(); ++index) {
        digest[index] = static_cast<uint8_t>((HexValue(filename[index * 2]) << 4) |
                                             HexValue(filename[index * 2 + 1]));
    }
    return true;
}

bool IsNewerFirmwareVersion(const std::string& current_version,
                            const std::string& candidate_version) {
    std::array<uint16_t, 3> current{};
    std::array<uint16_t, 3> candidate{};
    if (!ParseVersionComponents(current_version, current) ||
        !ParseVersionComponents(candidate_version, candidate)) {
        return false;
    }
    return candidate > current;
}

bool IsApprovedFirmwareImageVersion(const std::string& url,
                                    const std::string& current_version,
                                    const std::string& embedded_version) {
    std::string advertised_version;
    return ExtractFirmwareVersion(url, advertised_version) &&
           advertised_version == embedded_version &&
           IsNewerFirmwareVersion(current_version, embedded_version);
}

bool IsAllowedHealthUrl(const std::string& url) {
    return IsExactEndpoint(url, "https", "/kitchen-helper/preview/v1/health");
}

bool IsValidDeviceToken(const std::string& token) {
    constexpr std::string_view kPrefix = "pvd1_";
    constexpr size_t kUuidLength = 36;
    constexpr size_t kSecretLength = 43;
    constexpr size_t kSeparatorIndex = kPrefix.size() + kUuidLength;
    constexpr size_t kTokenLength = kSeparatorIndex + 1 + kSecretLength;
    if (token.size() != kTokenLength || token.compare(0, kPrefix.size(), kPrefix) != 0 ||
        token[kSeparatorIndex] != '.' ||
        !IsCanonicalUuid(token.substr(kPrefix.size(), kUuidLength))) {
        return false;
    }
    return std::all_of(token.begin() + kSeparatorIndex + 1, token.end(),
                       [](unsigned char character) {
                           return std::isalnum(character) || character == '-' || character == '_';
                       });
}

bool IsCanonicalUuid(const std::string& value) {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' || value[18] != '-' ||
        value[23] != '-') {
        return false;
    }
    for (size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            continue;
        }
        if (!IsHexCharacter(value[index])) {
            return false;
        }
    }

    const char version = static_cast<char>(std::tolower(static_cast<unsigned char>(value[14])));
    const char variant = static_cast<char>(std::tolower(static_cast<unsigned char>(value[19])));
    return version >= '1' && version <= '8' &&
           (variant == '8' || variant == '9' || variant == 'a' || variant == 'b');
}

}  // namespace ProvisionsEndpointPolicy
