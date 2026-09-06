# Orbit output fence core (disabled)

This is the physical core slice of `output_fence_v1`. It is not included in
CMake, advertised in hello, or connected to Application/AudioService. The
existing firmware behavior is unchanged. The wire is the frozen contract in
`Provisions/docs/orbit-output-fence-v1-contract.md` (native source `39bbd8857`),
with the corrected shared checkpoint fixture copied into
`scripts/tests/fixtures/output_fence_v1.json`. Its hash is
`6b93cb0e6e4e074d494685a44b4b656286edf5f97d85fa2e10b2ba1e4c228903`.

`Core` consumes only acquire, release, and close_commit with exact field sets.
It returns a closed success envelope or an internal Denied/RecoveryRequired
result with no invented wire response. Messages are bounded to 2048 bytes and
one flat JSON object. Integer lexemes, canonical nonzero UUIDs, digest, types,
versions, and receipt bounds are checked before changing state. The checkpoint
hash remains opaque to the device: exact prepared audio/duration is validated
by the gateway, which owns those facts.

The durable phases are:

- OWNED: an exact retry may return acquired only after fresh positive physical
  gate, input closure, output drain, and fallback-disabled evidence.
- DRAINED_PENDING_COMMIT: the immutable phone receipt is durable; the gate
  remains closed. Only the same release can replay drain_pending.
- TERMINAL: the matching backend commit UUID is durable. Only then may the
  physical gate open; the same close_commit replays released. Acquire and
  release retries cannot move this attempt backwards.

A newer owner requires a strictly greater global fence epoch and a different
checkpoint/playback identity. Sequence increases within the same lease; a new
lease may start at 1. An old receipt or close commit cannot clear a newer owner.
NVS compare/write/readback is serialized across adapter instances and rejects
phase regression or changed immutable receipts. Any uncertain write/readback
requires fresh hydration and retains admission blocking while unresolved.

## Persistent API

A single 224-byte little-endian binary manifest uses namespace `orbit_of_v1`,
key `owner`, magic `ORFENC01`. It contains the full acquire identity, phase,
phone terminal receipt, backend commit UUID, reserved zero bytes, and CRC32.
Every write compares the expected prior record, commits, and reads back the
exact bytes. No partition/settings/audio/timer/dictation keys change. An NVS
capacity failure returns recovery; there is no erase-to-make-room behavior.

Missing, corrupt, wrong-device, or unreadable records block all admission.
There is deliberately no absent-record initializer, commissioning API,
operator clear, timeout clear, or abort-unacquired transition. Tests seed known
records directly as fixtures; they do not provide a production commissioning
path. These missing integrations prevent enabling this capability.

## Physical integration requirements

The future runtime must construct/hydrate one core before I/O admission and
invoke it from a serialized worker after authenticating and negotiating the
bound gateway connection. The core checks the configured device UUID; it does
not replace transport authentication or verify backend authority independently.
No hook may run heavy work on ESP_TIMER or reenter the core synchronously.

`BlockAll` must synchronously fence every capture/ordinary/timer/fallback
producer. `Hold` binds the exact persisted owner and requests closure; its
return is never drain evidence. `Observe` must positively establish actual
input-worker closure and decoder/codec/ES8311/DMA drain under that same global
gate. Queuing a stop, a provider finish, lack of traffic, or a dictation close
request is insufficient. The held owner must prevent new I/O after observation.
`OpenAfterTerminal` must atomically compare identity/epoch before opening the
gate. A false result retains the gate and permits exact close-commit retry.

Tests compile the actual parser/core/NVS adapter with ASan/UBSan and inject NVS
faults plus controlled physical hooks. They cover phase retries, malformed
wire, every persisted phase after reboot, missing/corrupt records, NVS
write/commit/readback uncertainty, unknown closure, and concurrent stale
close-commit/new-owner behavior. Physical hooks remain test doubles; global
AudioService producer integration, distributed crash fixtures, commissioning,
and physical microphone/speaker acceptance remain separate gates.
