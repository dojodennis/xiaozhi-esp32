# Portable service schedule component

**The engineering demo face was rejected as product design.** Preserve these
integration tests; use the evolved iPhone `FreestyleTimerBoardView` for visual and
behavioral parity. No installation of this fixture face is proposed.

The portable scheduler now has a strict wire decoder, a bridge to the existing
dial/alarm transition engine, and a shared LVGL renderer. A separate **synthetic
bench variant** connects them to the real board display and two buttons. The same
renderer and board fonts run in the interactive host demo. No live protocol or
storage adapter is installed, and physical alarm delivery is not claimed.

Run the interactive desktop demo from the repository root:

```sh
python3 main/boards/m5stack/stopwatch/tests/run_service_schedule_demo.py
```

Open the printed localhost link. **Next checkpoint** moves the dinner fixture,
simulates disconnect/reconnect and advances to due times. Countdown seconds also
advance between presses. **Acknowledge alarm** acknowledges exactly one due item;
other simultaneous alarms remain active. **Restart** creates a fresh synthetic
fixture, not a network reset command. An ACK remains pending through reconnect;
only the matching mock receipt clears it. No generic SAVED receipt is shown.

Requires a C/C++ compiler, CMake and the existing managed LVGL/cJSON/board fonts.
Set `ORBIT_CMAKE` or `ORBIT_MANAGED_COMPONENTS` if necessary. On the current host,
`ORBIT_CMAKE=/tmp/orbit-schedule-cmake/cmake/data/bin/cmake` is available. Generate
repeatable real-renderer screenshots and state with `--capture /tmp/orbit-frames`.

Additional validation:

```sh
python3 main/boards/m5stack/stopwatch/tests/test_service_schedule_wire.py -v
python3 main/boards/m5stack/stopwatch/tests/test_service_schedule_demo.py -v
```

The 26 decoder tests use strict C++17 and ASan/UBSan. The eight integration tests
compile the actual LVGL renderer, board fonts, decoder, scheduler and alarm engine;
they verify running countdown pixels, dinner edits, exact ACKs, replay and clock
failure. The normal live timer transport remains separate and is not advertised
as supporting service schedules.

Run from the firmware repository root:

```sh
python3 main/boards/m5stack/stopwatch/tests/test_service_schedule.py -v
```

The runner compiles the actual C++17 source with warnings as errors, AddressSanitizer
and UndefinedBehaviorSanitizer, then runs 25 behavioral cases. It needs a host
`clang++` or `g++` (or `CXX`). The fixture is an unchanged copy of the shared gateway
`orbit_service_schedule_v1.json`: fictitious scope, dinner at 19:00 moved to 19:30,
the linked setup cue moving from 18:00 to 18:30, a fixed 18:00 reminder, and three
independent cooking timers. The tests consume its wire values, not a second deadline
calculation. They assert the independent expected epoch values and observable due,
acknowledgement, edit, disconnection and clock behavior.

## Integration contract

1. `service_schedule_wire.cc` validates the opt-in `provisions` frame,
   current websocket `session_id`, `state: service_schedule_snapshot`, and strict v1
   payload. It rejects unknown/duplicate keys and over-budget parsing before building
   a typed snapshot. Validate IANA zone membership and Unicode control categories
   through mandatory caller-supplied validators; the portable module checks only bounded zone syntax and UTF-8/C0/C1
   controls. It independently validates UUIDs, numeric bounds, uniqueness, total
   item count and the derived linked deadline.
2. Construct `Scheduler` with the enrolled assignment/device, never the incoming
   scope. Serialize `Apply`, `Tick`, `Acknowledge` and persistence on one owner task.
   A snapshot is a complete replacement; omitted items are cancelled. No temporary
   partial schedule is observable on rejection. A clock error also marks clock trust
   invalid, while leaving the last accepted schedule and acknowledgements intact.
3. Use a monotonic, nonnegative millisecond counter for `Apply` and `Tick`.
   Disconnection changes the connection indicator only. Cached timers continue to
   tick without cloud speech. Clock rollback/overflow stops newly due alarms and
   exposes `Invalid`; previously latched alarms remain pending. A newer authenticated
   snapshot with fresh non-regressing server time can restore trust. Variable network
   delay cannot wind the effective clock backwards. This is not RTC calibration or
   an accuracy guarantee across deep sleep; choose a clock that runs through the
   supported power states, or mark restore/recovery untrusted.
