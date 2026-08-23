#ifndef VOICE_UPLOAD_GATE_H
#define VOICE_UPLOAD_GATE_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <utility>

// A dependency-free upload gate so the physical Talk callback can invalidate
// microphone work without touching the audio engine from the button task.
class VoiceUploadGate {
public:
    uint32_t Open() {
        std::lock_guard<std::mutex> lock(send_lease_mutex_);
        enabled_.store(true, std::memory_order_release);
        return generation_.load(std::memory_order_acquire);
    }

    void Close() {
        // Deny new leases immediately, then wait for an already-authorized send
        // to finish. Re-close under the lease mutex in case Open raced us.
        const bool was_enabled = enabled_.exchange(false, std::memory_order_acq_rel);
        std::lock_guard<std::mutex> lock(send_lease_mutex_);
        const bool reopened = enabled_.exchange(false, std::memory_order_acq_rel);
        if (was_enabled || reopened) {
            generation_.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    bool IsOpen() const {
        return enabled_.load(std::memory_order_acquire);
    }

    uint32_t CurrentGeneration() const {
        return generation_.load(std::memory_order_acquire);
    }

    bool Allows(uint32_t generation) const {
        return enabled_.load(std::memory_order_acquire) &&
               generation == generation_.load(std::memory_order_acquire);
    }

    template <typename Action>
    bool WithSendLease(uint32_t generation, Action&& action) {
        std::lock_guard<std::mutex> lock(send_lease_mutex_);
        if (!Allows(generation)) {
            return false;
        }
        std::forward<Action>(action)();
        return true;
    }

private:
    std::atomic<bool> enabled_{false};
    std::atomic<uint32_t> generation_{0};
    std::mutex send_lease_mutex_;
};

#endif  // VOICE_UPLOAD_GATE_H
