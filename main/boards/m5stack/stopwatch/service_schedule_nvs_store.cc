#include "service_schedule_nvs_store.h"

#include <nvs.h>
#include <utility>

namespace orbit::service_schedule::storage {
namespace {
constexpr const char* kNamespace = "orbit_sched_v1";
constexpr const char* kKey = "state";
}  // namespace

LoadResult NvsStore::Load(FacePersistentState& output, Bytes& encoded) const {
    nvs_handle_t handle;
    auto status = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (status == ESP_ERR_NVS_NOT_FOUND)
        return LoadResult::Absent;
    if (status != ESP_OK)
        return LoadResult::IoError;
    size_t size = 0;
    status = nvs_get_blob(handle, kKey, nullptr, &size);
    if (status == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return LoadResult::Absent;
    }
    if (status != ESP_OK) {
        nvs_close(handle);
        return LoadResult::IoError;
    }
    if (size == 0 || size > kMaximumRecordBytes) {
        nvs_close(handle);
        return LoadResult::Corrupt;
    }
    Bytes candidate(size);
    status = nvs_get_blob(handle, kKey, candidate.data(), &size);
    nvs_close(handle);
    if (status != ESP_OK || size != candidate.size())
        return LoadResult::IoError;
    FacePersistentState decoded;
    if (Decode(candidate.data(), candidate.size(), scope_, decoded) != CodecResult::Accepted)
        return LoadResult::Corrupt;
    output = std::move(decoded);
    encoded = std::move(candidate);
    return LoadResult::Present;
}

SaveResult NvsStore::Transition(const Bytes* expected, const FacePersistentState& desired,
                                Bytes& verified) const {
    Bytes candidate;
    if (Encode(desired, scope_, candidate) != CodecResult::Accepted)
        return SaveResult::InvalidState;
    FacePersistentState prior;
    Bytes prior_bytes;
    const auto loaded = Load(prior, prior_bytes);
    if (loaded == LoadResult::IoError)
        return SaveResult::IoError;
    if (loaded == LoadResult::Corrupt)
        return SaveResult::Corrupt;
    if (expected ? loaded != LoadResult::Present || *expected != prior_bytes
                 : loaded != LoadResult::Absent)
        return SaveResult::Conflict;
    if (loaded == LoadResult::Present && candidate == prior_bytes) {
        verified = std::move(prior_bytes);
        return SaveResult::Unchanged;
    }
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK)
        return SaveResult::IoError;
    const bool committed =
        nvs_set_blob(handle, kKey, candidate.data(), candidate.size()) == ESP_OK &&
        nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    if (!committed)
        return SaveResult::Uncertain;
    FacePersistentState checked;
    Bytes readback;
    if (Load(checked, readback) != LoadResult::Present || readback != candidate)
        return SaveResult::Uncertain;
    verified = std::move(readback);
    return SaveResult::Saved;
}

}  // namespace orbit::service_schedule::storage
