# Orbit timer recovery and capture controls

9 September 2026. Codex implementation after the capture-compatible bench audit.
This is a firmware preparation milestone. Storage is not wired into the running
board, no new hardware bench mode exists yet, and no device was reset or flashed.

## What changed

`FaceModel` now exports and restores the scheduler and pending acknowledgement
outbox as one state. Restore validates the entire candidate before changing the
model, binds it to the caller's enrolled scope, starts disconnected and enters
`AwaitingFreshTime`. Cached replay cannot refresh time. Already-due alarms remain
due; acknowledged alarms stay silent; a receipt-confirmed UI cue is not replayed.

Pending acknowledgements retain exact identity, revision and cue occurrence when
a newer snapshot edits or removes their item. Receipt reconciliation removes only
the matching pending key and cannot silence the item's newer revision. The pending
key is the retained local history; validation does not authenticate fabricated
history or replace the backend's exact-key authority.

The new board-local storage component encodes a deterministic, versioned binary
record. It supports **six total timer/cue items, six pending acknowledgements and
64 retired IDs**. The maximum legal record is **3,567 bytes**, within its explicit
4,096-byte bound, including maximum labels and timezone. More items fail closed;
there is no truncation or eviction. This bounded bench storage does not change the
existing 64-item wire/scheduler contract. A Service timestamp lives in the snapshot;
linked reminder cues count toward the six stored items.

The record uses packed UUIDs, bounded integers and a checksum, with complete parse
and state validation. CRC32 detects accidental corruption; it provides neither
authentication nor protection against restoring an old valid flash image.

`NvsStore` uses only **`orbit_sched_v1/state`**. It distinguishes absence, corruption
and I/O failure, compares exact expected prior bytes, commits and verifies exact
readback. Any ambiguous write/commit/readback returns `Uncertain`; the caller must
retain its operation fence and reconcile, rather than assume the old state survived.
The adapter never initializes, erases or reformats NVS and never touches the capture
journal or existing `orbit_tmr_v1` playback lease.

Synchronous flash work belongs on a serialized storage worker. A future runtime
adapter must publish the verified result through `Application::Schedule` with its
generation/ownership fence. The NVS wrapper is not a global compare-and-swap and
does not validate semantic transition ordering or grant remote authority.

## Captured regression: blue controls lost during integration

The full host suite exposed that the timer/voice merge retained an older board
initializer and dropped existing capture controls: saved-recording retry from the
documented `a84ab07` lineage and dictation controls from `dc045a6`. The Application
implementations remained present. These handlers are restored only for local
capture, outside the synthetic demo. All blue gestures now check current alarm
state on the application task before retry, dictation or volume changes.

Independent review found that the real alarm remains active after local silence
so its finished timer can stay visible. `SilenceTimerAlarm` now consumes a gesture
only when it actually silences output. A later gesture can reach capture controls
while the quiet finished timer remains visible; a new alarm takes priority again.
The regression test links the actual alarm engine and extracts the real board and
Application handlers. Removing the fix makes the next post-silence gesture fail.

## Verification

- Nine face recovery scenarios pass with AddressSanitizer and UndefinedBehaviorSanitizer.
- Fifteen codec/NVS scenarios pass with the same sanitizers. These include an actual
  edit/retirement workflow, maximum-size roundtrip, every single-byte corruption and
  truncation, invalid payload with repaired checksum, explicit no-space errors,
  writes failing before and after persistence, corrupt/wrong readback and recovery.
  Only the NVS platform calls are substituted; this does not simulate physical flash.
- Existing scheduler (25), wire decoder (26) and actual-LVGL demo (eight) scenarios pass.
- All 301 host build/asset/profile tests pass after restoring the capture controls.
- Independent source review passed face recovery, storage and the real alarm/button
  regression. Touched C++ formatting and diff checks pass.

All three ESP-IDF 6.0.2 board builds pass: generic StopWatch, isolated synthetic
demo and the accepted capture-compatible `bench_profile.json`. The latter compiles
the real NVS adapter, retains local capture, `partitions/provisions/16m.csv`, enabled
validation and external RSA verification. Its unsigned app is 3,080,192 bytes,
leaving 1,048,576 bytes in the smallest application slot. This does not establish
NVS free capacity or turn the build into a new bench mode.

[Exact source, configuration, dependency and binary hashes](orbit-schedule-persistence-2026-09-09.json).
Builds used the reviewed working-tree source before its evidence commit. The
generic build preceded the final Provisions-only silence guard, which is excluded
from the generic branch; demo and capture builds include the reviewed final guard.
No external signing or installation was performed.

## Next integration and physical evidence

Add the serialized runtime persistence owner and explicit storage-failure face,
then connect the default-off capture-compatible bench mode to real codec output,
safe controls and one/six fixtures. Preserve the accepted timer appearance. Never
seed fake fresh time after restoring saved state.

The accepted partition contains **16 KiB shared NVS**. The encoded record limit
does not prove available space, NVS update/garbage-collection overhead or power-loss
behavior on the current board. Those measurements remain required, alongside
real sound, apron haptics, hotspot recovery, restart, runtime and the 48-hour soak.
The current device stays on its documented capture build. External signing and
fresh active-slot/recovery evidence still precede any later installation request.
