# Physical dictation v1

This is a source implementation, not physical-device acceptance. The frozen wire
contract is `orbit-offline-gateway/docs/orbit-dictation-device-v1.md`.

Blue double-click enters or leaves the dedicated Dictation screen. Blue single
click invokes its displayed Start, Stop or Resume action; the button library uses
disjoint single/double-click events. Yellow fresh holds record individual segments.
Leaving the screen closes input and retains journal/control state and raw parts.
The same held press cannot become an ordinary command after leaving. These
callbacks only publish atomic fences and event bits; main and the recorder worker
own codec, UI, protocol and storage work. Dictated words never select controls.

The device advertises `features.dictation_v1:true` alongside audio_capture and
turn_ids and requires a unique true server `provisions.dictation_v1` flag. Start
persists one UUID and waits for its exact ACK before input. Kind is `note`; this
profile has no configured IANA zone, so the explicit fallback is `UTC`. Resume
compares the previous acknowledged control revision, waits for ACK, and requires
a fresh physical edge. Start ACK1 means Stop payload1/ACK2, then Resume
payload2/ACK3. Receipt revision never substitutes for control revision. Pending
controls retry the same action, journal ID and payload; reconnect changes only
the transport session framing. A receipt lookup never grants recording authority.

The worker persists each hold's request UUID and ordinal before reporting the
microphone ready. An empty cancelled reservation is removed before Stop freezes
the manifest. Once audio exists, Stop counts the reservation even while encoding,
local persistence or upload remains pending. A failed raw write retains Processing
PCM for retry with that exact request and ordinal. Reboot cannot recover unsaved
RAM: a reserved part absent from the encrypted outbox is shown as needing recovery,
retains its manifest count and prevents new capture. It is never reported synced.

A held dictation input seals at exactly 160000 actual mono 16k samples; release or
that ten-second cap pauses and requires another physical hold. The existing 320
zero encoder-flush samples and Opus padding are excluded from `sample_count`.
Ordinary recording bounds/behavior remain unchanged. The manifest allows at most
60 segments/9600000 actual samples, but the outbox has only four 512KiB slots.
Occupied or quarantined slots are not evicted; a full store refuses a new hold.

The separate bounded NVS namespace `orbit_dct_v1`, key `journal`, stores a closed
version 1 binary manifest (`ORDICT01`, at most 1512 bytes): journal/assignment IDs, acknowledged authority
and expiry, pending control/CAS/count, and up to 60 immutable segment UUID/sample/
terminal records. Commits require read-back before publishing authority or removing
raw audio. A retained open ACK can authorize future fresh holds after a current
negotiated authenticated session proves the same assignment. That proof exists
only in RAM, survives a brief disconnect, and must be established again after boot.
An unsupported or reassigned peer invalidates it. Existing audio and timer NVS keys
are unchanged. The compact manifest bounds additional storage in the existing
16KiB NVS partition. If occupied settings leave insufficient capacity, persistence
fails closed without erasing old audio, timer or settings data.

After authenticated reassignment, a fresh Start may locally retire the previous
journal only when it was acknowledged, has zero segments and no pending control,
and every recording buffer, reservation, retry and outbox slot is positively empty.
The worker closes admission while it checks the actual slots and rechecks the
requested assignment, then commits one new pending-Start UUID. It does not send
an old-assignment Stop. Any uncertain phase, occupied/corrupt slot or missing part
preserves the previous journal and shows recovery. A failed read-back grants no
input; after reboot, the committed pending UUID, if present, still requires its
exact Start ACK and a fresh yellow press. Late acknowledgements for the retired
empty UUID cannot change the new journal. This does not retire backend data or
provide recovery for a nonempty or uncertain journal.

Only dictation captures use journal header `ORBAUD03`/version 3 (152 bytes). The old
`ORBAUD02`/version 2 (124 bytes) remains the exact write format for ordinary captures
and remains readable without rewriting any occupied slot. V3 authenticates purpose,
journal ID, sequence and actual sample count with the ciphertext. The 4096 byte body
offset, slot layout, packet limits, encryption keys and nonce mechanism are unchanged.

Dictation replay is always deferred and never acquires ordinary response authority.
Its closed capture metadata adds exactly purpose, dictation_session_id,
chunk_sequence and sample_count. Only an exact durable outer capture receipt and
matching terminal inner dictation receipt can remove raw audio. Session/capture/
sequence, terminal state, transcript-persisted flag, metadata, digest and byte count
are checked. The inner segment UUID is a separately validated server ID. A terminal
manifest update commits before the raw slot is erased, preserving total counts
when acknowledged slots are reclaimed. Pending, failed, unknown and malformed
receipts leave the raw part intact. No transcript text returns to the screen.

Host validation compiles the production journal, NVS adapter, recording worker,
wire parser, application press/mode/cap methods and AudioService capture fences.
Cases include Start/Stop/Resume CAS, uncertain ACKs/restart, Stop during sealing,
raw write/terminal-manifest failure, fixed IDs on retry, empty rapid release,
held input across screen exit, ACK arriving during a hold, exact cap samples,
legacy/v3 coexistence and torn headers. Peripheral task, network, codec and flash
interfaces are controlled host fixtures. Physical microphone/speaker behavior,
round-display legibility and actual button timing still require device validation.
