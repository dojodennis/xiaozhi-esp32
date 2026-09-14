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
  → the named face. After a successful answer the gateway may send the exact
  session/turn/request/capture-bound `provisions.capture_consumed` control
  described below. Fence, durable receipt, grant, timer-authority, pong and
  every other frame type are ignored in lite mode (never closing).
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
- **Uploads are never receipts.** Lite sends no durable `capture_receipt`. After a
  successful upload (`listen stop` sent) the entry is kept in flash and marked
  "uploaded, awaiting server receipt" (`MarkLiteUploaded` →
  `VoiceRecorder::MarkUploadedAwaitingReceipt`). The mark is persisted in NVS
  bound to the entry's journal sequence, so it survives a reboot and can never
  attach to a reused slot. Marked entries are not re-offered every 30 s.
- **Successful answers retire exactly one capture.** Only after the complete
  reply has crossed the device socket, the gateway checks framed byte count,
  packet count and SHA-256 against the capture envelope. It then sends
  `{"type":"provisions","state":"capture_consumed","session_id":…,"turn_id":N,"request_id":…,"capture":{…}}`.
  The ring accepts the exact schema and live session only, rechecks all capture
  metadata, stored size and digest on the recorder worker, and removes that one
  ordinary slot. Missing, failed, aborted or mismatched turns send no completion
  and retain the audio. This is not a durable transcript claim. Dictation is
  never accepted by this control.

### Accepted show-time trade-off: bounded slot reuse

The journal has four slots. Exact completion normally releases each slot. The
bounded reuse rule remains a failure fallback for a lost acknowledgement and is
only reached when a new press finds every slot occupied:

- evict exactly one entry: the **oldest** (lowest journal sequence) command
  capture that is in "uploaded, awaiting receipt" state **and** has been so for
  at least **30 minutes** (`kAwaitingReceiptEvictionUs`);
- age is measured against the trusted clock when both the upload mark and the
  new press carry one, otherwise against the monotonic clock since the mark;
  after a reboot without a trusted clock the age is unknown and the entry is
  **not** evictable;
- never a dictation (ORBAUD03) segment, never an entry that was not uploaded,
  never anything younger than the bound;
- every eviction is logged (`Orbit Lite: evicted the oldest
  uploaded-awaiting-receipt capture …`).

This fallback is a deliberate, bounded loss: a note that the Lite gateway
received at least 30 minutes earlier may be erased to make room for a new one.

If no retained slot qualifies for that bounded eviction, authenticated Lite
does not erase any journal data and does not enter a permanent "Couldn't save"
loop. One ordinary Talk capture may instead reuse the recorder's existing
bounded RAM replay buffer and stream directly to the current Lite socket. It is
explicitly marked Live-only: no local-save sound or durable-copy claim is made,
upload cannot create an awaiting marker, dictation is excluded, and a second
capture fails closed while the first RAM replay is still owned by the uploader.
The original four slots remain byte-for-byte unchanged. Full-gateway and
offline behavior retain the durable-journal requirement.

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
- `tts stop` → drain everything held, then Ready when the queue runs dry; a
  following `ready` face updates the pending idle face but never resets a
  decoder that is still draining;
- bounded at 50 frames (~3 s); overflow drops the oldest with a log line;
- a Talk press, socket loss, failed/offline face or a timed-out turn flushes it.

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
banner title. `failed` and `offline` end the turn immediately. `ready` ends an
idle turn, but after `tts stop` the playback-drained event owns completion so
the tail is not cut.

## Failure

- Socket close or a protocol error during a turn takes the existing path:
  OFFLINE face and reconnect. Lite playback is flushed with the socket.
- Reply watchdog: 12 s after the upload with no `tts`/face frame → Ready with
  the failed face, socket kept.
- A `tts start` with no `tts stop` within the existing 35 s TTS deadline →
  Ready with the failed face, socket kept.
- No ping/pong in lite; the heartbeat expiry is disabled. Liveness is the socket.

## Verification

This implementation was ported onto the known-working StopWatch base
`7105d4a`, preserving its timer, display and alarm-stop behavior. On 13
September 2026 it passed:

- the complete 335-test host suite, including Lite hello, jitter/drain,
  interruption, retained-capture, timer and reconnect coverage;
