#ifndef PROVISIONS_LITE_FACE_H_
#define PROVISIONS_LITE_FACE_H_

// Orbit Lite face messages:
//   {"type":"provisions","state":"face","face":"<name>","text":"<≤40 chars>"}
//
// The StopWatch renders faces from two existing inputs: Display::SetStatus()
// (resting screens: READY, CHECKING, REPLY, OFFLINE) and
// Display::ShowNotification() (3 s banners; an unknown string becomes an amber
// NOTICE-style banner titled with that string). A lite face maps onto one of
// those; where the ring has no exact screen the closest one is chosen and
// documented here. Any non-empty text goes to the reply surface via
// SetChatMessage("assistant", text), headed by the last banner title.
//
//   face      | render                          | ring screen
//   ----------+---------------------------------+-------------------------------
//   ready     | SetStatus(idle status)          | READY / HOLD TO TALK (green)
//   working   | SetStatus("Working")            | CHECKING / ONE MOMENT (blue)
//   speaking  | SetStatus("Speaking")           | REPLY / LISTEN (blue)
//   recorded  | ShowNotification("Recorded")    | RECORDED / FROM RECORDS (amber)
//   timer     | ShowNotification("TIMER")       | TIMER / LISTEN banner (amber)
//             |                                 |   (closest: no timer screen
//             |                                 |    without timers_v1)
//   failed    | ShowNotification("TRY AGAIN")   | TRY AGAIN / LISTEN banner (amber),
//             |                                 |   then back to the resting READY
//             |                                 |   (closest: the ring's only red
//             |                                 |    screen says OFFLINE, which
//             |                                 |    a connected turn must not)
//   offline   | SetStatus("Unavailable")        | OFFLINE / TRY AGAIN (red)
//
// Status faces also become the idle status (they survive the 1 s status tick)
// until the next press, tts start, turn end or face; banner faces auto-reset.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace provisions::lite {

enum class Face : uint8_t { kReady, kWorking, kSpeaking, kRecorded, kTimer, kFailed, kOffline };

constexpr size_t kMaximumFaceTextBytes = 40;

struct FaceRender {
    const char* status = nullptr;        // Display::SetStatus() when set
    const char* notification = nullptr;  // Display::ShowNotification() when set
    // Idle-status override kept until the next press/tts/face (nullptr: none).
    const char* idle_status = nullptr;
};

inline bool ParseFace(std::string_view name, Face& out) {
    struct Entry {
        const char* name;
        Face face;
    };
    static constexpr Entry kEntries[] = {
        {"ready", Face::kReady},       {"working", Face::kWorking}, {"speaking", Face::kSpeaking},
        {"recorded", Face::kRecorded}, {"timer", Face::kTimer},     {"failed", Face::kFailed},
        {"offline", Face::kOffline},
    };
    for (const auto& entry : kEntries) {
        if (name == entry.name) {
            out = entry.face;
            return true;
        }
    }
    return false;
}

inline FaceRender RenderFor(Face face) {
    switch (face) {
        case Face::kReady:
            return {"Ready", nullptr, nullptr};
        case Face::kWorking:
            return {"Working", nullptr, "Working"};
        case Face::kSpeaking:
            return {"Speaking", nullptr, "Speaking"};
        case Face::kRecorded:
            return {nullptr, "Recorded", nullptr};
        case Face::kTimer:
            return {nullptr, "TIMER", nullptr};
        case Face::kFailed:
            return {nullptr, "TRY AGAIN", nullptr};
        case Face::kOffline:
            return {"Unavailable", nullptr, "Unavailable"};
    }
    return {"Unavailable", nullptr, "Unavailable"};
}

// A face that ends the gateway's turn: response-pending clears and any
// playback still running is cut.
inline bool EndsTurn(Face face) {
    return face == Face::kReady || face == Face::kFailed || face == Face::kOffline;
}

// Drop control characters and cut at kMaximumFaceTextBytes without splitting
// a UTF-8 sequence. Empty in, empty out.
inline std::string FaceText(std::string_view text) {
    std::string out;
    out.reserve(text.size() < kMaximumFaceTextBytes ? text.size() : kMaximumFaceTextBytes);
    for (const unsigned char c : text) {
        if (c < 0x20 || c == 0x7f)
            continue;
        out.push_back(static_cast<char>(c));
    }
    if (out.size() > kMaximumFaceTextBytes) {
        size_t cut = kMaximumFaceTextBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xc0) == 0x80)
            --cut;
        out.resize(cut);
    }
    return out;
}

}  // namespace provisions::lite

#endif  // PROVISIONS_LITE_FACE_H_
