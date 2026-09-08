# Orbit schedule bench: supervised installation and recovery proposal

Status: prepared for review on 9 September 2026. **No signing, serial access,
reset or installation has occurred.** Codex owns the supervised procedure.
This is a temporary, offline hardware check, not a live schedule release.

## Exact candidate and recovery application

The candidate is the reviewed firmware source
`45772685c082a55ef26673da078979a96facdbf6`, with the distinct identity
`provisions-kitchen-helper-stopwatch-schedule-bench` and
`hardware_bench_profile.json`. Use the frozen archive, not the current `build/`
directory, which was subsequently used for another variant:

`/Users/dojo/.codex/device-builds/orbit-schedule-bench/2026-09-09-ecffe7aa`

Its `xiaozhi-unsigned.bin` is 3,080,192 bytes, SHA-256
`ecffe7aa30eddcf0e965a1beeeef82326357bbba28057ad418160d9f89ba706c`.
Offline esptool 5.3.0 reports a valid ESP32-S3 image/checksum/hash, ESP-IDF
6.0.2, version 2.4.8, 16 MiB flash, DIO/80 MHz and secure version zero.
The candidate is unsigned and must not be installed in that state.

The latest documented installed application is the physically accepted
Talk-release repair, source `a84ab070dfb3205c900430902c949d899e77e4c8`:

`/Users/dojo/.codex/device-builds/provisions-kitchen-helper/2026-09-06/stopwatch-2.4.8-talk-release-a84ab070/application.bin`

That signed recovery application is 3,084,288 bytes, SHA-256
`32473218a5c311c164815cc1ad0535eff0e182f178f0b7af59d657da13418f7a`.
Its archived public PEM has SHA-256
`09d1e0fd2dfb13640b8a99fcd7e284b6b24e6a06003bce4e3c72a49004364250`.
An offline RSA signature verification with that public key passed again during
this audit. This verifies the archived recovery file; it does not attest the
board's current contents. The older a437 capture-installation report is historical.

[Candidate source, build and renderer evidence](orbit-schedule-hardware-bench-2026-09-09.md)
and [offline preparation evidence](orbit-schedule-bench-installation-proposal-2026-09-09.json)
pin the inputs. The historical build has no comparable archived dependency-lock
hash; do not claim dependency equality with that build.

## Write boundary and boot behavior

The existing 16 MiB partition contract is:

| Region | Half-open flash range | Procedure |
| --- | --- | --- |
| Bootloader and pre-table bytes | `0x000000–0x008000` | Preserve |
| Partition table | `0x008000–0x009000` | Preserve and verify |
| Shared NVS | `0x009000–0x00d000` | Preserve |
| OTA selector | `0x00d000–0x00f000` | Preserve; inspect both records first |
| PHY and NVS keys, padding | `0x00f000–0x020000` | Preserve |
| ota_0 | `0x020000–0x410000` | Only a verified application span may change |
| ota_1 | `0x410000–0x800000` | Preserve |
| Assets | `0x800000–0x1000000` | Preserve, including capture journal at `0xe00000` |

The proposed path is an application-only overwrite of the **freshly proven,
currently selected and VALID ota_0 slot**. The signed image length and hash must
be pinned after external signing and public-key verification. For the archived
3,080,192-byte unsigned input, ordinary RSA v2 padding plus its signature sector
is expected to produce 3,084,288 bytes and the span `0x020000–0x311000`.
That size is an expectation, not evidence of a signed candidate. Reject any
unexpected size, changed unsigned payload, slot overrun or write outside the
reviewed sector-aligned span until it is separately reviewed.

Preserving a VALID selector is essential: this bench never starts network
activation and cannot reach `Ota::MarkCurrentVersionValid()`. Installing it as
OTA NEW/PENDING_VERIFY would leave it subject to rollback on a later reset.
Do not alter OTA data or add a fabricated gateway-health success to bypass this.
Stop if the freshly decoded active slot/state differs from the proposed path.

An overwrite of an already VALID slot **does not provide automatic rollback
based on the new bench application's health**. Recovery is the manual,
application-only procedure below. The profile uses signed-on-update RSA without
hardware Secure Boot; do not describe it as boot-time signature enforcement.
Update verification derives its trust key from the running application's
signature block, so use the same reviewed signing key for the bench and restore.
The private signing key is not an input to this audit and must not enter the
repository or report.

Do not use `scripts/prepare_provisions_reversible_bench.py` for this update.
That factory-style helper emits an erase command and writes bootloader, partition
table, new NVS, OTA data, application and assets. It also accepts registered
normal identities, not this offline bench identity. Do not use generated
multi-region flasher arguments, an OTA updater, or an old full-image restore.

## Before the supervised write

1. Agree a supervised interruption window with Dennis. Opening the recorded USB
   port can reset this board, so even a console/read operation belongs in that
   window. Confirm the actual board identity against recorded MAC
   `28:84:85:44:6b:10`; a path alone is not device identity.
2. Prepare the signed candidate from the pinned unsigned archive through the
   existing signing route. Independently verify its signature with the accepted
   public key, its complete unsigned payload, image metadata, length, hash and
   app-slot fit. Save the signed manifest and exact proposed write range for
   review. Do not rebuild or silently substitute a binary.
3. In the supervised window, obtain a fresh full 16 MiB recovery image and verify
   it independently against the stopped device. Keep it in protected local
   backup storage. It contains credentials, capture keys and potentially private
   recordings; publish only hashes and redacted structural results.