- an ESP-IDF 6.0.2 target build for `m5stack/stopwatch` with
  `bench_profile.json` (`provisions-kitchen-helper-stopwatch`); the application
  uses 0x320000 bytes and leaves 0xd0000 bytes (21%) in its smallest app
  partition;
- the matching gateway's complete test suite, lint and strict source type
  check, with the native Lite compatibility bridge disabled.

## Supervised installation — 13 September 2026

After explicit approval, source
`ee505135a49c5de75aaaa25891253b91c82fa999` was signed with the existing
accepted device key and installed application-only at `0x020000`. The signed
application is 3,280,896 bytes with SHA-256
`cba359b11b1f5d7a224592fe142531436765663e961b57d58a7bc09afeb61862`.
Independent span verification passed, and the complete 16 MiB post-flash image
exactly matched the expected overlay with SHA-256
`1ae8236a5adc33818858b0eaf08e8ae847f124d39589f77d89cbe987ff3a9860`;
every byte outside the application span remained unchanged. The private
pre-flash backup has SHA-256
`e381cb5912e9e0678a5497e1e744b0a77e8c24d5cb61920f145e25c64fc1aa52`.

Orbit booted and authenticated at 17:33:38 UTC with touch present. The matching
gateway revision `6cd34db10db472c72f314b0b93659db795fa3b5d` was then activated with
`ORBIT_LITE=true`, `ORBIT_LITE_VOICE_API=live`, and
`ORBIT_LITE_LEGACY_DEVICE_COMPAT=false`; Orbit reauthenticated in native Lite
mode at 17:37:12 UTC. Gateway health, zero restarts, route codes
`200/405/404/426`, and loopback-only exposure passed the stability gate.

Motor behavior, microphone capture, 24 kHz ES8311 playback quality and
interruption latency remain the focused physical acceptance gates. Exact signed
firmware `7105d4a` and gateway image `orbit-monaco-ba7e00e-amd64` remain the
rollback pair.

## Vibration-free physical stop candidate — 14 September 2026

Physical testing showed that the timer motor mechanically masked spoken stop
commands. Firmware `060caab` now records the authoritative ringing state and,
on a physical Talk press, pauses alarm output before microphone recording can
begin. The existing bounded hold resumes vibration only if the turn settles
without stopping the timer.

All 336 host tests and the ESP-IDF 6.0.2 stopwatch build pass. The externally
signed 3,280,896-byte application has SHA-256
`0203e060acec595de609b716775adfc5d2560a0aad7cbb2fde1fa395f2308e3b` and
verifies with the accepted public-key fingerprint
`09d1e0fd2dfb13640b8a99fcd7e284b6b24e6a06003bce4e3c72a49004364250`.
It is prepared for the established application-only `0x020000` installation;
after the user placed Orbit in download mode, only that signed application was
written. The write-time digest and independent post-read both match the signed
artifact. Before/after comparisons prove the partition table, NVS, OTA/PHY
state, NVS keys and all 32 chunks of the 2 MiB recording journal byte-identical.
No other partition was written or erased. Orbit remains in download mode until
the user performs a normal boot for physical acceptance.

## Ring-only touch dismissal — 14 September 2026

Firmware `5d91d28` initializes the StopWatch CST820 controller with M5Stack's
published reset and status-frame protocol. Touch is sampled only while the
alarm takeover is active. One touch-down immediately clears due timers, stops
the motor and uses the existing authoritative gateway dismissal callback; a
screen touch cannot cancel a timer that is only counting down.

All 337 host tests and the ESP-IDF 6.0.2 stopwatch build pass. The signed
3,280,896-byte application has SHA-256
`d58e51f4ac7a256d7afeec5722e5f4809d2443d89c33d5e2dd33d35968402579`.
After confirming target MAC `28:84:85:44:6b:10`, only this application was
written at `0x020000`. Write-time verification, an independent `verify-flash`
pass and the complete post-read all match the signed artifact. Before/after
comparisons prove the partition table, NVS, OTA/PHY state, NVS keys and all 32
chunks of the 2 MiB recording journal byte-identical. Orbit remains in download
mode pending normal boot and a physical ringing-timer tap test.
