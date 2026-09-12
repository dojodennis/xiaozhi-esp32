#include "provisions_hardware_facts.h"
#include <atomic>
#include <array>
#include <cstring>
#include <mutex>
namespace provisions::hardware {
namespace {
std::mutex mutex_;
std::array<char, 24> touch_{};
std::atomic<int> mic_ready_ms_{-1};
}  // namespace
void SetTouchProbe(const char* result) {
    if (result == nullptr)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    std::strncpy(touch_.data(), result, touch_.size() - 1);
    touch_[touch_.size() - 1] = '\0';
}
std::string TouchProbe() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::string(touch_.data());
}
void NoteMicReadyMs(int milliseconds) {
    if (milliseconds >= 0 && milliseconds <= 60000)
        mic_ready_ms_.store(milliseconds);
}
int MicReadyMs() { return mic_ready_ms_.load(); }
}  // namespace provisions::hardware
