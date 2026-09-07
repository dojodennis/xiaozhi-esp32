# Matched audio admission hooks

This source slice is guarded by `CONFIG_PROVISIONS_OUTPUT_FENCE_V1`. It does not add Kconfig,
CMake, advertisement, protocol, Core worker, initialization, commissioning or device changes.
The composition owner must finish those integrations and their tests before enabling it.
No claim of acoustic delivery or installed-device behavior follows from host/TU evidence.

## Physical worker boundary

AudioService owns one initially blocked `audio_admission::Gate`, before codec acquisition.
ES8311 allocates channels and registers the real DMA callback at construction but leaves both
channels disabled. Other codecs fail closed through `SupportsOutputFence() == false`.

`BeginAudioFenceClose()` closes the admission generation, seals local input, discards queues,
seals ordinary output, and wakes the actual workers. It does not complete a capture, ordinary
parent, timer preparation, timer recovery, or external work. The input task stops RX and records
its completed generation only after preparation/read/Append callbacks return. The output task
waits for input closure, queue/in-flight settlement, and all six actual TX DMA completions before
stopping TX and publishing its completed generation. A failed stop remains unknown.

`HoldAudioFence(identity, generation)` performs the metadata binding. `GetAudioFenceSnapshot`
combines that exact metadata with supported codec, actual worker closure generations, RX/TX
closure and queues. `OpenAudioFenceAfterTerminal` rechecks those facts plus the exact metadata
CAS. Only the Core adapter may invoke it after a validated durable terminal. An empty queue,
a stopped request, a cleared event bit, or this snapshot alone is not authority to open.

The engine acknowledgement is emitted only where no AFE/wake worker was initialized. Wake,
AFE/realtime upload, debugger feed and audio-test/replay are unsupported in this matched slice;
feature-on testing uses explicit discard, including `EnableAudioTesting(false)`.

## Exact capture parent

Before `VoiceRecorder::Begin` or `BeginDictation`, root obtains `ReserveCaptureParent(press)`.
The returned `CapturePermitPtr` is the same retained, noncopyable object throughout recorder
and upload work. AudioService itself retains it. Dropping a caller's shared pointer does not
release the Gate slot. A rejected physical edge is not retried later; require a fresh press.

`StartLocalRecording(press, permit)` requires that exact object and current physical edge.
The legacy one-argument start cannot open input under this feature. Preparation/read/Append
workers obtain exact child reservations and recheck them after codec/resampler/callback
boundaries. The immediate button release fence remains atomic and discards an in-flight read;
main still must call `SealCaptureInput(permit)`/reconcile the release to request actual RX stop.
An old permit cannot start, seal or retire another parent.

Root reserves each `Encode`, `CaptureWork` or `CaptureUpload` child with `ReserveCaptureWork`
before publishing work to its queue. Keep the original token through actual encode/save,
PCM-clear, callback and cancellation-resistant send completion. Check `CanPublishCaptureWork`
for further publication; use `CompleteCaptureWork` only after actual completion. A failed
save retaining Processing PCM keeps its work/parent held. No record, journal or NVS change is
introduced here.

`GetCaptureClosureSnapshot(permit)` exposes the exact parent, current input generation, source
press, sealed state, completed RX generation, preparation/read-or-Append/encode/recorder/upload
counts. `workers_closed` requires the input and recorder stages settled and RX actually stopped.
It excludes upload children so the serialized terminal receipt may retain its own actual send
reservation; it does not assert that upload bytes or callbacks settled. Root must establish those
facts. `ReleaseCaptureParent` additionally requires zero children including upload. It never
opens the global gate.

`ReserveSealedReplayParent(source_press, optional_source_generation, post_restart_loader)`
requires actual current input quiescence, no owner/work, and allocates a fresh nonzero current
input generation. Same-boot replay requires the real preserved nonzero source press/generation.
Only a positively identified post-restart sealed loader may use source press zero and an absent
historical generation. The optional historical value never supplies current wire generation or
closure evidence. Root preserves immutable metadata and proves loader provenance; receipt
lookup or caller JSON cannot authorize this API. Occupied old outbox records remain unchanged.

## Ordinary output and timers

Root allocates a nonwrapping nonzero per-boot ordinary handle and permanently maps it to the
full immutable wire identity. `BeginOrdinaryOutput(handle)` reserves its exclusive parent.
Only exact `PushOrdinaryPacket`, `SealOrdinaryOutput`, `IsOrdinaryOutputClosed` and retirement
use it. `IsOrdinaryOutputClosed` requires sealing, empty queues/in-flight work, actual DMA drain,
and only that retained parent remaining. Root separately requires zero queued/executing exact
ordinary callback counters and `NotifyPlayer::IsBusy() == false` before retirement. Generic
Notification reservations cannot borrow the ordinary parent. A generic drain event only wakes
this correlated check; it is not delivery proof. Decode failures invoke the matched error
callback while the decoder token/in-flight barrier is still held, before any drain callback.

Before volatile timer preparation, call `ReserveTimerPreparation(exact lease identity)`.
It excludes capture, ordinary output and notification before Player intent is published.
After physical eligibility, `ClaimTimerOutput(owner)` uses that already retained parent.
Only `PushTimerPacket(owner, packet)` may enqueue its media; a generic packet with the same
numeric playback ID cannot borrow it. Release the exact local output owner and then its exact
preparation only after the existing timer state machine establishes its required facts.

On boot/recovery, register the positively validated exact retained timer in Gate before Boot
ACK. `ClaimRetainedTimerOutput` is cleanup-only and cannot admit Decode/Output. Completing
that cleanup token does not retire the durable timer barrier. Root keeps the existing exact
terminal receipt/ACK and identity checks before retiring it.

## Codec calls and locks

Admitted codec input/output calls bind the exact retained token to the actual calling task for
one virtual operation. Plain, wrong-task, reentrant, stale and wrong-kind calls cannot borrow
it. RAII clears the task context on success, error and exceptional host exits. Existing StopWatch
virtual input/output meter callbacks remain inside the retained operation.

The ES8311 vendor open path calls data-interface enable with its device capabilities. This slice
therefore uses input-only or output-only handles instead of IN_OUT. Capture preparation closes
the prior drained output-only handle first; output open cannot implicitly restart RX. Input gain
and output volume are applied only to the admitted direction. Vendor activation marks closure
facts conservatively; actual channel-stop methods establish positive closure. Vendor files are
unchanged.

Outer lock order remains local-recording mutex -> audio-queue mutex -> Gate/token metadata.
Decoder work releases queue ownership around the decoder mutex/codec work and callbacks.
Codec context/data-interface locks are outside queue/Gate locks. Gate never performs codec,
Core, Player, NVS, network or callback work. App scheduling, timer task restrictions, global
main/recorder/notification/boot acknowledgements and authenticated JSON replies remain root work.

## Evidence

`test_provisions_audio_admission.py` exercises the real primitive; the added
`test_provisions_audio_fence_integration.py` compiles the actual AudioService/Codec/ES8311
methods with ASan/UBSan and controlled driver barriers. It covers boot, input preparation/read/
Append, release-only cancellation, retained recorder/upload, optional replay lineage, queue
waits, decoder/encoder cancellation, exact ordinary/timer owners, codec task context, vendor
open direction, and six DMA completions. Codec stubs are controlled hardware observations,
not proof of the installed device. The composition still needs actual recorder/App/protocol
integration checks and a full linked representative build before source acceptance.
