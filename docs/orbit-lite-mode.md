# Orbit Lite mode

The StopWatch ring can run against the thin "Orbit Lite" gateway. Lite is
selected by the server hello and lives for the life of that socket; the device
hello is unchanged, so the same firmware talks to the full gateway.

Source: `main/protocols/provisions_lite_hello.h`, `main/provisions_lite_jitter.h`,
`main/provisions_lite_face.h`, the `Orbit Lite` section of `main/application.cc`.
Host tests: `scripts/tests/test_provisions_lite_mode.py`.

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
lite-only rules make the recorder work without a full gateway:

- The lite hello installs a fixed capture context (`provisions::lite::kConversationId`)
  because the recorder journals every press under the live context and only
  offers captures whose conversation matches it. Captures made under a full
  gateway keep their own conversation and are never offered to a lite gateway.
- Lite sends no `capture_receipt`. A successful upload is acknowledged locally
  as a durable receipt (`AcknowledgeLiteUpload`), which retires the journal slot
  instead of re-offering it every 30 s. A deferred capture (one that missed its
  own press: reboot, re-offer after a failed upload, superseded press) is
  retired without upload — lite has no durable intake and speaking a stale
  request back out of context would be wrong.

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
