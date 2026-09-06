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

## Validation and activation boundary

All 148 host tests pass. The host suite compiles the actual journal with OpenSSL AES-GCM under ASan/UBSan.
It checks restart, every short-record write cut, tampering, corrupt-slot quarantine,
full capacity, lost/stale receipts, conflicting retries and storage failures.
Independent regressions compile the actual ESP adapter and boot entry point with
hardware stubs, exercise the maximum 342,350-byte recording and PSA failure
cleanup, and verify NVS preservation in both Provisions and generic profiles.
The canonical ESP-IDF 6.0.2 bench profile builds successfully.

The journal is not yet instantiated by the application and this commit writes no
device storage. Microphone ownership, authenticated context caching, reconnect
replay, server durable receipts, honest local feedback and physical power-loss
acceptance still need integration. Do not advertise offline capture or install
this checkpoint as a completed voice release.
