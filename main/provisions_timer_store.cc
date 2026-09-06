#include "provisions_timer_store.h"
#include <nvs.h>
namespace provisions::timers {
namespace {
constexpr const char* kNamespace = "orbit_tmr_v1";
constexpr const char* kKey = "attempt";
}  // namespace
Store::LoadResult NvsStore::Load(Record& record) {
    nvs_handle_t handle;
    auto status = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (status == ESP_ERR_NVS_NOT_FOUND)
        return LoadResult::Empty;
    if (status != ESP_OK)
        return LoadResult::Fault;
    size_t size = 0;
    status = nvs_get_blob(handle, kKey, nullptr, &size);
    if (status == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return LoadResult::Empty;
    }
    if (status != ESP_OK || size == 0 || size > 2048) {
        nvs_close(handle);
        return LoadResult::Fault;
    }
    std::string text(size, '\0');
    status = nvs_get_blob(handle, kKey, text.data(), &size);
    nvs_close(handle);
    return status == ESP_OK && size == text.size() && ParseRecord(text, record)
               ? LoadResult::Present
               : LoadResult::Fault;
}
bool NvsStore::Save(const Record& record) {
    Record prior;
    const auto found = Load(prior);
    // An immutable terminal cannot be overwritten; never replace a different lease.
    if (found == LoadResult::Fault ||
        (found == LoadResult::Present &&
         (RecordJson({prior.alarm, Outcome::Unknown}) !=
              RecordJson({record.alarm, Outcome::Unknown}) ||
          (prior.outcome != Outcome::Unknown && prior.outcome != record.outcome))))
        return false;
    const auto text = RecordJson(record);
    Record checked;
    if (!ParseRecord(text, checked))
        return false;
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK)
        return false;
    const bool saved = nvs_set_blob(handle, kKey, text.data(), text.size()) == ESP_OK &&
                       nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return saved && Load(checked) == LoadResult::Present && RecordJson(checked) == text;
}
bool NvsStore::Erase(const Record& expected) {
    Record prior;
    if (expected.outcome == Outcome::Unknown || Load(prior) != LoadResult::Present ||
        RecordJson(prior) != RecordJson(expected))
        return false;
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK)
        return false;
    const bool erased = nvs_erase_key(handle, kKey) == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return erased && Load(prior) == LoadResult::Empty;
}
}  // namespace provisions::timers
