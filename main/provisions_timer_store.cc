#include "provisions_timer_store.h"
#include <nvs.h>
namespace provisions::timers {
namespace {
constexpr const char* kNamespace = "orbit_tmr_v1";
constexpr const char* kKey = "attempt";

bool AllowedTransition(const DurableSlot& before, const DurableSlot& after) {
    if (SameDurableSlot(before, after))
        return before.state != DurableState::Empty;
    if (before.state == DurableState::Empty)
        return after.state == DurableState::Prepared && !after.lease_id.empty();
    if (before.state == DurableState::Prepared && before.lease_id == after.lease_id) {
        return after.state == DurableState::NoStartPending ||
               (after.state == DurableState::Alarm && after.record.outcome == Outcome::Unknown &&
                after.record.alarm.lease_id == before.lease_id);
    }
    if (before.state != DurableState::Alarm || after.state != DurableState::Alarm ||
        before.record.outcome != Outcome::Unknown || after.record.outcome == Outcome::Unknown)
        return false;
    return RecordJson({before.record.alarm, Outcome::Unknown}) ==
           RecordJson({after.record.alarm, Outcome::Unknown});
}
}  // namespace
Store::LoadResult NvsStore::Load(DurableSlot& slot) {
    nvs_handle_t handle;
    auto status = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (status == ESP_ERR_NVS_NOT_FOUND) {
        slot = {};
        return LoadResult::Empty;
    }
    if (status != ESP_OK)
        return LoadResult::Fault;
    size_t size = 0;
    status = nvs_get_blob(handle, kKey, nullptr, &size);
    if (status == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        slot = {};
        return LoadResult::Empty;
    }
    if (status != ESP_OK || size == 0 || size > 2048) {
        nvs_close(handle);
        return LoadResult::Fault;
    }
    std::string text(size, '\0');
    status = nvs_get_blob(handle, kKey, text.data(), &size);
    nvs_close(handle);
    if (status != ESP_OK || size != text.size() || !ParseDurableSlot(text, slot))
        return LoadResult::Fault;
    return slot.state == DurableState::Empty ? LoadResult::Empty : LoadResult::Present;
}
bool NvsStore::Transition(const DurableSlot& expected, const DurableSlot& desired) {
    DurableSlot prior;
    const auto found = Load(prior);
    if (found == LoadResult::Fault ||
        (found == LoadResult::Empty ? expected.state != DurableState::Empty
                                    : !SameDurableSlot(prior, expected)) ||
        !AllowedTransition(expected, desired))
        return false;
    const auto text = DurableSlotJson(desired);
    DurableSlot checked;
    if (text.empty() || !ParseDurableSlot(text, checked) || !SameDurableSlot(checked, desired))
        return false;
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK)
        return false;
    const bool saved = nvs_set_blob(handle, kKey, text.data(), text.size()) == ESP_OK &&
                       nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return saved && Load(checked) == LoadResult::Present && SameDurableSlot(checked, desired);
}
bool NvsStore::Erase(const DurableSlot& expected) {
    DurableSlot prior;
    const bool erasable =
        expected.state == DurableState::NoStartPending ||
        (expected.state == DurableState::Alarm && expected.record.outcome != Outcome::Unknown);
    if (!erasable || Load(prior) != LoadResult::Present || !SameDurableSlot(prior, expected))
        return false;
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK)
        return false;
    // A content-free, one-slot EMPTY marker makes an ambiguous readback
    // recoverable: failed set/commit leaves the terminal proof, while failed
    // verification leaves the marker for the next boot to validate. The next
    // preparation atomically overwrites this bounded marker.
    const DurableSlot empty_marker{DurableState::Empty, expected.lease_id, {}};
    const auto text = DurableSlotJson(empty_marker);
    const bool erased = !text.empty() &&
                        nvs_set_blob(handle, kKey, text.data(), text.size()) == ESP_OK &&
                        nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    DurableSlot checked;
    return erased && Load(checked) == LoadResult::Empty && SameDurableSlot(checked, empty_marker);
}
}  // namespace provisions::timers
