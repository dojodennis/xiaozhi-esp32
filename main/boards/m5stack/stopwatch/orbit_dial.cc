#include "sdkconfig.h"

#if CONFIG_PROVISIONS_GATEWAY_REQUIRED
#include "orbit_dial.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ProvisionsStopwatchOrbit {
namespace {

bool Retained(const ProvisionsTimerSnapshot::Timer& timer) {
    return timer.status == ProvisionsTimerSnapshot::TimerStatus::kActive ||
           timer.status == ProvisionsTimerSnapshot::TimerStatus::kAttention;
}

bool DeadlineOrder(const ProvisionsTimerSnapshot::Timer& left,
                   const ProvisionsTimerSnapshot::Timer& right) {
    if (left.deadline_ms != right.deadline_ms) {
        return left.deadline_ms < right.deadline_ms;
    }
    return left.id < right.id;
}

std::vector<ProvisionsTimerSnapshot::Timer> RetainedByDeadline(
    const std::vector<ProvisionsTimerSnapshot::Timer>& timers) {
    std::vector<ProvisionsTimerSnapshot::Timer> retained;
    retained.reserve(timers.size());
    for (const auto& timer : timers) {
        if (Retained(timer)) {
            retained.push_back(timer);
        }
    }
    std::sort(retained.begin(), retained.end(), DeadlineOrder);
    return retained;
}

std::vector<ProvisionsTimerSnapshot::Timer> CollisionCandidates(
    const std::vector<ProvisionsTimerSnapshot::Timer>& timers, int64_t now_ms) {
    std::vector<ProvisionsTimerSnapshot::Timer> active;
    active.reserve(timers.size());
    for (const auto& timer : timers) {
        if (timer.status == ProvisionsTimerSnapshot::TimerStatus::kActive &&
            timer.deadline_ms > now_ms) {
            active.push_back(timer);
        }
    }
    std::sort(active.begin(), active.end(), DeadlineOrder);
    return active;
}

}  // namespace

AlarmOutputChange AlarmState::Update(
    const std::vector<ProvisionsTimerSnapshot::Timer>& finished_timers) {
    std::vector<std::string> current_ids;
    current_ids.reserve(finished_timers.size());
    bool has_new_timer = false;
    for (const auto& timer : finished_timers) {
        current_ids.push_back(timer.id);
        if (std::find(finished_ids_.begin(), finished_ids_.end(), timer.id) ==
            finished_ids_.end()) {
            has_new_timer = true;
        }
    }

    const bool was_active = active_;
    if (current_ids.empty()) {
        active_ = false;
        silenced_ = false;
        finished_ids_.clear();
        return was_active ? AlarmOutputChange::kStop : AlarmOutputChange::kNone;
    }

    active_ = true;
    finished_ids_ = std::move(current_ids);
    if (!was_active || has_new_timer) {
        silenced_ = false;
        return AlarmOutputChange::kStart;
    }
    return AlarmOutputChange::kNone;
}

void AlarmState::BeginNewSession() {
    silenced_ = false;
    finished_ids_.clear();
}

AlarmOutputChange AlarmState::Reset() {
    active_ = false;
    silenced_ = false;
    finished_ids_.clear();
    return AlarmOutputChange::kStop;
}

AlarmOutputChange AlarmState::Silence() {
    if (!active_ || silenced_) {
        return AlarmOutputChange::kNone;
    }
    silenced_ = true;
    return AlarmOutputChange::kStop;
}

void SlotBoard::Update(const std::vector<ProvisionsTimerSnapshot::Timer>& timers, int64_t now_ms) {
    auto retained = RetainedByDeadline(timers);
    const int shown = std::min<int>(retained.size(), kMaximumSlots);
    overflow_count_ = static_cast<int>(retained.size()) - shown;

    std::vector<ProvisionsTimerSnapshot::Timer> chosen(retained.begin(), retained.begin() + shown);
    const auto is_chosen = [&chosen](const std::string& id) {
        return std::any_of(chosen.begin(), chosen.end(),
                           [&id](const auto& timer) { return timer.id == id; });
    };

    for (auto& slot : slots_) {
        if (!slot.occupied) {
            continue;
        }
        if (!is_chosen(slot.timer.id)) {
            slot = Slot{};
            continue;
        }
        const auto refreshed =
            std::find_if(chosen.begin(), chosen.end(),
                         [&slot](const auto& timer) { return timer.id == slot.timer.id; });
        if (refreshed != chosen.end()) {
            slot.timer = *refreshed;
        }
    }

    for (const auto& timer : chosen) {
        const bool already_seated = std::any_of(
            slots_.begin(), slots_.end(),
            [&timer](const Slot& slot) { return slot.occupied && slot.timer.id == timer.id; });
        if (already_seated) {
            continue;
        }
        const auto free_slot = std::find_if(slots_.begin(), slots_.end(),
                                            [](const Slot& slot) { return !slot.occupied; });
        if (free_slot != slots_.end()) {
            free_slot->occupied = true;
            free_slot->timer = timer;
            free_slot->first_seen_ms = now_ms;
        }
    }
}

