#ifndef ORBIT_SERVICE_SCHEDULE_NVS_STORE_H_
#define ORBIT_SERVICE_SCHEDULE_NVS_STORE_H_

#include <utility>
#include "service_schedule_storage.h"

namespace orbit::service_schedule::storage {

enum class LoadResult { Absent, Present, Corrupt, IoError };
enum class SaveResult { Saved, Unchanged, InvalidState, Conflict, Corrupt, IoError, Uncertain };
enum class StoreDomain { Live, Bench };

// Synchronous flash adapter for orbit_sched_v1/state (Live) or explicitly
// selected orbit_bench_v1/state (Bench). No arbitrary namespace is accepted.
// Synthetic bench state must never be written into the live schedule. Run on a serialized
// storage worker, never the main event loop or audio task. Publish verified state
// back through Application::Schedule with the caller's generation/ownership
// fence. No worker or runtime hook is implemented here. Does not
// initialize NVS, erase keys/namespaces, reformat storage or touch the capture
// journal / orbit_tmr_v1. Available NVS capacity and flash failure/power-loss
// behavior have not been measured on the board. A <=4096-byte record does not
// guarantee space in the accepted shared 16 KiB NVS partition.
//
// expected=nullptr means previously observed absence; otherwise it is the exact
// bytes returned by Load or a successful Transition. Comparison is NOT a global
// NVS CAS: caller must serialize ALL writes to this namespace on one worker.
// The caller owns admissible semantic transitions, scope admission and rollback
// protection. This adapter validates whole states, not remote authorization.
//
// Any failed/ambiguous set, commit or post-commit readback returns Uncertain:
// storage MAY already contain the desired bytes. Keep the transaction fenced
// and reconcile with Load; never infer that disk remains at expected. Only
// Saved/Unchanged publish verified bytes. Every other result preserves outputs.
class NvsStore {
public:
    explicit NvsStore(Scope enrolled_scope, StoreDomain domain = StoreDomain::Live)
        : scope_(std::move(enrolled_scope)), domain_(domain) {}
    LoadResult Load(FacePersistentState& output, Bytes& encoded) const;
    SaveResult Transition(const Bytes* expected, const FacePersistentState& desired,
                          Bytes& verified) const;

private:
    Scope scope_;
    StoreDomain domain_;
    const char* Namespace() const;
};

}  // namespace orbit::service_schedule::storage
#endif
