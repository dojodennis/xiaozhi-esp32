# Orbit Lite mode

The StopWatch ring can run against the thin "Orbit Lite" gateway. Lite is
selected by the server hello and lives for the life of that socket; the device
hello is unchanged, so the same firmware talks to the full gateway.

Source: `main/protocols/provisions_lite_hello.h`, `main/provisions_lite_jitter.h`,
`main/provisions_lite_face.h`, the `Orbit Lite` section of `main/application.cc`,
the lite paths of `main/provisions_voice_recorder.cc`.
Host tests: `scripts/tests/test_provisions_lite_mode.py`,
`scripts/tests/test_provisions_lite_preservation.py`.

## Contract

- Device hello: unchanged.
- Server hello may carry `"provisions": {"mode": "lite", "selected": []}`. When
  `mode == "lite"`: `timers_v1` / `timer_claim_recovery_v1` are not required,
  the output fence / receipt / grant machinery is not armed, the hello is not
  rejected, and a `lite` flag is set for the life of the socket. The hello's
  `audio_params` (`sample_rate`, `frame_duration`) drive playback as in stock
  xiaozhi. The gateway sends 24000 Hz mono 60 ms Opus; the ES8311 output on
  the StopWatch is 24 kHz, so this decodes natively (16 kHz is also accepted and
  goes through the existing rate converter). Any other rate or a frame duration
  outside 20/40/60 ms rejects the hello.
- Server → device: `{"type":"stt","text":...}` (optional face text),
  `{"type":"tts","state":"start"}` → speaking, `{"type":"tts","state":"sentence_start","text":...}`
  → face text line, binary Opus frames → playback, `{"type":"tts","state":"stop"}`
  → drain then Ready, `{"type":"provisions","state":"face","face":"<name>","text":"<≤40 chars>"}`
  → the named face. Fence, receipt, grant, timer-authority, pong and every
  other frame type are ignored in lite mode (never rejected, never closing).
- Device → server: on a Talk press the fork's existing abort frame
  `{"session_id":"<server hello session_id, or empty>","type":"abort"}` is sent
  (it is sent on every press, as before), playback stops at once and the jitter
  buffer is flushed; the same hold then records. Listen start/stop and mic
  frames are unchanged (see "Talk path" below for what that means on this build).

## Talk path on the StopWatch build

`provisions-kitchen-helper-stopwatch` has `CONFIG_PROVISIONS_LOCAL_CAPTURE=y`,
so the ring does not stream the microphone. A press records into the local
encrypted journal; the release saves it; the recorder then offers it and the
upload task sends the fork's existing wire sequence on the open socket:

1. `{"session_id":…,"type":"listen","state":"start","mode":"manual","turn_id":N,"request_id":…,"deferred":false,"capture":{…}}`
2. one binary frame per Opus packet (16 kHz mono, 60 ms, protocol version 1: raw payload)
3. `{"session_id":…,"type":"listen","state":"stop","turn_id":N}`

The lite gateway must treat 1 as `listen start` and ignore the extra keys. Two
lite-only rules make the recorder work without a full gateway, and both are
written to preserve recordings (Codex review of 10 September 2026, correction
items 2 and 3):

- **Capture identity.** The lite hello carries a fixed capture context
  (`provisions::lite::kConversationId`). Connecting hands it to
  `VoiceRecorder::UseLiteContext()`, a RAM-only tag for the life of that
  socket: new presses are journalled under it and only matching captures are
  offered (the replay filter itself is unchanged). The stored NVS context
  (`context_v1`), every retained full-gateway recording and the dictation
  journal are left exactly as they were, so they replay again when the full
  gateway returns and re-activates its negotiated context. A factory-fresh ring
  that has never held a full-gateway context cannot journal on lite (the
  journal key is only created under an authenticated negotiated context);
  that is reported as "Capture unavailable", not worked around.
- **Uploads are never receipts.** Lite sends no `capture_receipt`. After a
  successful upload (`listen stop` sent) the entry is kept in flash and marked
  "uploaded, awaiting server receipt" (`MarkLiteUploaded` →
  `VoiceRecorder::MarkUploadedAwaitingReceipt`). The mark is persisted in NVS
  bound to the entry's journal sequence, so it survives a reboot and can never
  attach to a reused slot. Marked entries are not re-offered every 30 s; they
  are removed only by a correlated durable `capture_receipt` (full gateway) or
  by the bounded eviction below. No `durable` receipt is constructed locally
  and the recorder's erase path is never reached from the lite upload. Deferred
  captures (reboot, re-offer, superseded press) are uploaded like any other and
  are never retired without an upload. Dictation segments are never uploaded
  on lite (explicitly refused, kept for the full gateway) and never marked.

