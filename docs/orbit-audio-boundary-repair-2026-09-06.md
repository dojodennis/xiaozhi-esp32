# Orbit firmware audio boundary repair

Source repair for audit A1–A3 and the firmware half of A5, based on installed
`a4376a34cb3ed6b2eef3207c472c16df060819b0` / documentation `b352f76`.
This candidate has not been installed or acoustically accepted.

## Repaired behavior

| Finding | Change | Evidence |
|---|---|---|
| A1: failed ES8311 reads reported successful samples | Disabled input, driver failures and partial sample returns now fail capture. Missing/failed/invalid resampler output also fails instead of being labelled 16 kHz microphone audio. | Actual-method C++ tests with ASan/UBSan inject each failure. |
| A2: previous RX/resampler data entered a new press | The input-owning task restarts RX and resets the resampler for each admitted press. Released or replaced preparation/reads are discarded. | Actual input-task tests cover a new press during preparation and release during a read. IDF 6.0.2 `i2s_channel_enable` resets the RX message queue; the channel start resets the current read position. |
| A3: old remote PCM remained eligible after Talk | The button-side atomic fence immediately revokes old output; main-task startup invalidates the playback generation and clears queued remote/local audio. The output task checks ownership again after output activation. Fresh capture waits for in-flight output and the ES8311 DMA ring to drain. | Actual queue/output/input methods tested together; six DMA completion callbacks are required after the final write, including partial-write failure. |
| A5: final captured samples stayed inside Opus | After the released microphone samples, the encoder consumes 320 zero samples and frame rounding. The 10-second microphone limit, 167-packet limit and journal byte limits are unchanged. | Actual encoder method runs against the packaged ESP32-S3 library in QEMU; a frame-aligned tone ending appears in the additional packet. Exact-sized ASan buffers verify no microphone read beyond release, including the full 160,000 samples. |
| Additional: recorder worker stack was too small | The worker stack grows from 12 KiB to 40 KiB. | The packaged 60 ms VOIP encoder overflows a 16 KiB probe. The actual repaired `Encode` method passes silence, noise and tone fixtures with at least 12,504 bytes spare in the 40 KiB probe, including subsequent decode verification. |
| Additional: write failure could later report notification success | Driver write success is checked. A failure invalidates the exact notification ID before output becomes observably idle; its completion reports failure once. | Actual notification methods reject stale IDs and never turn failure into success on later drain. |

## Input and output contracts

`IsLocalRecordingReady(press)` reports fresh RX/resampler preparation for the
current nonzero press. The display says **Preparing microphone** while waiting,
then **Listening**. Preparation adds no arbitrary warm-up discard. A press during
speaker output must wait for its remaining hardware tail; after 1.5 seconds
without drain it fails capture. Speech made before readiness is not represented
as captured. Measure this delay and the first spoken word on the real device.

`IsLocalRecordingClosed(press)` includes a discarded in-flight input operation.
`IsLocalInputIdle()` additionally rules out a newer active press. These are
observations, not reservations: relay integration must retain its press/route
generation fence when claiming playback. `IsPlaybackIdle()` now includes actual
ES8311 DMA completions. A drain proves output is empty; it does not itself prove
that a particular response played completely. Transport playback IDs, response
digests, cancellation and ownership must still bind any full-readback receipt.

Other codecs fail closed for this new fresh-input operation until they implement
their own preparation. Their inherited output-idle default is not hardware drain
proof. The local recorder remains enabled only for the existing StopWatch target.

## Codec measurement

The pinned `espressif/esp_audio_codec` 2.5.0 S3 archive has SHA-256
`4c5d764b3fdcbc75ba727b2cb5db1609363f2eeb4cd62467b74b03234780f3e6`.
The test intercepts the public Opus constructor/destructor at link time and
queries the encoder actually configured by the ESP wrapper. Production does not
cast or depend on the wrapper's private handle layout.

