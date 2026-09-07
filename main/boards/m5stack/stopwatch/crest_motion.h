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
constexpr int kTransitionMs = 220;
constexpr int kResultHoldMs = 4000;
constexpr uint32_t kIvory = 0xE8E0D2;
constexpr uint32_t kGold = 0xC5A46D;
constexpr uint32_t kAmber = 0xD7A45B;
constexpr float kPi = 3.14159265359F;

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
    frame.band_opacity = frame.star_opacity = 0;
    level = std::clamp(level, 0.0F, 1.0F);
    frame.color = state == State::Listening ? kGold : kIvory;
    const float time = static_cast<float>(elapsed_ms) / 1000.0F;
    for (int index = 0; index < 3; ++index) {
        float opacity = 0.8F;
        if (!reduced_motion && state == State::Listening) {
            frame.radii[index] += static_cast<int>(
                std::lround((1.0F + 4.0F * level) * std::sin(8.0F * kPi * time + index * 1.2F)));
        } else if (!reduced_motion && state == State::Speaking) {
            // The caller advances this clock only while output PCM is active.
            const float phase = std::fmod(time / 0.9F + index / 3.0F, 1.0F);
            frame.radii[index] = static_cast<int>(116.0F + 70.0F * phase + 8.0F * level);
            opacity = 0.25F + 0.65F * std::sin(kPi * phase);
        } else if (!reduced_motion && state == State::Thinking) {
            opacity = 0.35F + 0.4F * (0.5F + 0.5F * std::sin(2.0F * kPi * time / 1.8F +
                                                             index * 2.0F * kPi / 3.0F));
        }
        frame.opacity[index] = Opacity(opacity);
    }
    if (reduced_motion && state == State::Thinking)
        frame.opacity[0] = frame.opacity[1] = 0;
    if (reduced_motion && state == State::Speaking)
        frame.opacity[0] = 0;
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
    const float ease = phase * phase * (3.0F - 2.0F * phase);
    const auto blend = [ease](int a, int b) { return a + static_cast<int>((b - a) * ease); };
    out.band_opacity = blend(from.band_opacity, target.band_opacity);
    const float star_phase =
        target.star_opacity < from.star_opacity
            ? std::clamp((static_cast<float>(elapsed_ms) - 120) / 100, 0.0F, 1.0F)
            : std::min(static_cast<float>(elapsed_ms) / 100, 1.0F);
    out.star_opacity = from.star_opacity +
                       static_cast<int>((target.star_opacity - from.star_opacity) * star_phase);
    for (int index = 0; index < 3; ++index) {
        out.radii[index] = blend(from.radii[index], target.radii[index]);
        out.opacity[index] = blend(from.opacity[index], target.opacity[index]);
    }
    return out;
}

}  // namespace OrbitCrest