### Accepted show-time trade-off: bounded slot reuse

The journal has four slots. Because lite never confirms a save, marked entries
would otherwise accumulate until the ring can no longer record at a show. The
accepted rule, implemented in `VoiceRecorder::EvictForNewCapture()` and only
reached when a new press finds every slot occupied:

- evict exactly one entry: the **oldest** (lowest journal sequence) command
  capture that is in "uploaded, awaiting receipt" state **and** has been so for
  at least **30 minutes** (`kAwaitingReceiptEvictionUs`);
- age is measured against the trusted clock when both the upload mark and the
  new press carry one, otherwise against the monotonic clock since the mark;
  after a reboot without a trusted clock the age is unknown and the entry is
  **not** evictable;
- never a dictation (ORBAUD03) segment, never an entry that was not uploaded,
  never anything younger than the bound; if no entry qualifies the new press
  fails with "Couldn't save" exactly as a full store did before;
- every eviction is logged (`Orbit Lite: evicted the oldest
  uploaded-awaiting-receipt capture …`).

This is a deliberate, bounded loss: a note that the lite gateway received at
least 30 minutes earlier may be erased to make room for a new one. It is the
only path on which lite removes a recording.

### Dictation on lite

Dictation keeps the negotiated full-gateway route (`BeginDictation()` is
unchanged). Lite never negotiates it, so on lite the dictation screen reads
"Dictation needs the full gateway" and the hold does nothing instead of failing
silently. The RAM dictation assignment proof is neither confirmed nor cleared
by a lite socket; it stays for the next full-gateway session.

## Playback

`provisions::lite::JitterBuffer` sits between the socket and the decode queue:

- frames are buffered from the moment they arrive, before `tts start` included;
- `tts start` → Speaking → decoder reset → buffer enabled; playback starts after
  3 frames (~180 ms) or 400 ms after the first frame, whichever is first;
- frames keep flowing into the decode queue (20 frames) as they arrive; a 100 ms
  pump timer serves the start rule and refills the queue when it had no room;
- underrun (decode queue drained mid-stream) → wait for 2 frames, resume;
- `tts stop` → drain everything held, then Ready when the queue runs dry;
- bounded at 50 frames (~3 s); overflow drops the oldest with a log line;
- a Talk press, socket loss, a turn-ending face or a timed-out turn flushes it.

## Faces

| face | render | ring screen |
|---|---|---|
| ready | `SetStatus(idle status)` | READY / HOLD TO TALK (green) |
| working | `SetStatus("Working")` | CHECKING / ONE MOMENT (blue) |
| speaking | `SetStatus("Speaking")` | REPLY / LISTEN (blue) |
| recorded | `ShowNotification("Recorded")` | RECORDED / FROM RECORDS (amber, 3 s) |
| timer | `ShowNotification("TIMER")` | amber banner titled TIMER (closest: no timer screen without `timers_v1`) |
| failed | `ShowNotification("TRY AGAIN")` | amber banner titled TRY AGAIN, then the resting READY (closest: the only red screen says OFFLINE, which a connected turn must not) |
| offline | `SetStatus("Unavailable")` | OFFLINE / TRY AGAIN (red) |

Status faces become the idle status too, so they survive the 1 s status tick
until the next press, `tts start`, turn end or face. Non-empty `text` goes to
the reply surface via `SetChatMessage("assistant", text)` (≤ 40 bytes, control
characters dropped, UTF-8 never split); on the ring it is headed by the last
banner title. `ready`, `failed` and `offline` end the turn: response-pending
clears and any playback still running is cut.

## Failure

- Socket close or a protocol error during a turn takes the existing path:
  OFFLINE face, the PR #3 OFFLINE buzz (`NoteProvisionsOffline`), jittered
  reconnect. Lite playback is flushed with the socket.
- Reply watchdog: 12 s after the upload with no `tts`/face frame → Ready with
  the failed face, socket kept.
- A `tts start` with no `tts stop` within the existing 35 s TTS deadline →
  Ready with the failed face, socket kept.
- No ping/pong in lite; the heartbeat expiry is disabled. Liveness is the socket.

## Unverified (no toolchain on the authoring Mac)

Target compile, the motor, and 24 kHz decode on the ES8311 path are unverified
here; the host suite covers the hello parser, the jitter buffer, the face
mapping and the wiring.
