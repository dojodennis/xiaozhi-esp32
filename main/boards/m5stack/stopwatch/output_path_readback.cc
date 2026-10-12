#include "output_path_readback.h"

#if CONFIG_PROVISIONS_SCHEDULE_HARDWARE_BENCH

#include <utility>

namespace orbit::service_schedule {
namespace {
OutputReadValue PinValue(uint8_t pin, const OutputReadValue& reg) {
    if (pin >= 14)
        return {OutputReadStatus::InvalidPin, 0, 0};
    if (!reg.valid())
        return {reg.status, reg.driver_error, 0};
    return {OutputReadStatus::Ok, 0,
            static_cast<uint16_t>((reg.value >> pin) & uint16_t{1})};
}
}  // namespace

IoExpanderPinReadback DecodeIoExpanderPin(uint8_t pin,
                                         const IoExpanderRegisters& registers) {
    return {pin, PinValue(pin, registers.mode), PinValue(pin, registers.drive),
            PinValue(pin, registers.output), PinValue(pin, registers.input)};
}

const char* OutputReadStatusName(OutputReadStatus status) {
    switch (status) {
        case OutputReadStatus::Ok:
            return "ok";
        case OutputReadStatus::NotAttempted:
            return "not_attempted";
        case OutputReadStatus::Unavailable:
            return "unavailable";
        case OutputReadStatus::Closed:
            return "closed";
        case OutputReadStatus::InvalidPin:
            return "invalid_pin";
        case OutputReadStatus::ReadFailed:
            return "read_failed";
    }
    return "unknown";
}

OutputPathReadbackWorker::OutputPathReadbackWorker(OutputPathReadbackHooks hooks)
    : hooks_(std::move(hooks)), worker_([this] { Run(); }) {}

OutputPathReadbackWorker::~OutputPathReadbackWorker() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_one();
    if (worker_.joinable())
        worker_.join();
}

bool OutputPathReadbackWorker::Submit(OutputPathTrigger trigger) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || stats_.submitted >= kMaximumCaptures || pending_) {
        ++stats_.dropped_triggers;
        return false;
    }
    pending_ = trigger;
    ++stats_.submitted;
    ready_.notify_one();
    return true;
}

std::optional<OutputPathSnapshot> OutputPathReadbackWorker::Take() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (result_count_ == 0)
        return std::nullopt;
    auto result = results_[result_head_];
    result_head_ = (result_head_ + 1) % results_.size();
    --result_count_;
    return result;
}

OutputPathReadbackWorker::Stats OutputPathReadbackWorker::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void OutputPathReadbackWorker::Run() {
    for (;;) {
        OutputPathTrigger trigger;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
            if (stopping_)
                return;
            trigger = *pending_;
            pending_.reset();
        }

        OutputPathSnapshot snapshot;
        snapshot.trigger = trigger;
        if (hooks_.playback_generation)
            snapshot.generation_before = hooks_.playback_generation();
        if (hooks_.codec)
            snapshot.codec = hooks_.codec();
        const auto registers = hooks_.io_expander ? hooks_.io_expander()
                                                  : IoExpanderRegisters{};
        snapshot.codec_power = DecodeIoExpanderPin(hooks_.codec_power_pin, registers);
        snapshot.amplifier_enable =
            DecodeIoExpanderPin(hooks_.amplifier_enable_pin, registers);
        if (hooks_.playback_generation)
            snapshot.generation_after = hooks_.playback_generation();
        snapshot.captured_at_ms = hooks_.now_ms ? hooks_.now_ms() : 0;

        std::lock_guard<std::mutex> lock(mutex_);
        snapshot.capture_sequence = ++stats_.completed;
        if (result_count_ == results_.size()) {
            ++stats_.dropped_results;
            continue;
        }
        results_[(result_head_ + result_count_) % results_.size()] = snapshot;
        ++result_count_;
    }
}

}  // namespace orbit::service_schedule

#endif  // CONFIG_PROVISIONS_SCHEDULE_HARDWARE_BENCH
