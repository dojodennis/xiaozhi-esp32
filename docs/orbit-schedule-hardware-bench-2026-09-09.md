# Isolated schedule hardware bench — 9 September 2026

The timer worker, durable acknowledgments and real speaker path are now connected
in an explicit bench variant. This is a software candidate, not an installed build
or physical acceptance. The accepted phone and Orbit installations are unchanged.

## Behavior

- A dedicated 40 KiB, priority-2 storage thread owns the face model and NVS. One
  command can be queued/in flight, with one immutable publication awaiting the
  application owner. Countdown projections run at most four times per second;
  ordinary clock ticks do not write flash. Due transitions and exact local ACKs
  publish only after whole-state commit and matching readback.
- An uncertain write fences further mutations. Yellow explicitly rereads storage;
  only the exact candidate or expected prior state resolves that uncertainty.
  Recovery restores disconnected, with NEEDS SYNC. It never invents fresh time or
  replays a confirmed receipt. Verified absence permits a new physical seed press.
- The distinct `provisions-kitchen-helper-stopwatch-schedule-bench` variant keeps
  every accepted `bench_profile.json` hardware/security/partition setting, adding
  only explicit capture/bench flags. It uses the real ES8311 path and embedded
  exclamation clip through the existing queued decoder/output service.
- Capture recording, microphone reads/uploads, old remote timer services, Wi-Fi,
  bootstrap and OTA are not started in this mode. This is **not an electrical
  microphone-off guarantee**: the existing duplex codec still configures RX.
- Synthetic state uses **`orbit_bench_v1/state`**. The live schedule namespace,
  playback lease and capture journal are not used by the bench coordinator. No
  erase, automatic reset, history eviction or namespace migration is provided.
- The existing six-seat dial and alarm layout display a permanent BENCH label.
  A blue gesture binds the immutable publication only after the display method
  renders it. One pending gesture takes priority over coalesced ticks; pressure or
  stale input is visible. It never acknowledges whichever different alarm happens
  to be next when a delayed gesture runs.

| Control/state | Bench behavior |
| --- | --- |
| First boot, proven absent storage | Waits for yellow. No automatic fixture or write. |
| Yellow, absent state | Seeds six named synthetic timers, each due after 15 seconds. |
| Blue, due alert | Persists one exact local ACK; other due alerts keep sounding. |
| Restart with saved state | NEEDS SYNC; already-due alerts return, acknowledged ones stay quiet. |
| Yellow, uncertain storage | Explicit readback reconciliation; no blind write retry. |
| Audio fault | Stops and latches AUDIO FAULT; yellow explicitly retries after drain. |
| Haptic I2C fault | Shows HAPTIC FAULT; future motor requests may only attempt LOW. |
| Existing completed fixture | No reset/reseed or fabricated remote acknowledgment. |

The local ACK message means stored on this device. It never means a backend SAVED
receipt. This isolated fixture does not supply authenticated time, reconnect,
remote receipts, conversation, Service edits or repeated acceptance cycles.

## Output and review repairs

The alarm sequencer admits one clip only after actual playback drain, leaves a
one-second gap, and cancels its own clip when the final due alert is acknowledged.
It waits for in-flight/device output to drain before any subsequent start. Decode,
malformed/truncated Ogg and output failures now increment a local-feedback error
counter; an empty queue is not counted as success. Playback or cancellation that
fails to drain within five seconds latches a fault. An unrelated output owner is
never cancelled to make room.

Motor requests target 150 ms on and at least 850 ms off. If application scheduling
stalls, the next poll forces an overdue HIGH low and starts a full off interval.
The physical pulse can still run long during a stall; apron strength, audible
output, I2C timing and electrical behavior require measurement on the board.

Independent review found and closed the pre-render gesture race, a worker stop
notification that could be lost while entering its wait, and an ACK failure notice
that ordinary projections could overwrite. The idle worker now bounds its wait
to 100 ms without making the stop request block. These have executable regressions.

## Evidence and remaining gate

**377 grouped checks pass**: 302 repository checks plus three new board-adapter
regressions, 16 threaded-worker cases, nine coordinator cases, 14 alarm-output
cases, nine face-recovery cases, 16 codec/NVS cases and eight existing LVGL-demo
cases. These include ASan/UBSan and 128 bounded idle-stop cycles. Independent
source and integration reviews are GO.

All four ESP-IDF 6.0.2 variants build: isolated hardware bench, accepted capture,
generic StopWatch and the older synthetic demo. The unsigned hardware-bench app
is **3,080,192 bytes**, leaving 1,048,576 bytes in the app slot; SHA-256
`ecffe7aa30eddcf0e965a1beeeef82326357bbba28057ad418160d9f89ba706c`.

The exact board renderer produced 11 host frames: all 75 visible label bounds
fit the round display without overlap, and repeated full redraws match. View
[six running timers](orbit-schedule-hardware-bench-frames-2026-09-09/02-six-running.png)
and [restored alerts](orbit-schedule-hardware-bench-frames-2026-09-09/06-restored-due-needs-sync.png).
These are renderer evidence, not physical readability acceptance or a new product
design approval. Reproduce with `run_service_schedule_hardware_bench_render.py`
in the board tests directory and `--capture <output-directory>`.

Build/source/dependency hashes and final validation are recorded in the companion
`orbit-schedule-hardware-bench-2026-09-09.json`. The unsigned app is a review artifact;
it has not been signed, sent to a device or used to claim sound/latency acceptance.
The dependency lock is unversioned; its actual candidate hash is pinned without
claiming equality to a historical installed archive.

The 3,567-byte record bound does not prove available space or power-loss behavior
in shared 16 KiB NVS. First physical work still needs an explicit, reviewed
installation action preserving the journal and current recovery path. Then measure
one/six alerts, exact ACKs, restart/recovery, speaker and apron haptics. Real gateway
time/receipt integration, hotspot behavior, battery, uncoached usability and the
48-hour fixed-candidate soak remain open. No serial console, reset, flash, purchase
or human acoustic evidence was produced by this change.
