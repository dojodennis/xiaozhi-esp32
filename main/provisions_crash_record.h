#ifndef PROVISIONS_CRASH_RECORD_H_
#define PROVISIONS_CRASH_RECORD_H_

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "cJSON.h"

// Why the ring last restarted, reported in the hello "hardware" object.
//
// A --wrap=esp_panic_handler wrapper writes one Record to RTC memory that
// survives the panic reboot (no heap, I2C, flash or logging there). At the next
// boot CaptureAtBoot() copies a valid record to NVS namespace "diag" (keys
// "crash" and "age") right after nvs_flash_init, and counts boots since that
// record. The gateway allowlists every key AppendFacts() emits; each text value
// matches ^[A-Za-z0-9_.-]{1,40}$ or the hello is rejected.
//
// The diagnostic Orbit profiles also make a starved idle task panic after 60 s
// (CONFIG_ESP_TASK_WDT_PANIC). Only the IDLE tasks are subscribed: nothing in
// this tree calls esp_task_wdt_add, and the main task is deliberately not
// subscribed because Ota::Upgrade and the TLS connect plus 10 s server-hello
// wait in WebsocketProtocol::OpenAudioChannel block it for longer.
namespace provisions::crash {

#if defined(__GNUC__) || defined(__clang__)
#define PROVISIONS_CRASH_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define PROVISIONS_CRASH_ALWAYS_INLINE inline
#endif

inline constexpr uint32_t kMagic = 0x4F524231;  // "ORB1"; bump when Record changes.
inline constexpr size_t kBacktraceDepth = 4;
inline constexpr size_t kWhyBytes = 40;   // Up to 39 characters and a terminator.
inline constexpr size_t kTextMax = 40;    // Gateway limit for one text value.
inline constexpr uint32_t kMaxAge = 10000;
// "%08x.%08x.%08x.%08x" is 35 characters.
inline constexpr size_t kBacktraceTextBytes = 36;

// Values follow panic_exception_t.
enum Kind : uint32_t {
    kKindDebug = 0,
    kKindIwdt = 1,
    kKindTwdt = 2,
    kKindAbort = 3,
    kKindFault = 4,
};

struct Record {
    uint32_t magic;
    uint32_t kind;
    uint32_t cause;  // Xtensa EXCCAUSE of the panicking frame.
    uint32_t bt[kBacktraceDepth];  // bt[0] is the panic PC.
    char why[kWhyBytes];  // Abort details or exception name, head first.
    uint32_t crc;  // CRC-32 of every byte before this field.
};

// Standard reflected CRC-32 (zlib). Always inlined so the panic wrapper never
// calls into flash.
PROVISIONS_CRASH_ALWAYS_INLINE uint32_t Crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

PROVISIONS_CRASH_ALWAYS_INLINE uint32_t RecordCrc(const Record& record) {
    return Crc32(static_cast<const uint8_t*>(static_cast<const void*>(&record)),
                 offsetof(Record, crc));
}

inline bool RecordValid(const Record& record) {
    return record.magic == kMagic && record.crc == RecordCrc(record);
}

// Index is the esp_reset_reason_t value (checked against esp_system.h on device).
inline const char* ResetName(int reason) {
    static constexpr const char* kNames[] = {
        "unknown",   "poweron",  "ext",  "sw",  "panic", "int_wdt", "task_wdt",   "wdt",
        "deepsleep", "brownout", "sdio", "usb", "jtag",  "efuse",   "pwr_glitch", "cpu_lockup",
    };
    if (reason < 0 || static_cast<size_t>(reason) >= sizeof(kNames) / sizeof(kNames[0])) {
        return "unknown";
    }
    return kNames[reason];
}

inline const char* KindName(uint32_t kind) {
    static constexpr const char* kNames[] = {"debug", "iwdt", "twdt", "abort", "fault"};
    return kind < sizeof(kNames) / sizeof(kNames[0]) ? kNames[kind] : "unknown";
}

// Writes "%08x.%08x.%08x.%08x"; returns false when `capacity` is too small.
inline bool FormatBacktrace(const Record& record, char* out, size_t capacity) {
    if (out == nullptr || capacity < kBacktraceTextBytes) {
        if (out != nullptr && capacity > 0) {
            out[0] = '\0';
        }
        return false;
    }
    const int written = std::snprintf(out, capacity, "%08" PRIx32 ".%08" PRIx32 ".%08" PRIx32 ".%08" PRIx32,
                                      record.bt[0], record.bt[1], record.bt[2], record.bt[3]);
    return written == static_cast<int>(kBacktraceTextBytes - 1);
}

inline bool WhyCharAllowed(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '.' || c == '-';
}

// Keeps the last kTextMax characters of `in` (read at most `in_capacity`
// bytes, so an unterminated field stays bounded), maps everything outside
// [A-Za-z0-9_.-] to '_', and always terminates `out`. An empty result means the
// key is omitted.
inline void SanitizeTail(const char* in, size_t in_capacity, char* out, size_t out_capacity) {
    if (out == nullptr || out_capacity == 0) {
        return;
    }
    out[0] = '\0';
    if (in == nullptr) {
        return;
    }
    size_t length = 0;
    while (length < in_capacity && in[length] != '\0') {
        ++length;
    }
    size_t keep = length < kTextMax ? length : kTextMax;
    if (keep > out_capacity - 1) {
        keep = out_capacity - 1;
    }
    const size_t start = length - keep;
    for (size_t i = 0; i < keep; ++i) {
        const char c = in[start + i];
        out[i] = WhyCharAllowed(c) ? c : '_';
    }
    out[keep] = '\0';
}

// Boot counter since the last record. A fresh record restarts it at 0; later
// boots add one until kMaxAge. Returns false when nothing should be written
// (no record was ever stored, or the counter is already saturated).
inline bool NextAge(bool fresh_record, bool age_present, uint32_t stored_age, uint32_t* next) {
    if (next == nullptr) {
        return false;
    }
    if (fresh_record) {
        *next = 0;
        return true;
    }
    if (age_present && stored_age < kMaxAge) {
        *next = stored_age + 1;
        return true;
    }
    return false;
}

// Adds "reset" always, and the crash_* keys when `last` is a stored record.
inline void AppendFacts(cJSON* hardware, int reset_reason, const Record* last, uint32_t age) {
    if (hardware == nullptr) {
        return;
    }
    cJSON_AddStringToObject(hardware, "reset", ResetName(reset_reason));
    if (last == nullptr) {
        return;
    }
    cJSON_AddStringToObject(hardware, "crash_kind", KindName(last->kind));
    char backtrace[kBacktraceTextBytes];
    if (FormatBacktrace(*last, backtrace, sizeof(backtrace))) {
        cJSON_AddStringToObject(hardware, "crash_bt", backtrace);
    }
    char why[kTextMax + 1];
    SanitizeTail(last->why, sizeof(last->why), why, sizeof(why));
    if (why[0] != '\0') {
        cJSON_AddStringToObject(hardware, "crash_why", why);
    }
    cJSON_AddNumberToObject(hardware, "crash_age", age > kMaxAge ? kMaxAge : age);
}

#if defined(ESP_PLATFORM)
// Call once, right after nvs_flash_init() succeeds and before any other task.
void CaptureAtBoot();
// Adds reset and crash facts to the hello "hardware" object.
void AddHelloFacts(cJSON* hardware);
#endif

}  // namespace provisions::crash

#endif  // PROVISIONS_CRASH_RECORD_H_