4. `items()` provides bounded pending state: `due && !acknowledged`. It does not play
   sound or claim a saved receipt. The existing alarm/output owner retains takeover
   priority; the adapter decides display, sound and haptic arbitration. Obtain a
   deliberate physical acknowledgement and pass the exact `AlarmKey`. Cue keys include
   service occurrence; timer keys deliberately omit it, so an unchanged timer stays
   acknowledged through a service switch. Higher item revisions represent new alarm
   identities. Changed label, deadline or cue kind at the same item revision is rejected.
   Each cue ID belongs to exactly one occurrence. An occurrence switch must supply
   fresh cue IDs; the old IDs are retired so switching back cannot replay an
   acknowledged reminder. Independent timers retain their IDs and acknowledgements.
5. `Scheduler::ExportState` is a typed persistence seam. `FaceModel::ExportState`
   adds the exact pending ACK outbox. The bounded `service_schedule_storage` codec
   and separate `service_schedule_nvs_store` now implement whole-state NVS
   commit/readback. The serialized worker is connected in the separate hardware
   bench; live backend ACK reconciliation is not wired.
   Atomically save the full snapshot, active states,
   last known time and retired IDs, then verify readback before claiming durable
   acknowledgement. Storage errors must remain visible and prevent an unsupported
   durable receipt. Define rollback/recovery of RAM state in the adapter; the portable
   mutation alone is only local state.
6. `Restore` validates a complete state into a fresh instance and starts in
   `AwaitingFreshTime`. A cached identical replay or reconnect cannot re-seed its
   clock. Pending alarms and exact acknowledgements survive; a newer authenticated
   snapshot with fresh time is required to latch additional alarms. Corruption or
   wrong scope must fail visibly. Upstream persistence must protect state integrity
   and prevent loading an older valid snapshot; the portable module cannot detect
   rollback of the entire storage image by itself.

## Bounded history and remaining work

There are at most **64 total active cues/timers plus 64 retired item IDs**. Removing
an ID retires it across subsequent occurrences in the same assignment/device scope;
it cannot be reintroduced with either the same or a higher item revision. This
prevents removed timers being replayed as fresh alarms. Exhausted retirement history
returns `HistoryCapacity` atomically and leaves the accepted schedule running.
An authorized, reconciled reset or durable server history/epoch design is required
for long-term turnover; it is not implemented here. Reconnect, a new occurrence or
an untrusted reset frame must never clear the history or snapshot revision gate.

Still required for **live** use: authenticated scope/current-occurrence admission,
negotiated delivery and durable revision allocation, live binding of the serialized NVS worker,
trusted clock/power-state policy, output-owner composition with the durable audio
timer player, and authoritative receipt reconciliation. The synthetic board mapping
uses the existing visual alarm engine and motor callback; its DummyAudioCodec
does not initialize microphone/speaker hardware. It is not an acoustic test.

The board variant is `provisions-kitchen-helper-stopwatch-schedule-demo`, selected
with `CONFIG_PROVISIONS_SCHEDULE_BENCH_DEMO=y`, local capture off and a distinct
firmware identity. `StartNetwork` is a local no-op. Yellow advances the fixture;
blue acknowledges one alert. Both callbacks enqueue onto the Application owner.
Every face, including alarm takeover, is labelled DEMO. The normal variant defaults
to demo disabled. See `docs/orbit-service-schedule-demo-2026-09-08.md` for the
exact build evidence and physical gate. No board was reset, flashed or operated.

The [9 September persistence and control report](../../../../../docs/orbit-schedule-persistence-2026-09-09.md)
records the six-item/six-pending/64-retired storage bound, failure semantics and
restored capture controls. Run `test_service_schedule_face_persistence.py` and
`test_service_schedule_storage.py` here for the sanitizer-backed recovery checks.
The existing synthetic demo still resets its fixture and does not use this storage.

## Isolated real-speaker bench

`provisions-kitchen-helper-stopwatch-schedule-bench` uses
`hardware_bench_profile.json`, preserving the accepted capture-compatible profile
with an explicit distinct identity. Its worker stores synthetic state only in
`orbit_bench_v1/state`; capture/read/upload and network activity are not started.
Yellow after verified absent storage seeds six short test timers; blue persists
one exact shown ACK. Restore stays NEEDS SYNC. No remote receipt or reset is faked.

Run `test_service_schedule_worker.py`, `test_service_schedule_hardware_bench.py`
and `test_service_schedule_alarm_output.py` for threaded persistence, coordinator
and output regressions. The repository-level
`scripts/tests/test_provisions_hardware_bench_adapter.py` extracts the actual board
methods to exercise render/gesture races and bounded callback admission.

See the [hardware bench report](../../../../../docs/orbit-schedule-hardware-bench-2026-09-09.md)
for output failure semantics, build evidence and the remaining physical gates.
