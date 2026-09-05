#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace OrbitCrest {

struct AudioMeter {
    std::atomic<uint32_t> mean_absolute{0};
    std::atomic<uint32_t> sampled_ms{0};

    void Observe(const int16_t* samples, std::size_t size, uint32_t now_ms) {
        uint32_t total = 0;
        uint32_t count = 0;
        const std::size_t stride = std::max<std::size_t>(1, (size + 127) / 128);
        // At most 128 samples, no copies/allocations/locks or PCM mutation.
        for (std::size_t index = 0; index < size; index += stride) {
            const int value = samples[index];
            total += value < 0 ? -value : value;
            ++count;
        }
        mean_absolute.store(count == 0 ? 0 : total / count, std::memory_order_relaxed);
        sampled_ms.store(now_ms, std::memory_order_release);
    }
};

inline AudioMeter input_meter;
inline AudioMeter output_meter;

}  // namespace OrbitCrest
