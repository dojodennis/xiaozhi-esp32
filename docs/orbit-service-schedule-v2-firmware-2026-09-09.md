# Service schedule v2 firmware: absent Service and owned fresh-clock exchange

Local implementation on firmware baseline `69afde7b1ce454d022fb987f88035d778240d569`.
Gateway contract and exact shared fixture come from `4887498759b31760eddfd0344f02587697d56250`
in `orbit-realtime-feasibility`, `docs/orbit-service-schedule-v2-contract.md`.
This adds portable protocol/model/storage/worker support; it does not advertise a
capability, attach a websocket handler, establish backend authority, or change an
installed device. The frozen `ecffe7aa` bench archive is unchanged.

## Implemented boundary

- V1 snapshot validation, clock behavior and OSS1 encoding-1 bytes are retained.
  V2 snapshots omit wire `server_now_ms`; typed/persisted server time is zero.
  Absence is exactly null occurrence, zero Service revision/deadline, empty zone
  and no cues. A caller's empty occurrence context means positively authorized
  absence, never unknown. Independent timers retain IDs/revisions, latched flags
  and exact ACK keys through Service removal and replacement. Removed cue IDs and
  their pending historical ACKs remain retained.
- The sequence gate remains assignment/device-wide. Moving from v1 to v2 requires
  a newer snapshot. A v2 instance rejects v1 downgrade before and after reboot.
  Encoding 2 explicitly packs an absent occurrence as sixteen zero bytes; the
  decoder never silently converts versions. A v2 record may have last-known time
  zero only without due flags, acknowledged flags or pending ACKs.
- `wire::EncodeClockRequest` and `wire::DecodeClock` implement the closed, separate
  clock envelopes. The response decoder preserves output on any failure and
  retains strict complete parsing, UUID/numeric limits, duplicate/unknown-key
  rejection, escaped-NUL rejection and the 32 KiB raw bound. The gateway's request
  decoder separately caps its small request at 1024 bytes.
- `Scheduler::BeginClockRequest` binds one locally owned nonce to scope, current
  authenticated session and snapshot revision. `AcceptClock` consumes an exact
  response once within 2000 ms. A distinct fresh nonce can replace a lost request
  only after that window; 2000 ms is still occupied, 2001 ms permits replacement.
  Wrong keys cannot consume the rightful request. Exact late/invalid responses
  consume their request without establishing trust. Disconnect, session change,
  newer accepted snapshot, restore and detected rollback/overflow invalidate it.
- V2 snapshots never create clock trust. Already trusted monotonic ticking carries
  through definition edits and disconnection. A fresh response cannot wind a
  running clock backwards or fall below the saved floor when recovering. Restore
  and uncertain-write reconciliation start disconnected and awaiting fresh time.
- The serialized worker has bounded `BeginClockRequest` and `AcceptClock` commands.
  V2 connection events require an explicit valid session; absent/invalid session
  clears the gate and reports `ClockRejected`. Clock acceptance that latches new
  due flags saves the complete schedule/outbox and verifies readback before
  publication. Ordinary ticking or a clock sample without a due transition does
  not write flash. Every store transition failure, including definite IO errors,
  retains the prior publication and fences subsequent commands until explicit
  reconciliation; validation failures before a store call do not enter recovery. A consumed clock nonce remains consumed even when saving fails.
  No clock result fabricates an authoritative ACK receipt.
- The actual LVGL view displays `--:--` for absent Service and never formats epoch
  zero as a Service time. Its existing timer clock-trust display remains intact.

The compact worker/storage admission is **six combined snapshot items**, including
acknowledged items still present, six pending ACKs, and 64 retired UUIDs. The
portable wire/model limit remains 64 combined items. Neither is permission to
truncate or select a subset. The existing 3567-byte worst-case record bound remains
below 4096 bytes; physical NVS free capacity is not established by that proof.

## Verification

Run from the firmware repository root:

```sh
ORBIT_CMAKE=/private/tmp/orbit-schedule-cmake/cmake/data/bin/cmake \
  python3 -m unittest discover \
  -s main/boards/m5stack/stopwatch/tests -p 'test_service_schedule*.py' -v
```

