#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

// Board-local, allocation-free motion/caption contract. No audio or business
// records are changed by this view model.
namespace OrbitCrest {

constexpr int kDisplaySize = 466;
constexpr int kSafeRadius = 201;
constexpr int kTransitionMs = 360;
constexpr int kResultHoldMs = 4000;
constexpr uint32_t kIvory = 0xE8E0D2;
constexpr uint32_t kGold = 0xC5A46D;
constexpr uint32_t kAmber = 0xD7A45B;
constexpr float kPi = 3.14159265359F;
constexpr uint8_t kActiveStarOpacity = 224;

enum class State { Boot, Connecting, Idle, Listening, Thinking, Speaking, Error, Result };

struct Frame {
    std::array<int, 3> radii{120, 154, 184};
    std::array<uint8_t, 3> opacity{0, 0, 0};
    uint8_t band_opacity = 255;
    uint8_t star_opacity = 255;
    uint32_t color = kIvory;
};

inline uint8_t Opacity(float value) {
    return static_cast<uint8_t>(std::clamp(value, 0.0F, 1.0F) * 255.0F);
}

inline float RaisedCosine(float cycles) { return 0.5F - 0.5F * std::cos(2.0F * kPi * cycles); }

inline float SmootherStep(float phase) {
    phase = std::clamp(phase, 0.0F, 1.0F);
    return phase * phase * phase * (phase * (phase * 6.0F - 15.0F) + 10.0F);
}

inline uint32_t BlendColor(uint32_t from, uint32_t target, float amount) {
    uint32_t result = 0;
    for (int shift : {16, 8, 0}) {
        const int start = static_cast<int>((from >> shift) & 0xFF);
        const int end = static_cast<int>((target >> shift) & 0xFF);
        const auto channel =
            static_cast<uint32_t>(std::lround(start + static_cast<float>(end - start) * amount));
        result |= channel << shift;
    }
    return result;
}

inline bool UsesRings(State state) {
    return state == State::Listening || state == State::Thinking || state == State::Speaking;
}

inline const char* Caption(State state) {
    switch (state) {
        case State::Boot:
            return "Starting";
        case State::Connecting:
            return "Connecting";
        case State::Thinking:
            return "Thinking";
        case State::Error:
            return "Please try again";
        default:
            return "";
    }
}

// Only authenticated, existing gateway display labels may become captions.
// Unknown strings and raw transcripts are never rendered as technical labels.
inline const char* ResultCaption(const char* text) {
    if (text == nullptr)
        return "";
    struct Mapping {
        const char* input;
        const char* caption;
    };
    constexpr Mapping mappings[] = {
        {"Added", "Draft updated\nNot sent"},
        {"Undone", "Removed from draft"},
        {"Found", "Found in records"},
        {"Delivered", "Delivery recorded"},
        {"On the way", "On the way"},
        {"Replied", "Reply received"},
        {"No new reply", "No new reply"},
        {"Reply waiting", "Reply waiting"},
        {"No thread", "No conversation"},
        {"Draft only", "Draft only\nNot sent"},
        {"Recorded", "From your records"},
        {"Choose one", "Which one?"},
        {"Need unit", "Which unit?"},
        {"Ready to add", "Check Provisions"},
        {"No match", "No match"},
        {"Cancelled", "Cancelled"},
        {"Check app", "Check Provisions"},
        {"Not changed", "Nothing changed"},
    };
    for (const auto& mapping : mappings) {
        if (std::strcmp(text, mapping.input) == 0)
            return mapping.caption;
    }
    return "";
}

inline float AudioLevel(uint32_t mean_absolute, uint32_t age_ms) {
    if (age_ms > 120)
        return 0.0F;
    // Conservative fixed bench floor; physical kitchen calibration is pending.
    const float above_floor = std::max(0.0F, static_cast<float>(mean_absolute) - 100.0F);
    return std::sqrt(std::min(above_floor / 6000.0F, 1.0F));
}

inline Frame Rings(State state, uint32_t elapsed_ms, float level, bool reduced_motion) {
    Frame frame;
    // Keep the exact center star as a quiet visual anchor. The outer crest
    // band alone yields to three fixed circles; their geometry never changes
    // while LVGL is painting the active face.
    frame.band_opacity = 0;
    frame.star_opacity = kActiveStarOpacity;
    level = std::clamp(level, 0.0F, 1.0F);
    frame.color = state == State::Listening ? kGold : kIvory;
    const float time = static_cast<float>(elapsed_ms) / 1000.0F;

    if (reduced_motion) {
        if (state == State::Thinking) {
            frame.opacity = {0, 0, Opacity(0.58F)};
        } else if (state == State::Speaking) {
            frame.opacity = {0, Opacity(0.58F), Opacity(0.42F)};
        } else {
            frame.opacity = {Opacity(0.68F), Opacity(0.52F), Opacity(0.38F)};
        }
        return frame;
    }

    constexpr std::array<float, 3> kListeningBase{0.68F, 0.52F, 0.38F};
    const float listening_breath = RaisedCosine(time / 2.4F);
    for (int index = 0; index < 3; ++index) {
        float opacity = kListeningBase[index];
        if (state == State::Listening) {
            // One slow shared breath keeps the circles optically concentric;
            // voice energy adds presence without making their edges wobble.
            opacity += 0.08F * listening_breath + 0.16F * level;
        } else if (state == State::Speaking) {
            // A continuous luminance wave moves out through fixed circles.
            // There is no sawtooth radius reset and natural PCM gaps cannot
            // stop its clock; audio contributes only a bounded lift.
            const float wave = RaisedCosine(time / 2.2F - index * 0.16F);
            opacity = 0.30F + 0.36F * wave + 0.18F * level;
        } else if (state == State::Thinking) {
            const float wave = RaisedCosine(time / 2.8F - index / 3.0F);
            opacity = 0.22F + 0.36F * wave;
        }
        frame.opacity[index] = Opacity(opacity);
    }
    return frame;
}

// Blend from the last rendered frame so rapid press/release cannot jump back
// to a fully opaque crest or briefly expose a blank face.
inline Frame Transition(const Frame& from, const Frame& target, uint32_t elapsed_ms,
                        bool reduced_motion) {
    if (reduced_motion || elapsed_ms >= kTransitionMs)
        return target;
    Frame out = target;
    const float phase = static_cast<float>(elapsed_ms) / kTransitionMs;
    const float ease = SmootherStep(phase);
    const auto blend = [ease](int a, int b) {
        return static_cast<int>(std::lround(a + static_cast<float>(b - a) * ease));
    };
    out.band_opacity = blend(from.band_opacity, target.band_opacity);
    out.star_opacity = blend(from.star_opacity, target.star_opacity);
    out.color = BlendColor(from.color, target.color, ease);
    for (int index = 0; index < 3; ++index) {
        out.radii[index] = blend(from.radii[index], target.radii[index]);
        out.opacity[index] = blend(from.opacity[index], target.opacity[index]);
    }
    return out;
}

}  // namespace OrbitCrest