4. From fresh evidence, verify chip/security configuration, the existing
   bootloader and partition layout, both OTA-selector records and their CRCs,
   selected VALID ota_0, the installed signed application's hash and accepted
   verification key. A mismatch with the recorded baseline stops this proposal;
   it is not permission to force the old image onto the device.
5. Check shared NVS integrity and available capacity without erasing or dumping
   secrets. Record whether `orbit_bench_v1/state` is absent, present, corrupt or
   unreadable. Only verified absence permits a new physical seed. The 3,567-byte
   format bound does not prove that this 16 KiB NVS partition has enough free
   pages for commit/compaction. Verify the capture-journal boundary and preserve
   every byte there. No automatic namespace clear or global NVS erase is allowed.
6. Construct the expected post-write full image locally from the fresh recovery
   image, replacing only the exact signed app span. Record the old/new app hashes,
   full-image hashes, range and unchanged-region comparison. Keep raw recovery
   and expected images private. Review the concrete package before the write.

Any physical mismatch, signature failure, invalid/uncertain OTA state, unsafe
NVS condition or changed artifact leaves installation unapproved. Local software
work can continue independently.

## Installation observation and limited hardware check

After authorization for the exact package, enter the documented download state
once. Subsequent app write/readback operations must use the established no-reset
sequence with both `--before no-reset` and `--after no-reset` explicitly set,
and exactly one app offset/file pair. Apply the same reset controls to security
inspection: one historical read-only security command ended with an RTS reset.
Verify the app and the complete
16 MiB expected image **before normal boot**; prove every byte outside the signed
app span unchanged. Do not write bootloader, NVS, OTA data, assets or eFuses.

On normal boot, record the bench identity, visible BENCH indicator, boot outcome
and storage status. A blank screen, reset loop or storage fault is a failed gate,
not permission to erase. No microphone capture, gateway or remote receipts are
expected from this profile.

With verified empty bench state, one physical yellow press seeds six short named
timers. Observe the first expiry, real speaker alarm, motor behavior and whether
blue acknowledges exactly the alarm that was displayed. Record acoustic and
button timing and any ALARM/HAPTIC/storage fault. A locally committed ACK may say
it is stored here with sync pending; it must never claim a server SAVED receipt.

A supervised restart must restore the saved schedule/outbox and show NEEDS SYNC.
All six fixture timers share the same seed-plus-15-second deadline. After expiry
and one acknowledged alarm, restart must preserve that exact ACK as pending and
restore the other five due alarms, including their alert output. Persisted due
items retain their DONE presentation. This sequence does not demonstrate unknown
future countdowns; that rendering remains host-tested until a distinct reviewed
physical scenario exists. This offline fixture cannot furnish fresh authoritative
time after restart, synchronize an ACK, edit Service, or run repeated independent
seed cycles. Do not erase present state to make those gates appear to pass.
Sound/haptic strength must be observed on the actual board, including through an
apron. Host renderer and queue-drain tests are not that evidence.

This procedure can establish initial hardware behavior and durable restart, but
not the full 20-cycle, hotspot, live Service, battery or 48-hour soak acceptance.
Those require the authenticated live schedule contract and a fixed integrated
candidate. No purchase gate is passed by this proposal.

## Restore the accepted voice application

Before leaving the bench, take and verify a **new** protected full-device recovery
image in the supervised download state. It must include any NVS changes made by
the bench. Runtime writes and NVS page compaction may change physical NVS bytes;
the other namespaces must remain logically intact. Byte-for-byte preservation
outside the app span is proved separately for each stopped install/restore write,
not across the entire running bench session. Re-attest active slot, security and
partition state. Build an expected
restore image from that fresh image by overlaying only the archived, verified
a84 signed application at `0x20000`, length 3,084,288, ending at `0x311000`.

Perform that application-only restore and verify the entire expected image
before boot. Preserve current capture keys, retries, journal, assignments and the
bench namespace. Never restore the September 6 full-flash backup after subsequent
capture activity, and never pair an old NVS snapshot with a newer capture journal.
If a future signed bench has a different length, review the leftover app tail
explicitly; this proposal does not authorize a whole-slot erase.

After restore, verify the accepted application boots and re-authenticates, then
have Dennis check Talk capture/release and one audible response. Record both the
bench result and successful restoration. A board left on the offline bench is
not a completed installation/recovery session.

## Source anchors

- `main/boards/m5stack/stopwatch/hardware_bench_profile.json`: distinct offline
  identity and retained security/hardware configuration.
- `main/boards/m5stack/stopwatch/m5stack_stopwatch.cc`, `StartNetwork()` and
  `StartHardwareBench()`: offline startup and explicit Bench store domain.
- `main/application.cc`, `HandleActivationDoneEvent()` and
  `ReconnectVoiceGateway()`: gateway-conditioned production boot acceptance.
- `main/ota.cc`, `MarkCurrentVersionValid()`: changes only pending-verification
  state; it is not invoked by offline bench startup.
- `partitions/provisions/16m.csv`: fixed partition boundaries.
- `scripts/prepare_provisions_reversible_bench.py`, factory bundle command
  generation: incompatible erase/multi-region writes.

No runtime source changed for this proposal. Current firmware remains the
reviewed candidate above, and the board's actual current state still requires
fresh supervised evidence.