The combined result is **92 unittest tests passed**, recorded with hashes in the
[companion evidence JSON](orbit-service-schedule-v2-firmware-2026-09-09.json).
It includes the existing scheduler, strict v1 wire, face recovery, NVS injection,
real threaded worker, alarm-output/coordinator and real LVGL suites; counts are
contained groups, not additive independent test totals. The v2-specific coverage
includes eleven actual model/codec/clock scenarios, six Python v2 tests with
malformed-frame loops, and four additions within twenty threaded-worker scenarios.
The actual LVGL suite has nine scenarios, including the host-only absent-Service
transition. Its instrumented formatter must receive zero epoch-zero calls, so an
unguarded renderer cannot pass merely because a formatter returns `--:--`.
The [actual local LVGL frame](orbit-service-schedule-v2-absent-2026-09-09.png) and
[frame state](orbit-service-schedule-v2-absent-2026-09-09.json) record that synthetic
rendering; they are not photographs or hardware evidence. C++ portable test targets use AddressSanitizer and UndefinedBehaviorSanitizer;
the existing LVGL CMake target uses its real renderer/fonts with warning checks.

The v1 `orbit_service_schedule_v1_oss1.hex` golden is **315 bytes**, captured by
compiling the actual committed `69afde7` scheduler, wire, face, storage and dial
sources and applying the unchanged first shared v1 fixture at monotonic zero.
The candidate must reproduce these exact bytes. A separate old-renderer negative
control, compiled from `69afde7`, calls the epoch-zero formatter once and renders
its deliberate error sentinel; the candidate calls it zero times. It is not regenerated by the
candidate test. The v2 JSON is an exact byte copy of the gateway fixture.

Meaningful v2 cases include 2000/2001 ms boundaries, missing/lost/duplicate/old
nonces, stale scope/session/revision, negative/regressed monotonic time, epoch
bounds and projection overflow, snapshot/replay/restore invalidation, v1 upgrade
and post-reboot downgrade refusal, absent/present roundtrips, six ACKs plus all 64
retired IDs, every-byte corruption/truncation, delayed storage completion, unchanged
flash-write counts on ordinary clock projection, six newly due timers, one exact
ACK, IO/uncertain candidate/prior reconciliation, and no success publication before
verified persistence.

[Four representative IDF build records](orbit-service-schedule-v2-builds-2026-09-09.json)
cover schedule-demo, hardware-bench, accepted capture-compatible profile and generic
stopwatch using the pinned cached IDF 6.0.2 image, `--network none`, and canonical
`scripts/build.py`. All eleven production source hashes remained unchanged through
all builds; all owned containers were removed and all seven frozen archive file
hashes remained unchanged. No board was opened, reset, flashed or operated.

## Caller duties and remaining integration

The API is not a source of enrollment, current occurrence, durable snapshot
revision, server clock authority or semantic ACK receipts. Before admission, the
caller must authenticate the actual socket, bind enrolled scope and authorized
current selection/absence, negotiate v2, validate IANA/Unicode policy and enforce
its transport-generation fence. Generate unpredictable never-reused request UUIDs;
no durable nonce-history ledger is added to firmware. Bind every queued callback
to that actual socket generation, not only IDs supplied by JSON.

A `ClockRequested` publication allows sending that exact request only while its
socket/owner is still current. Start the monotonic window before worker admission
and socket send, so local queue time cannot make an old response appear fresh;
capture receive time from that same supported monotonic source. Check current
socket ownership again before accepting the response. The worker token additionally
fences owner/snapshot revision, and publication sequence fences UI callbacks.
The dedicated worker continues to own synchronous NVS I/O; no main/audio callback
may perform it. This change adds no scope-reset/erase and does not change existing namespace
selection; callers must explicitly choose the intended live or bench store.
Live snapshot delivery, source/revision authority, precise remote ACK outbox
reconciliation, power-state clock policy and supervised physical acceptance remain
separate gates. This milestone is local behavioral/build proof, not live or acoustic
performance evidence.
