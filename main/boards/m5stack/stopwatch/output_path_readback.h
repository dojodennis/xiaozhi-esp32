#ifndef ORBIT_OUTPUT_PATH_READBACK_H_
#define ORBIT_OUTPUT_PATH_READBACK_H_

#include "sdkconfig.h"

#if CONFIG_PROVISIONS_SCHEDULE_HARDWARE_BENCH

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace orbit::service_schedule {

enum class OutputReadStatus : uint8_t {
    Ok,
    NotAttempted,
    Unavailable,
    Closed,
    InvalidPin,
    ReadFailed,
};

struct OutputReadValue {
    OutputReadStatus status = OutputReadStatus::NotAttempted;
    int driver_error = 0;
    uint16_t value = 0;

    bool valid() const { return status == OutputReadStatus::Ok; }
};

struct CodecOutputReadback {
    int requested_volume = -1;
    bool available = false;
    bool open = false;
    OutputReadValue clock_01;
    OutputReadValue clock_02;
    OutputReadValue mute_31;
    OutputReadValue volume_32;
};

struct IoExpanderRegisters {
    OutputReadValue mode;
    OutputReadValue drive;
    OutputReadValue output;
    OutputReadValue input;
};

struct IoExpanderPinReadback {
    uint8_t pin = 0;
    OutputReadValue mode;
    OutputReadValue drive;
    OutputReadValue output;
    OutputReadValue input;
};

IoExpanderPinReadback DecodeIoExpanderPin(uint8_t pin,
                                         const IoExpanderRegisters& registers);

struct OutputPathTrigger {
    int64_t at_ms = 0;
    uint64_t trace_sequence = 0;
    uint64_t attempt = 0;
    uint32_t playback_generation = 0;
};

struct OutputPathSnapshot {
    uint64_t capture_sequence = 0;
    int64_t captured_at_ms = 0;
    OutputPathTrigger trigger;
    uint32_t generation_before = 0;
    uint32_t generation_after = 0;
    CodecOutputReadback codec;
    IoExpanderPinReadback codec_power;
    IoExpanderPinReadback amplifier_enable;

    bool exact_trigger_generation() const {
        return generation_before == generation_after &&
               generation_before == trigger.playback_generation;
    }
};

struct OutputPathReadbackHooks {
    std::function<int64_t()> now_ms;
    std::function<uint32_t()> playback_generation;
    std::function<CodecOutputReadback()> codec;
    std::function<IoExpanderRegisters()> io_expander;
    uint8_t codec_power_pin = 0;
    uint8_t amplifier_enable_pin = 0;
};

// One worker performs the finite-time I2C reads outside the application and
// audio owners. At most four first-write triggers and four results are retained.
class OutputPathReadbackWorker {
public:
    static constexpr size_t kMaximumCaptures = 4;
    static constexpr size_t kResultCapacity = 4;

    struct Stats {
        uint64_t submitted = 0;
        uint64_t completed = 0;
        uint64_t dropped_triggers = 0;
        uint64_t dropped_results = 0;
    };

    explicit OutputPathReadbackWorker(OutputPathReadbackHooks hooks);
    ~OutputPathReadbackWorker();
    OutputPathReadbackWorker(const OutputPathReadbackWorker&) = delete;
    OutputPathReadbackWorker& operator=(const OutputPathReadbackWorker&) = delete;

    bool Submit(OutputPathTrigger trigger);
    std::optional<OutputPathSnapshot> Take();
    Stats stats() const;

private:
    void Run();

    OutputPathReadbackHooks hooks_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::thread worker_;
    bool stopping_ = false;
    std::optional<OutputPathTrigger> pending_;
    std::array<OutputPathSnapshot, kResultCapacity> results_{};
    size_t result_head_ = 0;
    size_t result_count_ = 0;
    Stats stats_;
};

const char* OutputReadStatusName(OutputReadStatus status);

}  // namespace orbit::service_schedule

#endif  // CONFIG_PROVISIONS_SCHEDULE_HARDWARE_BENCH
#endif  // ORBIT_OUTPUT_PATH_READBACK_H_
