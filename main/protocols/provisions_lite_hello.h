#ifndef PROVISIONS_LITE_HELLO_H_
#define PROVISIONS_LITE_HELLO_H_

// Orbit Lite server hello. The thin gateway answers the unchanged device hello
// with `"provisions": {"mode": "lite", "selected": []}`; nothing else in the
// hello is negotiated: no timers_v1 / timer_claim_recovery_v1 / dictation_v1 /
// audio_capture requirement, no output fence, receipt or grant arming, no
// session-UUID policy. Audio parameters are taken exactly as stock xiaozhi
// does. This header depends only on cJSON so the host suite can exercise it
// against a stub tree.

#include <cJSON.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace provisions::lite {

struct HelloParams {
    std::string session_id;  // may stay empty; lite frames carry no session
    int sample_rate = 24000;
    int frame_duration = 60;
};

enum class HelloResult {
    kNotLite,          // no provisions.mode == "lite": use the full negotiation
    kAccepted,
    kBadVersion,       // version present and not 1
    kBadAudioFormat,   // audio_params.format present and not "opus"
    kBadChannels,      // audio_params.channels present and not 1
    kBadSampleRate,    // audio_params.sample_rate not 16000 or 24000
    kBadFrameDuration  // audio_params.frame_duration not 20, 40 or 60 ms
};

// The lite gateway has no conversation model. The local recorder still needs
// a non-zero conversation identity for every capture it journals (and it only
// offers journalled captures whose conversation matches the live context), so
// every lite session shares this fixed identity. Captures made under a full
// gateway keep their own conversation and are never offered to a lite gateway.
inline constexpr std::array<uint8_t, 16> kConversationId = {
    'o', 'r', 'b', 'i', 't', '-', 'l', 'i', 't', 'e', '-', 'v', '1', 0, 0, 0};

inline bool IsLiteHello(const cJSON* provisions) {
    if (!cJSON_IsObject(provisions))
        return false;
    const cJSON* mode = cJSON_GetObjectItemCaseSensitive(provisions, "mode");
    return cJSON_IsString(mode) && mode->valuestring != nullptr &&
           std::strcmp(mode->valuestring, "lite") == 0;
}

// 24 kHz is the ES8311 output rate on the StopWatch, so 24 kHz Opus decodes
// natively; 16 kHz goes through the existing rate converter.
inline bool SupportedSampleRate(int rate) { return rate == 16000 || rate == 24000; }
inline bool SupportedFrameDuration(int ms) { return ms == 20 || ms == 40 || ms == 60; }

// Stock xiaozhi semantics: every audio parameter is optional and defaults to
// the fork's 24 kHz / 60 ms output; a present value must be one the decode
// path handles. `version` is optional and must be 1 when present.
inline HelloResult ParseLiteHello(const cJSON* root, HelloParams& out) {
    const cJSON* provisions = cJSON_GetObjectItemCaseSensitive(root, "provisions");
    if (!IsLiteHello(provisions))
        return HelloResult::kNotLite;
    const cJSON* version = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (version != nullptr && (!cJSON_IsNumber(version) || version->valuedouble != 1))
        return HelloResult::kBadVersion;

    HelloParams params;
    const cJSON* session = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    if (cJSON_IsString(session) && session->valuestring != nullptr)
        params.session_id = session->valuestring;

    const cJSON* audio = cJSON_GetObjectItemCaseSensitive(root, "audio_params");
    if (cJSON_IsObject(audio)) {
        const cJSON* format = cJSON_GetObjectItemCaseSensitive(audio, "format");
        if (format != nullptr && (!cJSON_IsString(format) || format->valuestring == nullptr ||
                                  std::strcmp(format->valuestring, "opus") != 0))
            return HelloResult::kBadAudioFormat;
        const cJSON* channels = cJSON_GetObjectItemCaseSensitive(audio, "channels");
        if (channels != nullptr && (!cJSON_IsNumber(channels) || channels->valuedouble != 1))
            return HelloResult::kBadChannels;
        const cJSON* rate = cJSON_GetObjectItemCaseSensitive(audio, "sample_rate");
        if (rate != nullptr) {
            if (!cJSON_IsNumber(rate) || !SupportedSampleRate(rate->valueint))
                return HelloResult::kBadSampleRate;
            params.sample_rate = rate->valueint;
        }
        const cJSON* duration = cJSON_GetObjectItemCaseSensitive(audio, "frame_duration");
        if (duration != nullptr) {
            if (!cJSON_IsNumber(duration) || !SupportedFrameDuration(duration->valueint))
                return HelloResult::kBadFrameDuration;
            params.frame_duration = duration->valueint;
        }
    }
    out = std::move(params);
    return HelloResult::kAccepted;
}

}  // namespace provisions::lite

#endif  // PROVISIONS_LITE_HELLO_H_
