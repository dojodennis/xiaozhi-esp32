#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace OrbitUsbSelfTest {

// Transport integrity only; firmware authenticity uses the normal RSA signature.
inline uint32_t Checksum(const std::vector<uint8_t>& bytes) {
    uint32_t value = 2166136261U;
    for (uint8_t byte : bytes) {
        value = (value ^ byte) * 16777619U;
    }
    return value;
}

inline void AddRun(std::vector<uint8_t>& out, uint16_t count, uint16_t pixel) {
    out.push_back(count & 255);
    out.push_back(count >> 8);
    out.push_back(pixel & 255);
    out.push_back(pixel >> 8);
}

// Read native little-endian RGB565, respecting LVGL's actual row stride.
inline std::vector<uint8_t> Encode(const uint8_t* pixels, uint32_t width, uint32_t height,
                                   uint32_t stride) {
    std::vector<uint8_t> out;
    if (!pixels || width == 0 || height == 0 || width > 466 || height > 466 || stride < width * 2) {
        return out;
    }
    out.reserve(width * height * 4);
    uint16_t count = 0;
    uint16_t previous = 0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* position = pixels + y * stride + x * 2;
            const uint16_t pixel = position[0] | (uint16_t(position[1]) << 8);
            if (count && (pixel != previous || count == 65535)) {
                AddRun(out, count, previous);
                count = 0;
            }
            previous = pixel;
            ++count;
        }
    }
    AddRun(out, count, previous);
    return out;
}

}  // namespace OrbitUsbSelfTest
