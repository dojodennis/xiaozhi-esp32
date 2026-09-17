#pragma once

#include <algorithm>
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
constexpr int kDashSlots = 8;
constexpr int kRingRadius = 140;
constexpr uint32_t kIvory = 0xE8E0D2;
constexpr uint32_t kGold = 0xC5A46D;
constexpr uint32_t kAmber = 0xD7A45B;
// Calm Ready ring. PTT stages stay in the yellow family from the moodboard;
// Working/Speaking must never fall back to teal or ivory.
constexpr uint32_t kTeal = 0x4ECDC4;
constexpr uint32_t kPttYellow = 0xF2C84B;
constexpr float kPi = 3.14159265359F;
constexpr uint8_t kActiveStarOpacity = 224;

enum class State { Boot, Connecting, Idle, Listening, Thinking, Speaking, Error, Result };

struct Frame {
    int radius = kRingRadius;
    int width = 6;
    uint8_t opacity = 0;
    uint8_t band_opacity = 255;
    uint8_t star_opacity = 255;
    uint32_t color = kIvory;
    uint8_t dash_count = 1;
    uint16_t dash_span_deg = 360;
    int16_t rotation_deg = 0;
};

struct DashSlot {
    bool visible = false;
    int rotation_deg = 0;
    uint16_t span_deg = 360;
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

inline bool IsPttYellow(uint32_t color) {
    const int red = static_cast<int>((color >> 16) & 0xFF);
    const int green = static_cast<int>((color >> 8) & 0xFF);
    const int blue = static_cast<int>(color & 0xFF);
    return red > 180 && green > 140 && blue < 120 && red >= green && green > blue + 40;
}

inline bool IsTeal(uint32_t color) {
    const int red = static_cast<int>((color >> 16) & 0xFF);
    const int green = static_cast<int>((color >> 8) & 0xFF);
    const int blue = static_cast<int>(color & 0xFF);
    return green > 140 && blue > 140 && red < green && red < blue;
}

inline const char* Caption(State state) {
    switch (state) {
        case State::Boot:
            return "Starting";
        case State::Connecting:
            return "Connecting";
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

inline Frame QuietRing(uint32_t color, int width, uint8_t opacity) {
    Frame frame;
    frame.band_opacity = 0;
    frame.star_opacity = 0;
    frame.color = color;
    frame.radius = kRingRadius;
    frame.width = width;
    frame.opacity = opacity;
    frame.dash_count = 1;
    frame.dash_span_deg = 360;
    frame.rotation_deg = 0;
    return frame;
}

inline Frame Rings(State state, uint32_t elapsed_ms, float level, bool reduced_motion) {
    level = std::clamp(level, 0.0F, 1.0F);
    const float time = static_cast<float>(elapsed_ms) / 1000.0F;

    if (state == State::Idle)
        return QuietRing(kTeal, 6, Opacity(0.82F));
    if (state == State::Result)
        return QuietRing(kPttYellow, 6, Opacity(0.28F));

    Frame frame = QuietRing(kPttYellow, 18, 0);
    if (state == State::Listening) {
        frame.width = 20;
        frame.dash_count = 1;
        frame.dash_span_deg = 360;
        const float breath = reduced_motion ? 0.0F : RaisedCosine(time / 2.4F);
        frame.opacity = Opacity(0.78F + 0.10F * breath + 0.12F * level);
        return frame;
    }

    // Working and Speaking stay in the same yellow family. Geometry and motion
    // carry the stage: rotating dashes vs a softer stationary pulse.
    frame.dash_count = kDashSlots;
    if (state == State::Thinking) {
        frame.width = 14;
        frame.dash_span_deg = 26;
        frame.rotation_deg =
            reduced_motion ? 0 : static_cast<int16_t>((elapsed_ms / 6) % 360);  // ~167 deg/s
        frame.opacity = Opacity(0.92F);
        return frame;
    }

    frame.width = 12;
    frame.dash_span_deg = 16;
    frame.rotation_deg = 0;
    const float pulse = reduced_motion ? 0.55F : RaisedCosine(time / 1.8F);
    frame.opacity = Opacity(0.38F + 0.40F * pulse + 0.18F * level);
    return frame;
}

inline Frame Face(State state, uint32_t elapsed_ms, float level, bool reduced_motion) {
    if (UsesRings(state) || state == State::Idle || state == State::Result)
        return Rings(state, elapsed_ms, level, reduced_motion);
    Frame frame;
    if (state == State::Error) {
        frame = QuietRing(kAmber, 8, 200);
        frame.dash_span_deg = 336;
        frame.rotation_deg = 270;
        return frame;
    }
    if (state == State::Boot || state == State::Connecting) {
        frame.band_opacity = 255;
        frame.star_opacity = 255;
    } else {
        frame.band_opacity = 0;
        frame.star_opacity = 0;
    }
    return frame;
}

inline DashSlot Slot(const Frame& frame, int index) {
    DashSlot slot;
    if (frame.opacity == 0 || index < 0 || index >= kDashSlots)
        return slot;
    if (frame.dash_count <= 1) {
        slot.visible = index == 0;
        slot.rotation_deg = frame.rotation_deg;
        slot.span_deg = frame.dash_span_deg;
        return slot;
    }
    slot.visible = index < frame.dash_count;
    const int step = 360 / frame.dash_count;
    slot.rotation_deg = (static_cast<int>(frame.rotation_deg) + index * step) % 360;
    slot.span_deg = frame.dash_span_deg;
    return slot;
}

// Blend from the last rendered frame so rapid press/release cannot jump back
// to a fully opaque crest or briefly expose a blank face. Dash geometry snaps
// with the target stage so a solid Listening ring does not collapse into eight
// overlapping full circles.
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
    out.radius = blend(from.radius, target.radius);
    out.width = std::max(1, blend(from.width, target.width));
    out.opacity = blend(from.opacity, target.opacity);
    return out;
}

}  // namespace OrbitCrest
