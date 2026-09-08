# Portable service schedule component

This is a locally tested component, with no live protocol, Application, renderer,
audio, button or storage adapter installed. It does not alter the existing timer
player or claim physical alarm delivery. The owning files are `service_schedule.h`
and `service_schedule.cc` directly above this directory.

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

1. An authenticated adapter validates the negotiated, opt-in `provisions` frame,
   current websocket `session_id`, `state: service_schedule_snapshot`, and strict v1
   payload. It rejects unknown/duplicate keys and over-budget parsing before building
   a typed snapshot. Validate IANA zone membership and Unicode control categories
   upstream; the portable module checks only bounded zone syntax and UTF-8/C0/C1
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
5. `ExportState` is a typed persistence seam. No NVS write, readback or backend ACK
   reconciliation is implemented. Atomically save the full snapshot, active states,
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

Still required: strict wire adapter and scope/current-occurrence admission, negotiated
delivery and persistent revision allocation, NVS transaction/readback adapter, clock
source/power-state policy, existing timer/alarm-owner integration, renderer/physical
button mapping, reconciliation, affected firmware build and board acceptance. Host
tests provide no installed-build, acoustic, vibration, disconnect-delivery or battery
endurance evidence. No device was flashed or operated for this component.
