# StopWatch timer consumer (F1)

The local capture profile advertises `features.timers_v1: true`. Timer input and
receipts remain disabled until the authenticated hello contains one boolean
`provisions.timers_v1: true`. Older gateways retain their existing voice behavior.
Timer TTS is handled before ordinary voice `turn_id` validation; it carries the
frozen playback/timer/revision/attempt identity and no `turn_id`.

PostgreSQL remains the timer authority. The device accepts a whole, closed-schema
snapshot of up to 32 active/expired timers, with stable spoken numbers 1–99,
canonical nonzero UUIDs, revisions 1–2147483647, RFC3339 deadlines and labels of
1–80 Unicode scalars / at most 320 UTF-8 bytes. Duplicate IDs/numbers, unknown
fields, invalid values and oversized snapshots reject the entire update. The
round face presents two timers plus a remaining count. A countdown reaching zero
never generates or acknowledges an alarm. Reconnect clears stale presentation
until a new negotiated snapshot arrives.

The WebSocket JSON limit is 32768 bytes, with a 16-level nesting budget before
cJSON parsing. Binary packets remain limited to 2048 bytes. One alarm owns a
bounded 20-packet inbox and the existing decode/playback queues; the gateway
paces one 60 ms Opus packet every 60 ms without catch-up bursts. A burst that
exceeds capacity fails closed. There are at most 500 packets per alarm and a
45-second live-attempt deadline. No new timer audio is admitted while listening,
processing, notification playback, other output, or an unresolved timer owns the
device. Physical press IDs invalidate even a complete tap coalesced before the
main task runs. No timer-button callback performs storage, parsing or codec work.

An alarm includes `lease_id`, `playback_id`, `timer_id`, `timer_revision`, `attempt`,
`audio_sha256`, `packet_count`, its spoken number and label. SHA-256 covers the
concatenation of each two-byte little-endian packet length and exact packet.
The start, sentence_start and stop identities must match. Stop is network end,
not audible completion. Each received binary retains its original transport
session; a delayed callback from an older session cannot attach to a new alarm.

The player reserves a nonwrapping high-half local output ID. Ordinary
notifications use the low half. AudioService checks the reservation at enqueue
and again after output activation, preserving its physical capture fence.
Completion additionally requires every announced packet to be received, hashed,
submitted and observed in the exact codec-write progress sequence. Timer decoding
also requires full packet consumption and exactly 1440 PCM samples before output. The existing
`IsPlaybackIdle()` observes empty queues, no decoder/output work in flight, and
actual ES8311/I2S DMA closure. A missing decode or failed write cannot report
success. Interruption/failure cancels pending output and waits for that same
physical closure.

Persistence uses the distinct `orbit_tmr_v1` NVS namespace, key `attempt`, with a
bounded version-1 record. Existing voice journal/NVS content is untouched. The
initial unknown attempt is committed and read back before audio is queued. The
exact terminal outcome is committed and read back before sending a receipt.
Every terminal outcome—completed, failed or interrupted—has
`output_drained: true`, meaning actual local output closure. Unknown output emits
no receipt and remains fenced. Failed storage, corrupt state or uncertain drain
also retain the fence.

A terminal record is immutable. The device retries the identical receipt until
an exact `drain_ack` includes the current transport session and original lease,
playback, timer, revision and attempt. After reconnect the action becomes
`reconcile_drain` and only transport session framing changes. Erase plus readback
must succeed before the exact local output reservation is released. A rebooted
unknown attempt is closed locally and reconciled as interrupted; it is never
replayed or inferred to have completed. Backend assignment/lease validation and
the global prohibition on new alarms while an older lease is unclosed remain
necessary parts of the integrated contract.

## Validation and remaining acceptance

`test_provisions_timers.py` compiles the production parser, NVS adapter and player
with ASan/UBSan. Its combined bridge runs production AudioService enqueue,
reset, output task, drain observation and owner methods together with production
ES8311 write/DMA callbacks. Completed playback, failed decode, failed output and
interruption remain fenced through five DMA completions and close only after the
sixth. Other cases cover strict wire bounds, whole-snapshot rejection, immutable
storage, storage/erase failures, stale acknowledgements, retries, reboot,
reconnect, old-session binary input and the absence of local countdown alarms.
Platform NVS and codec I/O calls are fault-injected host substitutes; the
vendor Opus decoder and electrical/acoustic output are not emulated by this test.

`scripts/verify_provisions_timer_decoder.py` separately ran the actual packaged
S3 Opus encoder/decoder library in isolated QEMU. Twelve silence, noise and tone
packets all consumed their entire 8–255 encoded bytes and returned 2880 PCM bytes
(1440 mono samples). Library SHA-256:
`4c5d764b3fdcbc75ba727b2cb5db1609363f2eeb4cd62467b74b03234780f3e6`.
This verifies the decoder accounting guard against the packaged implementation;
it does not exercise electrical audio output.

The complete host suite and canonical ESP-IDF 6.0.2 StopWatch bench build must pass
for the final candidate. Physical speaker/microphone behavior, round-display
legibility/font coverage, power-loss behavior on a device and integrated gateway /
PostgreSQL timer delivery still require acceptance against the combined release.
This source change does not install, sign, flash, merge or deploy firmware.
