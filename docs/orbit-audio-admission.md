# Shared audio admission primitive

`main/audio/provisions_audio_admission.h/.cc` supplies bounded synchronized metadata for the
composition owner's global fence adapter. The matched, default-off AudioService hooks are described in
`orbit-audio-fence-integration.md`. The primitive itself is neither physical proof nor
permission to enable the feature. Kconfig/CMake and the global adapter remain integration work. It performs no allocation, NVS, codec, socket,
Core/Player call, callback, or wait for external work under its locks.

`Gate` starts blocked at generation 1 with no owner or acknowledgements. `BeginClose()` closes
an open gate and advances its generation; repeated close requests remain on that same generation.
`Invalidate()` explicitly starts another closure boundary, including while already blocked.
Both preserve outstanding reservations and retained timer identity. Generation exhaustion
permanently blocks admission and acknowledgements; it never wraps.

The adapter converts the complete validated Core identity into the fixed UUID/hash byte arrays
in `FenceIdentity`. `Hold(identity, generation)` binds that exact owner while blocked. A different
owner requires the previous guarded terminal opening, a higher server fence epoch, and the
existing per-lease sequence/new-playback rules. This metadata cannot validate authentication,
durable writes, canonical input text, or physical drain; those remain the caller's responsibility.

Call `Reserve(producer, token)` before producer preparation or I/O. Capture, OrdinaryOutput,
and TimerPreparation are exclusive parents: each requires no active slot, and their presence
rejects all generic fresh producers. Capture descendants must use the exact parent with
InputPreparation/InputRead/Encode/CaptureWork/CaptureUpload; ordinary and timer descendants
admit only Decode/Output. Typed `ReserveCaptureMedia`/`ReserveTimerMedia` reject substitution.
`ReserveMedia` validates the actual parent kind and its closed child allowlist. The 32 slots
count parents and descendants, including completed-parent children until their own completion.
Admission requires the open latch; existing foreground/codec/timer checks still apply.

`SealCaptureInput(parent)` permanently denies new input children and invalidates outstanding
input publication. It preserves admitted encode/recorder/upload work until actual completion.
It is not microphone closure. `Snapshot(parent)` reports the exact retained parent and children;
`IsChild`/`CompleteChild` cannot settle an unrelated token. `AllowsCaptureInput` requires an
actual Capture descendant, so a generic InputRead reservation cannot authorize codec input.
`AllowsPublication(token)` requires the exact current owned slot, current generation and open
latch. A successful check is not a lock held around the subsequent operation: keep the token
outstanding through that operation so concurrent closure cannot report completion.

Each `Reservation` is noncopyable, nonmovable and one-shot. Pin its lifetime through the actual
I/O and callbacks, including cancellation-resistant work. `Complete(token)` explicitly frees
only that token's slot and remains valid after invalidation. The token object stays spent;
it cannot be issued again, and a delayed duplicate completion cannot free later work. Destroying
or losing an outstanding token does not release its slot. The gate must outlive its workers and
token objects. Capacity exhaustion denies new work without evicting another owner.

After each closure boundary, actual owning integrations explicitly acknowledge `Boot`, `Main`,
`Notification`, `Recorder`, and `Engine` with the closure generation. A queued stop or clearing
an event bit is not such an acknowledgement. `Snapshot(identity).metadata_closed` requires the
exact owner, blocked latch, all five current acknowledgements, no active token and no retained
timer. Snapshot has no storage or physical effects. Its no-argument diagnostic form does not
assert owner-matched closure. `OpenAfterTerminal(identity, generation)` compares this metadata
atomically and opens only on an exact match. Invoke it only after the Core's validated durable
terminal plus real codec/DMA evidence; the method establishes neither fact. Completing tokens,
acknowledging boundaries, retiring a timer or observing empty slots never opens automatically.

For a positively validated existing durable timer, `RegisterRetainedTimer(identity, generation)`
must precede that generation's Boot acknowledgement. Its closed identity variant is either
Preparation (lease only, covering Prepared/NoStartPending) or Alarm (lease/playback/timer/
revision/attempt). An exact retry preserves the registration; conflicting identity is refused.
This is not a way to declare a new timer or infer one from absence. `ReserveTimerRecovery` only
claims the already registered exact identity, admits one cleanup token, and never grants
publication permission. Ordinary `Reserve(TimerRecovery)` is forbidden. The original fresh
preparation token cannot be relabelled into cleanup. Token completion preserves the retained
timer barrier. `RetireRetainedTimer` requires its exact identity/current generation and no active
timer preparation/recovery token; call it only after the existing timer's exact durable terminal
ACK. Late old retirement cannot clear a different timer. Neither registration nor retirement
provides server or persistent authority itself.

Lock order inside the primitive is gate metadata mutex → token metadata mutex. No method
acquires an external owner lock. Integrations must keep one documented outer order, must not
call back into Core/Player under these locks, and must retain their reservations across external
work instead of holding metadata locks across it. AudioService now binds its actual input/output and codec workers. Recorder, App callbacks,
notification, timer wire, transport, Core adapter and boot acknowledgements remain explicit
composition-owner integration work; no missing acknowledgement is inferred.

Focused verification compiles the actual source with ASan/UBSan and warnings-as-errors:

```sh
python3 -m unittest discover -s scripts/tests -p test_provisions_audio_admission.py -v
```

The tests cover missing acknowledgements, exact identity/epoch CAS, nonwrapping generations,
idempotent retries, all producer kinds, 32 simultaneous reservations, cross-gate token races,
late old completion/new press, abandoned tokens, capacity, both retained timer kinds, exclusive
parents, exact descendants, sealed input and retained recorder/upload work.
They establish metadata behavior, not physical microphone/speaker acceptance.
