# Orbit alarm repeat diagnostics — 9 September 2026

**This candidate locates the weak-alarm fault; it does not claim to fix audibility.**
The supervised session restored normal a84 voice successfully. Its initial audible
alarm later became absent or faint. The [physical evidence](https://github.com/dojodennis/xiaozhi-esp32/blob/da3f280067eb1fc3fea34c5726289fd25c3abbfc/docs/orbit-schedule-bench-device-2026-09-09/README.md)
is retained without promoting incomplete speaker, apron or restart observations
to acceptance. No device has been touched while preparing these diagnostics.

The candidate starts from the actual tested firmware
`45772685c082a55ef26673da078979a96facdbf6`. It changes only diagnostic accounting in
the isolated hardware-bench build. Clip, gain, PA control, decoder lifecycle,
playback arbitration, alarm cadence, buttons, face and persistent state are unchanged.
Normal firmware compiles the instrumentation out.

## What the next trace tells us

Each accepted embedded clip has a sequence and playback generation. Every start
attempt, including a rejected one, advances an attempt counter. Recorded events
include start, first successful driver write, final DMA drain, cancellation,
replacement, reset and stop. Abort records mark outstanding decode/write work;
an in-flight driver write may finish after cancellation. It is never reported as
absent or as a completed audible reply.

The fixed eight-record queue never blocks playback. Its cumulative loss count is
visible in each consumed event. PCM peak and sum of squares are calculated outside
the audio queue mutex. The application owner consumes at most one event per poll
and logs outside that mutex; audio tasks perform no new serial logging. Diagnostic
logging can still perturb application timing and is not a latency acceptance run.

`decode` is successful packets / attempts; `written`, `drop` and `fail` are driver
output outcomes. `samples` is post-resampler decoded / successfully written PCM.
`peak` and `squares` describe only successfully written samples. RMS is
`sqrt(squares / output_samples)` when samples are nonzero. `t` is the event's
monotonic timestamp, not the later console-print time. A successful driver write
or DMA drain does not prove that the PA and physical speaker were audible.

## Evidence and limits

The exact 1,663-byte exclamation clip decodes into 15 packets. Ten passes through
one host libopus decoder and ten FFmpeg loop/resample passes retain healthy levels.
There is no reproduced silence in those host paths. The existing output-fence
branch is compiled out of the tested image, and the application's drain handler
does not suppress the bench sequencer. [Reproducible host evidence](offline/README.md).
These checks do not exercise the ESP decoder/resampler or the physical codec.

The actual worker/demux/sequencer host harness uses controlled decoder and output
doubles. It verifies ten repeats with a one-second logical gap; second-repeat
decode/write failure and an artificial boundary drop; cancellation during decode
and blocked output; replacement, stop, rejection and diagnostic overflow. The
artificial boundary mismatch is a trace test, not a demonstrated incident cause.
Independent review accepted the corrected diagnostic attribution and bounded queue.
[Review and validation](validation.json), [exact bench build](bench-build.json).

The final bench configuration and dependency lock match the frozen tested profile
exactly. The first local build omitted explicit language/wake-model arguments;
that artifact was rejected, retained privately and never signed or installed.
Use the final manifest, not that earlier artifact. No gain or asset change is
justified by the present evidence.

## Next physical gate

The [signed retest package](signed/README.md) now includes public verification,
an exact application-only installation/restoration proposal and independent GO.
A new supervised window remains required before installation. Preserve the restored
working a84 application and current NVS/capture journal. Existing bench state has
Rice pending acknowledgement and five due alerts; do not erase or reseed it.

During the later approved check, correlate complete observed alarm intervals with
trace generations, write/sample levels, errors and final drain. A healthy software
trace with faint sound points the next investigation toward the device output
chain; missing writes or weak PCM identifies a software boundary. Neither outcome
alone passes apron strength, voice-created timers, hotspot/battery or the 48-hour soak.
