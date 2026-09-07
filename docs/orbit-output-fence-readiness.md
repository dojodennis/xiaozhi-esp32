# Read-only Core readiness

`Core::Snapshot() const` returns `ReadinessSnapshot` with a `Readiness` state and
`std::optional<uint64_t> fence_epoch`. The same Core mutex serializes this read
with explicit `Hydrate()` and command processing. A snapshot performs no store
load, compare/exchange, physical hook, transition, or initialization.

| State | Cached evidence | Epoch |
| --- | --- | --- |
| `Blocked` | Before hydration, invalid configured device, corrupt/faulted storage, or uncertain commit/readback | Absent |
| `Uncommissioned` | Explicit hydration returned `Store::LoadResult::Missing` | Absent |
| `Blocked` | Valid record retained, but a physical hold/closure/open check failed | Retained epoch |
| `RecoveryRequired` | Valid nonterminal record with a successful hold or closure check | Retained epoch |
| `Ready` | Valid terminal record and successful guarded `OpenAfterTerminal` | Retained epoch |

Repeated reads return the cached result. Changing external storage or physical
conditions does not refresh it. Only explicit hydration or an actual Core
command result updates readiness; a malformed or stale command cannot change
it. Failed persistence discards epoch trust until explicit rehydration. Existing
wire replies, durable phases, record formats, and gate-opening rules are unchanged.

This is only the Core portion of admission. The composition owner must also
check synchronized capture, timer, output, and unsettled-work reservations.
Snapshot `Ready` alone cannot enable advertisement or authorize media. Hello
construction must not invoke hydration. There is no missing-state initializer,
commissioning, erase, migration, or device action in this change.

The focused host suite compiles actual Core/parser/NVS methods under ASan/UBSan.
Spies cover repeated reads in every state, storage/physical failure, unknown
commit/readback, exact retries, all durable phases, and synchronization during
a real command transition. Hardware hooks remain fixtures.
