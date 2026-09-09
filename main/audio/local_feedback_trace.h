#ifndef LOCAL_FEEDBACK_TRACE_H_
#define LOCAL_FEEDBACK_TRACE_H_

#include <cstddef>
#include <cstdint>
#include <array>
#include <chrono>
#include <optional>
#include <vector>

// Diagnostic accounting for one embedded clip, used only by the isolated bench.
// The AudioService queue mutex owns every access. No PCM or transcript is kept.
// A successful driver write and DMA drain do not prove acoustic audibility.
struct LocalFeedbackTraceCounts {
    uint64_t sequence = 0;
    uint32_t generation = 0, initial_errors = 0;
    uint32_t decode_attempts = 0, decoded_packets = 0, output_packets = 0;
    uint32_t dropped_packets = 0, failed_writes = 0, peak = 0;
    uint64_t decoded_samples = 0, output_samples = 0, square_sum = 0;
    bool reported = true;
};

struct LocalFeedbackTrace : LocalFeedbackTraceCounts {
    struct Level {
        uint64_t samples = 0, square_sum = 0;
        uint32_t peak = 0;
    };
    struct Event {
        LocalFeedbackTraceCounts counts;
        const char* reason = nullptr;
        int64_t at_ms = 0;
        uint64_t lost_events = 0;
        uint64_t attempt = 0;
        uint32_t errors = 0;
        bool decode_in_flight = false, output_in_flight = false;
    };
    // Fixed diagnostic queue. Overflow is visible; it never blocks playback.
    std::array<Event, 8> events{};
    std::size_t head = 0, count = 0;
    uint64_t lost_events = 0, attempts = 0;

    void Record(const char* reason, uint32_t errors, bool decoding = false, bool writing = false) {
        if (count == events.size()) {
            ++lost_events;
            return;
        }
        events[(head + count++) % events.size()] = {
            *this, reason,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count(),
            lost_events, attempts, errors, decoding, writing};
    }

    std::optional<Event> Pop() {
        if (count == 0)
            return std::nullopt;
        auto result = events[head];
        head = (head + 1) % events.size();
        --count;
        result.lost_events = lost_events;
        return result;
    }

    void Finish(const char* reason, uint32_t errors, bool decoding = false, bool writing = false) {
        if (!reported) {
            Record(reason, errors - initial_errors, decoding, writing);
            reported = true;
        }
    }

    void Reject(const char* reason) {
        ++attempts;
        // Keep an already-active clip intact if a competing start was denied.
        const auto current = static_cast<LocalFeedbackTraceCounts>(*this);
        static_cast<LocalFeedbackTraceCounts&>(*this) = {};
        sequence = current.sequence;
        Record(reason, 0);
        static_cast<LocalFeedbackTraceCounts&>(*this) = current;
    }

    void Start(uint32_t next_generation, uint32_t errors) {
        ++attempts;
        const auto next_sequence = sequence + 1;
        static_cast<LocalFeedbackTraceCounts&>(*this) = {};
        sequence = next_sequence;
        generation = next_generation;
        initial_errors = errors;
        reported = false;
        Record("start", 0);
    }

    void Decoded(uint32_t packet_generation, bool decoded, std::size_t samples) {
        if (reported || packet_generation != generation)
            return;
        ++decode_attempts;
        if (decoded) {
            ++decoded_packets;
            decoded_samples += samples;
        }
    }

    // Measure outside the queue mutex; only constant-size counters are locked.
    static Level Measure(const std::vector<int16_t>& pcm) {
        Level level;
        level.samples = pcm.size();
        for (int16_t sample : pcm) {
            const int32_t value = sample;
            const uint32_t magnitude = value < 0 ? -value : value;
            if (magnitude > level.peak)
                level.peak = magnitude;
            level.square_sum += static_cast<uint64_t>(static_cast<int64_t>(value) * value);
        }
        return level;
    }

    bool Output(uint32_t packet_generation, bool current, bool played, const Level& level) {
        if (reported || packet_generation != generation)
            return false;
        if (!current) {
            ++dropped_packets;
            return false;
        }
        if (!played) {
            ++failed_writes;
            return false;
        }
        const bool first = output_packets == 0;
        ++output_packets;
        output_samples += level.samples;
        if (level.peak > peak)
            peak = level.peak;
        square_sum += level.square_sum;
        if (first)
            Record("first_write", 0);
        return first;
    }
};

#endif