int SlotBoard::occupied_count() const {
    return static_cast<int>(std::count_if(slots_.begin(), slots_.end(),
                                          [](const Slot& slot) { return slot.occupied; }));
}

std::vector<std::string> CollidingIds(const std::vector<ProvisionsTimerSnapshot::Timer>& timers,
                                      int64_t now_ms) {
    const auto active = CollisionCandidates(timers, now_ms);
    std::vector<std::string> ids;
    for (std::size_t index = 0; index + 1 < active.size(); ++index) {
        if (active[index + 1].deadline_ms - active[index].deadline_ms <= kCollisionWindowMs) {
            if (ids.empty() || ids.back() != active[index].id) {
                ids.push_back(active[index].id);
            }
            ids.push_back(active[index + 1].id);
        }
    }
    return ids;
}

void PreserveAttention(const std::vector<ProvisionsTimerSnapshot::Timer>& previous,
                       std::vector<ProvisionsTimerSnapshot::Timer>& current) {
    for (auto& timer : current) {
        const auto prior =
            std::find_if(previous.begin(), previous.end(), [&timer](const auto& candidate) {
                return candidate.id == timer.id && candidate.deadline_ms == timer.deadline_ms &&
                       candidate.status == ProvisionsTimerSnapshot::TimerStatus::kAttention;
            });
        if (prior != previous.end()) {
            timer.status = ProvisionsTimerSnapshot::TimerStatus::kAttention;
        }
    }
}

void LatchDueTimers(std::vector<ProvisionsTimerSnapshot::Timer>& timers, int64_t now_ms) {
    for (auto& timer : timers) {
        if (timer.status == ProvisionsTimerSnapshot::TimerStatus::kAttention ||
            timer.deadline_ms <= now_ms) {
            timer.status = ProvisionsTimerSnapshot::TimerStatus::kAttention;
        }
    }
}

DialCenter SlotCenter(int index) {
    constexpr float kOrbitRadius = 153.0F;
    constexpr float kCenter = kDisplaySize / 2.0F;
    constexpr float kPi = 3.14159265358979F;
    const float angle = (-90.0F + 60.0F * static_cast<float>(index)) * kPi / 180.0F;
    return DialCenter{
        static_cast<int16_t>(std::lround(kCenter + kOrbitRadius * std::cos(angle))),
        static_cast<int16_t>(std::lround(kCenter + kOrbitRadius * std::sin(angle))),
    };
}

float RemainingFraction(const Slot& slot, int64_t now_ms) {
    const int64_t total = slot.timer.deadline_ms - slot.first_seen_ms;
    if (total <= 0) {
        return 0.0F;
    }
    const int64_t remaining = slot.timer.deadline_ms - now_ms;
    if (remaining <= 0) {
        return 0.0F;
    }
    if (remaining >= total) {
        return 1.0F;
    }
    return static_cast<float>(remaining) / static_cast<float>(total);
}

std::string FormatRemaining(int64_t deadline_ms, int64_t now_ms) {
    int64_t seconds = (deadline_ms - now_ms + 999) / 1000;
    if (seconds < 0) {
        seconds = 0;
    }
    char buffer[32];
    if (seconds < 3600) {
        std::snprintf(buffer, sizeof(buffer), "%lld:%02lld", static_cast<long long>(seconds / 60),
                      static_cast<long long>(seconds % 60));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%lld:%02lld", static_cast<long long>(seconds / 3600),
                      static_cast<long long>((seconds % 3600) / 60));
    }
    return buffer;
}

std::vector<ProvisionsTimerSnapshot::Timer> FinishedTimers(
    const std::vector<ProvisionsTimerSnapshot::Timer>& timers, int64_t now_ms) {
    std::vector<ProvisionsTimerSnapshot::Timer> finished;
    finished.reserve(timers.size());
    for (const auto& timer : timers) {
        if (timer.status == ProvisionsTimerSnapshot::TimerStatus::kAttention ||
            (timer.status == ProvisionsTimerSnapshot::TimerStatus::kActive &&
             timer.deadline_ms <= now_ms)) {
            finished.push_back(timer);
        }
    }
    std::sort(finished.begin(), finished.end(), DeadlineOrder);
    return finished;
}

}  // namespace ProvisionsStopwatchOrbit

#endif  // CONFIG_PROVISIONS_GATEWAY_REQUIRED
