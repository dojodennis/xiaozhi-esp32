# Orbit offline capture

## Storage foundation

The StopWatch has a bounded journal for four recordings of up to 167 Opus packets
(16 kHz mono, 60 ms, at most 2048 bytes per packet). It stores the original request
and conversation IDs, the exact prior answer ID/revision, capture time when known,
and framed audio. It never evicts
an older recording to make room. Full storage, malformed audio, unavailable keys,
and flash failures return explicit failure results.

The final 2 MiB of the existing 8 MiB assets partition holds four 512 KiB slots.
The accepted assets occupy 2,694,681 bytes. The partition table and accepted crest
remain unchanged. The adapter checks every packed asset extent before accessing
the reserved tail; the default asset builder enforces the 6 MiB boundary. Other
asset modes and runtime asset downloads are disabled for this profile so they
cannot erase saved recordings.

Each record uses AES-256-GCM through the ESP-IDF 6 PSA API. IDs, lengths, sequence,
codec details, time, prior answer reference and nonce are authenticated alongside
encrypted audio. Format v2 has a 124-byte header; unsupported versions remain
quarantined. The earlier v1 storage-only checkpoint was never instantiated or
installed, so there are no deployed v1 recordings to migrate. A
separate NVS key survives credential rotation. First creation requires an explicit
authenticated-context decision and an entirely erased journal region. Existing
keys open offline. A committed, independently read-back NVS counter supplies
unique nonces; missing, failed or exhausted counters stop new saves. Startup never
automatically erases this profile's NVS on initialization errors.

Ciphertext is written before the header. A save succeeds only after authenticated
read-back and exact comparison. Interrupted records remain quarantined. A matching
server receipt may erase only its exact request ID and conversation ID; a stale
receipt cannot erase a subsequently reused slot. Flash and crypto work must run
on a dedicated worker. The two large scratch buffers are allocated once in PSRAM.

This is application-level audio encryption. The reversible bench configuration
has no flash or NVS encryption, so a physical full-flash dump also contains the
key. This change enables no irreversible security flags.

## Device capture and replay

The StopWatch profile now instantiates the journal. Its physical Talk edges own
raw microphone capture independently of Wi-Fi: two preallocated PCM buffers each
hold up to ten seconds of 16 kHz mono audio. The input task copies at most ten
milliseconds per callback. Releasing a press freezes its buffer while the next
press can use the other buffer. Overflow, an empty press or unavailable storage
reports failure; it never acknowledges a partial recording as saved.

A dedicated worker encodes the released buffer into Opus, assigns a nonce-derived
request UUID, persists it, verifies it and clears the PCM. The receipt cue occurs
only after that verified local save. UUIDs remain distinct across offline boots,
including after every journal slot has been acknowledged and erased. Raw input
bypasses the streaming AFE/VAD pipeline; recognition quality still requires the
physical acoustic comparison below.

A separate bounded network task transmits immutable recordings and reconnects.
New presses cancel old narration/uploads without blocking local microphone
capture. Replays retain the original IDs, timestamp, answer reference and framed
SHA-256. Only the first offer can be a foreground answer; later offers are silent
and no more frequent than once per thirty seconds. The backend separately bounds
transcription attempts. A needs-attention response keeps the recording locally.
Every receipt is checked against the complete capture envelope and actual digest
before erasure, including receipts arriving after a newer Talk press.

The authenticated gateway supplies assignment and answer context. Before a new
caption can be presented, the worker writes and verifies an `ORP1` pending cache;
the network callback waits at most one second. The main caption handler activates
that context and queues the final `ORC1` commit. Boot accepts only a committed
cache. A failed update therefore cannot silently restore the previous answer
reference. Cache writes have three attempts spaced thirty seconds apart; a fresh
authenticated context can reopen that bounded retry window. No flash operation
runs on the button, input or main task.

The profile requires the negotiated `audio_capture` gateway feature and matching
backend intake/context routes. A legacy gateway cannot initialize this recorder.
Keep the existing installed firmware until the coordinated backend/gateway
activation is validated. CoreS3 and generic profiles retain their streaming path.

## Network stalls and cancellation

Local capture uses the ESP-IDF WebSocket framing and TLS transport through a
single I/O worker. Main-task controls enter a queue of at most four pending
frames; handshake and upload workers wait for actual transmission results.
Partial writes continue only the unsent bytes under one three-second deadline.
Connect and HTTP upgrade share an eight-second deadline, although the underlying
DNS resolver can exceed that deadline before returning. The caller cancels its
wait after twenty seconds. A new Talk press cancels an active upload immediately;
only the I/O worker can dispose its TLS handles after the pending operation exits.

Incoming text is capped at 8 KiB and binary at 2 KiB, including fragmented
messages. A stalled frame read has a 500 ms bound; an unfinished message has a
five-second deadline. Authentication, trusted certificates and the compiled
gateway endpoint remain required. The other profiles keep their existing
transport. Public-client tests exercise partial/error/WANT writes, stalled
connect/read/upgrade, bounded queues, fragmentation and disposal from callbacks.

## Validation and activation boundary

The host suite compiles the actual journal, ESP adapter, PCM ownership and wire
parser with fault-injected dependencies, OpenSSL AES-GCM and ASan/UBSan. It checks
restart, interrupted writes, corrupt/full storage, exact metadata, stale receipts,
nonce faults and rapid press/release interleavings. The canonical ESP-IDF 6.0.2
StopWatch bench profile builds, including both WebSocket and MQTT protocol code.

This candidate has not been installed. The remaining release work is local
spoken feedback and the recording-repair
experience, coordinated gateway/backend activation, and physical acceptance.
The device must demonstrate real microphone
recognition and speaker clarity, power loss at each save/cache boundary, full
storage, repeated presses during reconnection, restart replay, and truthful
feedback when storage or speech processing fails. Software tests do not establish
those physical results.
