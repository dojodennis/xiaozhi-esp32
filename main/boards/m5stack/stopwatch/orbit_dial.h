#ifndef PROVISIONS_STOPWATCH_ORBIT_DIAL_H_
#define PROVISIONS_STOPWATCH_ORBIT_DIAL_H_

#include "provisions_timer_snapshot.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ProvisionsStopwatchOrbit {

inline constexpr int kMaximumSlots = 6;
inline constexpr int64_t kCollisionWindowMs = 60'000;
inline constexpr int kDisplaySize = 466;
inline constexpr int kSlotRadius = 52;
// A fingertip is wider than the ring: taps this far from a slot centre still
// count. Neighbouring centres are 153 px apart, so hit circles never overlap.
inline constexpr int kSlotHitRadius = kSlotRadius + 24;
// The CST820 reports half-resolution points: M5Stack's own StopWatch setup
// (M5GFX Touch_CST816S, x_min/y_min 0, x_max/y_max 233, offset_rotation 0)
// stretches 0..233 onto the whole panel on both axes.
inline constexpr int kTouchRawMaximum = 233;

enum class AlarmOutputChange : uint8_t {
    kNone,
    kStart,
    kStop,
};

class AlarmState {
public:
    AlarmOutputChange Update(const std::vector<ProvisionsTimerSnapshot::Timer>& finished_timers);
    void BeginNewSession();
    AlarmOutputChange Reset();
    AlarmOutputChange Silence();

    bool active() const { return active_; }
    bool silenced() const { return silenced_; }

private:
    bool active_ = false;
    bool silenced_ = false;
    std::vector<std::string> finished_ids_;
};

struct Slot {
    bool occupied = false;
    ProvisionsTimerSnapshot::Timer timer;
    int64_t first_seen_ms = 0;
};

struct DialCenter {
    int16_t x = 0;
    int16_t y = 0;
};

class SlotBoard {
public:
    void Update(const std::vector<ProvisionsTimerSnapshot::Timer>& timers, int64_t now_ms);

    const std::array<Slot, kMaximumSlots>& slots() const { return slots_; }
    int overflow_count() const { return overflow_count_; }
    int occupied_count() const;

private:
    std::array<Slot, kMaximumSlots> slots_{};
    int overflow_count_ = 0;
};

std::vector<std::string> CollidingIds(const std::vector<ProvisionsTimerSnapshot::Timer>& timers,
                                      int64_t now_ms);
void PreserveAttention(const std::vector<ProvisionsTimerSnapshot::Timer>& previous,
                       std::vector<ProvisionsTimerSnapshot::Timer>& current);
void LatchDueTimers(std::vector<ProvisionsTimerSnapshot::Timer>& timers, int64_t now_ms);
DialCenter SlotCenter(int index);
// One CST820 raw axis (0..kTouchRawMaximum) as a display pixel
// (0..kDisplaySize - 1), rounded and clamped. Same scale on x and y.
int RawTouchToDisplay(int raw);
// Slot index whose hit circle contains the display point, or -1.
int SlotAtPoint(int x, int y);
// Index in `timers` of the compact face's focus, never `service_index`: the
// pinned timer while it is still running or due and seated on `board` (so it
// has a slot colour and a counting ring), otherwise the soonest running timer
// (a due one only when none runs). A pin that no longer qualifies is cleared.
// Returns -1 when nothing qualifies.
int CompactFocusIndex(const std::vector<ProvisionsTimerSnapshot::Timer>& timers,
                      int service_index, const SlotBoard& board, std::string& pinned_id);
float RemainingFraction(const Slot& slot, int64_t now_ms);
std::string FormatRemaining(int64_t deadline_ms, int64_t now_ms);
std::vector<ProvisionsTimerSnapshot::Timer> FinishedTimers(
    const std::vector<ProvisionsTimerSnapshot::Timer>& timers, int64_t now_ms);

}  // namespace ProvisionsStopwatchOrbit

#endif  // PROVISIONS_STOPWATCH_ORBIT_DIAL_H_
