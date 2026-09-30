#pragma once
#include <cstdint>
extern uint32_t test_time_ms;
inline int64_t esp_timer_get_time() { return int64_t(test_time_ms) * 1000; }