At 16 kHz, mono, 60 ms, VOIP, complexity 0, DTX/VBR enabled, FEC disabled, the
packaged encoder reports **104 samples / 6.5 ms lookahead**. The
[Opus API](https://opus-codec.org/docs/opus_api-1.5/group__opus__encoderctls.html)
defines this as codec delay. The test requires it to remain within the 320-sample
padding budget, so a dependency/configuration change must rerun the probe. Fresh
gateway decoders and fresh TTS encoders are supplied separately by gateway commit
`1add147`; this firmware patch preserves existing wire and stored-recording formats.

## Reproduction and limits

```sh
python3 -m unittest discover -s scripts/tests -v
python3 scripts/verify_provisions_opus_tail.py --output /private/tmp/orbit-opus-method-probe
docker run --rm --network none -v "$PWD:/workspace" -w /workspace \
  -e CCACHE_DISABLE=1 espressif/idf:v6.0.2 python scripts/build.py \
  m5stack/stopwatch --config bench_profile.json \
  --name provisions-kitchen-helper-stopwatch --language en-US
```

The probe generates a temporary minimal ESP-IDF project and uses the actual
`VoiceRecorder::Encode` body and stack/padding constants from this checkout. It
checks 12 real-codec cases and recovers nonzero final-frame tone energy
(`13,448,194,298` summed squared PCM amplitude). It never opens a microphone,
plays room audio, uses a device credential or connects to a customer backend.

Validation logs are `/private/tmp/orbit-audio-boundary-final-host.log`,
`/private/tmp/orbit-opus-method-evidence.log`,
`/private/tmp/orbit-opus-method-probe/probe.log`, and
`/private/tmp/orbit-audio-boundary-final-build.log`. The full host suite has
177 tests. Formatting is checked only for touched C++ ranges.
All 177 passed; the canonical build and merge-image stage exited successfully.
The unsigned application is `0x2f0000` bytes with `0x100000` bytes slot headroom;
its SHA-256 is `83f4c22f27d96ec5952459abffba858cdb9b8c106de3bee13063df202a98d908`.

The target build checks the StopWatch S3 image and compiles the shared codec and
network components; host fixtures include gateway/local-capture conditional
paths. Other board images and hardware behavior are not claimed verified.
The first build's merge stage encountered one truncated generated Ethernet
object. Rebuilding generated dependencies with ccache disabled completed; no
vendor source was changed.

Remaining physical checks: immediate/repeated presses, interruption during the
last speaker frame, first/last consonants, microphone faults, capture-limit
release, delayed main-task handling, notification failure, and retry/restart of
an existing journal recording. No NVS, journal layout, partition, security,
accepted display profile or persistent identity was changed. No flash, merge or
deployment is part of this repair.


## Talk callback and runtime failure follow-up

The technical owner's corrected video timeline places both black/Starting
transitions about 1.25–1.45 seconds **after release**. This is consistent with
the independently reproduced undersized encoder worker, whose 40 KiB repair
above remains essential. The video does not establish an exception type, fault
address, or the physical reset cause. A capture status previously mapped to
Starting can also misrepresent state; software probes are not reset telemetry.

The StopWatch Talk callbacks now perform only lock-free atomic physical fences
and event-bit publication. Recorder startup/release, protocol interruption,
queue cancellation and UI changes run on the application task. There is no
added fixed delay. A down/up/down sequence while main is busy releases the older
capture and admits only the latest still-held press. Release immediately closes
sample eligibility. Output remains fenced through a complete short tap until
main cancels the old queues; an older reconciliation cannot enable a newer hold.
The physical-held flag is timer-owned, so a delayed recording failure cannot
clear a newer press. Failures can display an error while the button remains held;
local error audio waits until the actual release.

`SetPowerSaveLevel(PERFORMANCE)` on the application task still resets the
45-second display idle timer and restores a dimmed display/backlight. This also
runs for a released short tap or a failed recording startup.

ES8311 runtime allocation, open, gain and volume failures now log the failed
stage, close/delete any incomplete handle, clear input/output eligibility and
turn off the PA instead of aborting. A later activation uses a fresh handle.
Injected failures demonstrate this behavior; they do not prove such a driver
failure happened on the user's unit. Boot hardware construction remains subject
to its existing checks.

The crest now explicitly recognizes capture statuses: Saving, Retry queued and
Preparing microphone use the working state; Saved on Orbit uses idle; Couldn't
save, Capture unavailable, Hold blue to retry and Recording kept use error.
They no longer impersonate Boot/Starting. Unknown strings retain the existing
fallback; this does not expose arbitrary transcript text on the crest.

Additional reproduction:

```sh
python3 scripts/verify_provisions_button_timer.py --output /private/tmp/orbit-button-timer-probe
```

That probe executes the actual board callbacks, `Button` wrappers, physical
Application methods and audio fence methods on real ESP-IDF `ESP_TIMER_TASK`,
with its configured 3,584-byte stack and stack canary, in ESP32-S3 QEMU. All
1,000 down/up cycles complete while main holds its recording control mutex;
minimum timer stack free is 3,432 bytes. The registration seam does not simulate
GPIO electrical behavior or real codec hardware. The host reentrancy test
separately executes actual recording-start/stop methods with startup blocked
while release/new-press callbacks complete. Additional ASan/UBSan tests cover
coalesced output fencing, activation failures, status mapping and dimmed Talk.
The unfinished-read case runs only the production physical release fence before
the read returns, with main stop delayed. Actual main-handler tests also cover
a complete down/up before any startup (zero Begin calls), down/up/down (only
press 2 begins), stale STOP, and duplicate START. The profile assertion isolates
the local-capture preprocessor branch so the nonlocal upload-close branch cannot
mask a missing physical fence.

Follow-up evidence: `/private/tmp/orbit-button-timer-probe/probe.log`,
`/private/tmp/orbit-timer-split-integration.log`,
`/private/tmp/orbit-timer-split-final-host.log` and
`/private/tmp/orbit-timer-split-final-build.log`.

All 180 follow-up host tests pass. The canonical build and merged-image stage
exit successfully. The follow-up unsigned application remains `0x2f0000` bytes
with `0x100000` bytes slot headroom; its SHA-256 is
`54ca29f2ca800b7e67cbb6e8504ba514e7e9141dddc9b6b24c71fdce5e819b17`.
No device installation or acoustic acceptance was performed.
