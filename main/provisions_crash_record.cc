#include "provisions_crash_record.h"

#include <esp_attr.h>
#include <esp_debug_helpers.h>
#include <esp_memory_utils.h>
#include <esp_system.h>
#include <nvs.h>
#include <sdkconfig.h>

#include "esp_private/panic_internal.h"
#include "esp_private/system_internal.h"
#if CONFIG_IDF_TARGET_ARCH_XTENSA
#include <esp_cpu_utils.h>
#include <xtensa_context.h>
#endif

namespace provisions::crash {
namespace {

constexpr const char* kNamespace = "diag";
constexpr const char* kRecordKey = "crash";
constexpr const char* kAgeKey = "age";

// Survives the panic reboot (a CPU reset); garbage after power-on, which the
// magic and CRC reject.
RTC_NOINIT_ATTR Record s_rtc;
// Keeps the first record if the panic handler itself faults.
DRAM_ATTR bool s_panic_entered = false;

Record s_last{};
bool s_have_last = false;
uint32_t s_age = 0;
esp_reset_reason_t s_reset = ESP_RST_UNKNOWN;

}  // namespace
}  // namespace provisions::crash

// ResetName() indexes by esp_reset_reason_t.
static_assert(ESP_RST_UNKNOWN == 0 && ESP_RST_POWERON == 1 && ESP_RST_EXT == 2 && ESP_RST_SW == 3 &&
              ESP_RST_PANIC == 4 && ESP_RST_INT_WDT == 5 && ESP_RST_TASK_WDT == 6 && ESP_RST_WDT == 7 &&
              ESP_RST_DEEPSLEEP == 8 && ESP_RST_BROWNOUT == 9 && ESP_RST_SDIO == 10 && ESP_RST_USB == 11 &&
              ESP_RST_JTAG == 12 && ESP_RST_EFUSE == 13 && ESP_RST_PWR_GLITCH == 14 &&
              ESP_RST_CPU_LOCKUP == 15);
static_assert(static_cast<uint32_t>(PANIC_EXCEPTION_DEBUG) == provisions::crash::kKindDebug &&
              static_cast<uint32_t>(PANIC_EXCEPTION_IWDT) == provisions::crash::kKindIwdt &&
              static_cast<uint32_t>(PANIC_EXCEPTION_TWDT) == provisions::crash::kKindTwdt &&
              static_cast<uint32_t>(PANIC_EXCEPTION_ABORT) == provisions::crash::kKindAbort &&
              static_cast<uint32_t>(PANIC_EXCEPTION_FAULT) == provisions::crash::kKindFault);
static_assert(sizeof(provisions::crash::Record) == 72);
static_assert(offsetof(provisions::crash::Record, crc) == 68);

extern "C" void __real_esp_panic_handler(panic_info_t* info);

// Linked in place of esp_panic_handler (-Wl,--wrap=esp_panic_handler), so it
// runs before anything is printed. It only touches IRAM, DRAM, RTC memory and
// always-inline helpers: no heap, I2C, flash writes or logging.
extern "C" IRAM_ATTR __attribute__((no_stack_protector)) void __wrap_esp_panic_handler(panic_info_t* info) {
    using namespace provisions::crash;
    if (info != nullptr && !s_panic_entered) {
        s_panic_entered = true;
        Record record;
        record.magic = kMagic;
        record.cause = 0;
        for (size_t i = 0; i < kBacktraceDepth; ++i) {
            record.bt[i] = 0;
        }
        for (size_t i = 0; i < kWhyBytes; ++i) {
            record.why[i] = '\0';
        }
        // esp_panic_handler only rewrites info->exception for aborts after this
        // point, so read the abort flag here. The task watchdog aborts with the
        // TASK_WDT reset hint set.
        if (g_panic_abort) {
            record.kind = esp_reset_reason_get_hint() == ESP_RST_TASK_WDT ? kKindTwdt : kKindAbort;
        } else {
            record.kind = static_cast<uint32_t>(info->exception);
        }
        record.bt[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(info->addr));
#if CONFIG_IDF_TARGET_ARCH_XTENSA
        const auto* frame = static_cast<const XtExcFrame*>(info->frame);
        if (frame != nullptr) {
            record.cause = static_cast<uint32_t>(frame->exccause);
            record.bt[0] = esp_cpu_process_stack_pc(static_cast<uint32_t>(frame->pc));
        }
#endif
        // Save the PC before touching strings or walking the stack.
        record.crc = RecordCrc(record);
        s_rtc = record;

        const char* why = g_panic_abort ? g_panic_abort_details : info->reason;
        if (why != nullptr) {
            for (size_t i = 0; i + 1 < kWhyBytes && why[i] != '\0'; ++i) {
                record.why[i] = why[i];
            }
        }
#if CONFIG_IDF_TARGET_ARCH_XTENSA
        if (frame != nullptr) {
            esp_backtrace_frame_t walk = {
                .pc = static_cast<uint32_t>(frame->pc),
                .sp = static_cast<uint32_t>(frame->a1),
                .next_pc = static_cast<uint32_t>(frame->a0),
                .exc_frame = frame,
            };
            for (size_t i = 1; i < kBacktraceDepth && walk.next_pc != 0 && esp_stack_ptr_is_sane(walk.sp); ++i) {
                if (!esp_backtrace_get_next_frame(&walk)) {
                    break;
                }
                record.bt[i] = esp_cpu_process_stack_pc(walk.pc);
            }
        }
#endif
        record.crc = RecordCrc(record);
        s_rtc = record;
    }
    __real_esp_panic_handler(info);
}

namespace provisions::crash {

void CaptureAtBoot() {
    s_reset = esp_reset_reason();
    const bool fresh = RecordValid(s_rtc);
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        // Report it from RAM and keep the RTC copy for the next boot.
        if (fresh) {
            s_last = s_rtc;
            s_have_last = true;
            s_age = 0;
        }
        return;
    }

    uint32_t stored_age = 0;
    const bool age_present = nvs_get_u32(handle, kAgeKey, &stored_age) == ESP_OK;
    bool stored = true;
    if (fresh) {
        stored = nvs_set_blob(handle, kRecordKey, &s_rtc, sizeof(s_rtc)) == ESP_OK;
    }
    uint32_t next_age = 0;
    if (stored && NextAge(fresh, age_present, stored_age, &next_age)) {
        stored = nvs_set_u32(handle, kAgeKey, next_age) == ESP_OK;
    }
    stored = nvs_commit(handle) == ESP_OK && stored;

    if (fresh && !stored) {
        s_last = s_rtc;
        s_have_last = true;
        s_age = 0;
        nvs_close(handle);
        return;
    }
    if (fresh) {
        s_rtc.magic = 0;  // Copied; do not count it again.
    }

    Record last{};
    size_t size = sizeof(last);
    s_have_last = nvs_get_blob(handle, kRecordKey, &last, &size) == ESP_OK && size == sizeof(last) &&
                  RecordValid(last);
    if (s_have_last) {
        s_last = last;
    }
    s_age = 0;
    if (nvs_get_u32(handle, kAgeKey, &s_age) != ESP_OK) {
        s_age = 0;
    }
    nvs_close(handle);
}

void AddHelloFacts(cJSON* hardware) {
    AppendFacts(hardware, static_cast<int>(s_reset), s_have_last ? &s_last : nullptr, s_age);
}

}  // namespace provisions::crash
