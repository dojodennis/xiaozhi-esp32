#ifndef PROVISIONS_STOPWATCH_BUTTON_CHORD_H_
#define PROVISIONS_STOPWATCH_BUTTON_CHORD_H_

#include <cstdint>

namespace ProvisionsStopWatch {

// Talk (yellow) + blue pressed within kWindowUs of each other is a chord that
// toggles the timer face. Neither single-button action may fire for a chord:
// the Talk start is held for the window (a chord never opens the microphone
// and never leaves a stray capture), and every blue gesture that belongs to
// the chord is swallowed until blue is pressed again on its own.
// Physical edges and the window timer all run on ESP_TIMER_TASK; the class
// itself is plain data so it can be host-tested.
class ButtonChord {
public:
    static constexpr int64_t kWindowUs = 150 * 1000;
    enum class Edge : uint8_t {
        kNone,      // Nothing to do now.
        kArmTalk,   // Start the window timer; Talk starts at TalkWindowElapsed.
        kChord,     // Toggle the timer face; any armed Talk start is cancelled.
    };

    Edge TalkDown(int64_t now_us) {
        talk_down_ = true;
        talk_started_ = false;
        talk_down_us_ = now_us;
        if (chord_)
            return Edge::kNone;  // Still inside a chord that has not been released.
        if (blue_down_ && now_us - blue_down_us_ <= kWindowUs)
            return BeginChord();
        return Edge::kArmTalk;
    }

    // True when Talk should start now: held for the whole window, no chord.
    bool TalkWindowElapsed() {
        if (!talk_down_ || talk_started_ || chord_)
            return false;
        talk_started_ = true;
        return true;
    }

    // True when the release must stop a Talk capture that actually started.
    bool TalkUp() {
        const bool started = talk_started_;
        // Released inside the window with no chord: a click. The microphone
        // never opens for it, but the menu confirms on it, so a quick yellow
        // tap on "Timers" works the first time instead of needing a hold.
        talk_clicked_ = talk_down_ && !talk_started_ && !chord_;
        talk_down_ = talk_started_ = false;
        if (!blue_down_)
            chord_ = false;
        return started;
    }

    // True once per click reported by TalkUp().
    bool TakeTalkClick() {
        const bool clicked = talk_clicked_;
        talk_clicked_ = false;
        return clicked;
    }

    Edge BlueDown(int64_t now_us) {
        blue_down_ = true;
        blue_down_us_ = now_us;
        if (chord_) {
            swallow_blue_ = true;
            return Edge::kNone;
        }
        if (talk_down_ && !talk_started_ && now_us - talk_down_us_ <= kWindowUs)
            return BeginChord();
        swallow_blue_ = false;  // A fresh blue press on its own acts normally.
        return Edge::kNone;
    }

    void BlueUp() {
        blue_down_ = false;
        if (!talk_down_)
            chord_ = false;
    }

    // Blue click/double/long callbacks ask this first; true means "belongs to
    // a chord, do nothing".
    bool SwallowBlueGesture() const { return swallow_blue_; }

    // Both buttons are down inside a chord. The lock commits only after they
    // have been held, so a brush in a pocket does not lock or unlock.
    bool BlueHeld() const { return blue_down_; }

    bool BothHeld() const { return chord_ && talk_down_ && blue_down_; }

private:
    Edge BeginChord() {
        chord_ = swallow_blue_ = true;
        talk_started_ = false;
        return Edge::kChord;
    }
    bool talk_down_ = false, talk_started_ = false, talk_clicked_ = false, blue_down_ = false;
    bool chord_ = false, swallow_blue_ = false;
    int64_t talk_down_us_ = 0, blue_down_us_ = 0;
};

}  // namespace ProvisionsStopWatch

#endif  // PROVISIONS_STOPWATCH_BUTTON_CHORD_H_
