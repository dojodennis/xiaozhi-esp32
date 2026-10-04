#ifndef PROVISIONS_STOPWATCH_BUTTON_CHORD_H_
#define PROVISIONS_STOPWATCH_BUTTON_CHORD_H_

#include <cstdint>

namespace ProvisionsStopWatch {

// Talk (yellow) + blue pressed within kWindowUs of each other is a chord.
// A short chord opens the mode choice after both releases; a held chord locks.
// Neither single-button action may fire for a chord:
// the Talk start is held for the window (a chord never opens the microphone
// and never leaves a stray capture), and every blue gesture that belongs to
// the chord is swallowed until blue is pressed again on its own.
// Physical edges and the window timer all run on ESP_TIMER_TASK; the class
// itself is plain data so it can be host-tested.
class ButtonChord {
public:
    static constexpr int64_t kWindowUs = 150 * 1000;
    static constexpr int64_t kLockHoldUs = 600 * 1000;
    enum class Edge : uint8_t {
        kNone,     // Nothing to do now.
        kArmTalk,  // Start the window timer; Talk starts at TalkWindowElapsed.
        kChord,    // Arm the lock hold; any armed Talk start is cancelled.
    };

    Edge TalkDown(int64_t now_us) {
        ++generation_;
        mode_choice_ready_ = 0;
        talk_down_ = true;
        talk_started_ = false;
        talk_down_us_ = now_us;
        if (chord_) {
            mode_choice_allowed_ = false;  // A partial release/repress is not a new choice.
            return Edge::kNone;  // Still inside a chord that has not been released.
        }
        if (blue_down_ && now_us - blue_down_us_ <= kWindowUs)
            return BeginChord(now_us);
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
    bool TalkUp(int64_t now_us = 0) {
        const bool started = talk_started_;
        // Released inside the window with no chord: a click. The microphone
        // never opens for it, but the menu confirms on it, so a quick yellow
        // tap on "Timers" works the first time instead of needing a hold.
        talk_clicked_ = talk_down_ && !talk_started_ && !chord_;
        talk_down_ = talk_started_ = false;
        lock_hold_allowed_ = false;
        FinishChordIfReleased(now_us);
        return started;
    }

    // True once per click reported by TalkUp().
    bool TakeTalkClick() {
        const bool clicked = talk_clicked_;
        talk_clicked_ = false;
        return clicked;
    }

    Edge BlueDown(int64_t now_us) {
        ++generation_;
        mode_choice_ready_ = 0;
        blue_down_ = true;
        blue_down_us_ = now_us;
        if (chord_) {
            mode_choice_allowed_ = false;
            swallow_blue_ = true;
            return Edge::kNone;
        }
        if (talk_down_ && !talk_started_ && now_us - talk_down_us_ <= kWindowUs)
            return BeginChord(now_us);
        swallow_blue_ = false;  // A fresh blue press on its own acts normally.
        return Edge::kNone;
    }

    void BlueUp(int64_t now_us = 0) {
        blue_down_ = false;
        lock_hold_allowed_ = false;
        FinishChordIfReleased(now_us);
    }

    // Blue click/double/long callbacks ask this first; true means "belongs to
    // a chord, do nothing".
    bool SwallowBlueGesture() const { return swallow_blue_; }

    // Both buttons are down inside a chord. The lock commits only after they
    // have been held, so a brush in a pocket does not lock or unlock.
    bool BlueHeld() const { return blue_down_; }

    bool BothHeld() const { return chord_ && talk_down_ && blue_down_; }

    // The timer rechecks the physical hold and marks it before queuing UI work.
    // A late timer cannot lock a fresh chord before its own hold interval.
    bool CommitLockHold(int64_t now_us) {
        if (!BothHeld() || !lock_hold_allowed_ || lock_committed_ ||
            now_us - chord_down_us_ < kLockHoldUs)
            return false;
        lock_committed_ = true;
        mode_choice_allowed_ = false;
        return true;
    }

    void CancelModeChoice() {
        mode_choice_allowed_ = false;
        lock_hold_allowed_ = false;
    }

    uint32_t TakeModeChoice() {
        const uint32_t ready = mode_choice_ready_;
        mode_choice_ready_ = 0;
        return ready;
    }

    // Recheck on the application owner, including its nested queued callback.
    // Any fresh physical press invalidates a previously released short chord.
    bool ModeChoiceValid(uint32_t generation) const {
        return generation != 0 && generation == generation_ &&
               generation == mode_choice_generation_ && !talk_down_ && !blue_down_;
    }

private:
    Edge BeginChord(int64_t now_us) {
        chord_ = swallow_blue_ = true;
        talk_started_ = false;
        chord_down_us_ = now_us;
        chord_generation_ = generation_;
        mode_choice_allowed_ = true;
        lock_hold_allowed_ = true;
        lock_committed_ = false;
        return Edge::kChord;
    }

    void FinishChordIfReleased(int64_t now_us) {
        if (talk_down_ || blue_down_)
            return;
        if (chord_ && mode_choice_allowed_ && !lock_committed_ &&
            generation_ == chord_generation_ && now_us >= chord_down_us_ &&
            now_us - chord_down_us_ < kLockHoldUs) {
            mode_choice_ready_ = mode_choice_generation_ = generation_;
        }
        chord_ = false;
        mode_choice_allowed_ = false;
    }
    bool talk_down_ = false, talk_started_ = false, talk_clicked_ = false, blue_down_ = false;
    bool chord_ = false, swallow_blue_ = false;
    bool mode_choice_allowed_ = false, lock_hold_allowed_ = false, lock_committed_ = false;
    int64_t talk_down_us_ = 0, blue_down_us_ = 0;
    int64_t chord_down_us_ = 0;
    uint32_t generation_ = 0, chord_generation_ = 0;
    uint32_t mode_choice_ready_ = 0, mode_choice_generation_ = 0;
};

}  // namespace ProvisionsStopWatch

#endif  // PROVISIONS_STOPWATCH_BUTTON_CHORD_H_
