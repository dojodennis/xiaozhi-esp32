#ifndef PROVISIONS_LITE_JITTER_H_
#define PROVISIONS_LITE_JITTER_H_

// Orbit Lite playback jitter buffer.
//
// The thin gateway streams Opus frames as soon as it has them and may race its
// own `tts start`. Stock playback hands every frame straight to the decoder and
// drops what arrives outside the speaking state; on marina Wi-Fi that clips
// the first syllable and stutters on every gap. This buffer sits between the
// socket and the decode queue:
//
//   * frames are held (never dropped for state) until playback is Enabled by
//     the speaking transition, which runs after the decoder reset;
//   * playback starts after kStartFrames frames or kStartDelayMs after the
//     first frame, whichever comes first;
//   * an underrun (sink drained while playing) waits for kResumeFrames;
//   * Stop() drains everything regardless of thresholds; Reset() discards;
//   * the buffer is bounded at kCapacity frames, dropping the oldest.
//
// Time is passed in by the caller (milliseconds, any monotonic origin) and
// the sink is a callback that refuses a frame, leaving it untouched, when its
// own queue is full, so the class is host-testable and free of FreeRTOS.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>

namespace provisions::lite {

template <typename Packet>
class JitterBuffer {
public:
    static constexpr size_t kStartFrames = 3;      // ~180 ms at 60 ms frames
    static constexpr uint32_t kStartDelayMs = 400;
    static constexpr size_t kResumeFrames = 2;
    static constexpr size_t kCapacity = 50;        // ~3 s at 60 ms frames

    enum class State : uint8_t {
        kIdle,         // nothing buffered since the last Reset
        kBuffering,    // frames held until the start threshold
        kPlaying,      // streaming through to the sink
        kRebuffering,  // underrun: waiting for kResumeFrames
        kDraining,     // tts stop seen: release everything, then finish
    };

    struct PushResult {
        size_t released = 0;  // frames handed to the sink by this call
        size_t dropped = 0;   // oldest frames discarded on overflow
    };

    // Sink: consumes the frame (moving from it) and returns true, or returns
    // false without touching it when it cannot take more now.
    using Sink = std::function<bool(Packet&)>;

    // Speaking state reached and the decoder was reset: frames may flow.
    void Enable() {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_ = true;
        if (state_ == State::kIdle)
            state_ = State::kBuffering;
    }

    PushResult Push(Packet packet, uint64_t now_ms, const Sink& sink) {
        std::lock_guard<std::mutex> lock(mutex_);
        PushResult result;
        if (state_ == State::kIdle)
            state_ = State::kBuffering;
        if (frames_.empty())
            first_frame_ms_ = now_ms;
        if (frames_.size() >= kCapacity) {
            frames_.pop_front();
            ++result.dropped;
            ++dropped_total_;
        }
        frames_.push_back(std::move(packet));
        result.released = PumpLocked(now_ms, sink);
        return result;
    }

    // Periodic pump: serves the start delay and refills a sink that was full.
    size_t Pump(uint64_t now_ms, const Sink& sink) {
        std::lock_guard<std::mutex> lock(mutex_);
        return PumpLocked(now_ms, sink);
    }

    // The sink ran dry. Returns true when the turn is finished (draining and
    // nothing left to hand over); the caller then returns to Ready.
    bool OnSinkDrained() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::kDraining)
            return frames_.empty();
        if (state_ == State::kPlaying && frames_.empty())
            state_ = State::kRebuffering;
        return false;
    }

    // tts stop: release what is held, then finish when the sink drains.
    // Returns true when nothing is held (the caller checks the sink itself).
    bool Stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_) {
            // No speaking state was ever reached for this turn: nothing can
            // play, so a stray stop simply clears the race-ahead frames.
            frames_.clear();
            state_ = State::kIdle;
            return true;
        }
        state_ = State::kDraining;
        return frames_.empty();
    }

    // Abort / socket loss / turn end: discard everything.
    void Reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        frames_.clear();
        enabled_ = false;
        state_ = State::kIdle;
        first_frame_ms_ = 0;
    }

    State state() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return frames_.size();
    }
    bool empty() const { return size() == 0; }
    bool enabled() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return enabled_;
    }
    size_t dropped_total() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_total_;
    }

private:
    size_t PumpLocked(uint64_t now_ms, const Sink& sink) {
        if (!enabled_ || frames_.empty())
            return 0;
        if (state_ == State::kBuffering) {
            const bool enough = frames_.size() >= kStartFrames;
            const bool waited = now_ms >= first_frame_ms_ + kStartDelayMs;
            if (!enough && !waited)
                return 0;
            state_ = State::kPlaying;
        } else if (state_ == State::kRebuffering) {
            if (frames_.size() < kResumeFrames)
                return 0;
            state_ = State::kPlaying;
        }
        size_t released = 0;
        while (!frames_.empty()) {
            if (!sink(frames_.front()))
                break;  // the sink refused: the frame stays at the head
            frames_.pop_front();
            ++released;
        }
        return released;
    }

    mutable std::mutex mutex_;
    std::deque<Packet> frames_;
    State state_ = State::kIdle;
    bool enabled_ = false;
    uint64_t first_frame_ms_ = 0;
    size_t dropped_total_ = 0;
};

}  // namespace provisions::lite

#endif  // PROVISIONS_LITE_JITTER_H_
