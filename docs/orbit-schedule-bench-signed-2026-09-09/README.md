# Signed Orbit timer bench — ready for a supervised decision

The frozen timer bench is now signed and verified off the device. No serial
port was opened, board reset, firmware installed or provider called. Dennis's
request to accelerate puts this physical check ahead of broader architecture.

| Artifact | Exact value |
| --- | --- |
| Reviewed runtime | `45772685c082a55ef26673da078979a96facdbf6` |
| Profile | `provisions-kitchen-helper-stopwatch-schedule-bench` |
| Unsigned payload | 3,080,192 bytes; `ecffe7aa30eddcf0e965a1beeeef82326357bbba28057ad418160d9f89ba706c` |
| Signed application | 3,084,288 bytes; `e8e8811c3d6c80bc0119e14aea1c89fbd2b6265a1f561bca91a310612540c7b2` |
| Accepted public key | `09d1e0fd2dfb13640b8a99fcd7e284b6b24e6a06003bce4e3c72a49004364250` |
| Proposed application span | `0x020000–0x311000`, exclusive end; only after fresh selected VALID ota_0 proof |

Binary and private preparation archive:

`/Users/dojo/.codex/device-builds/orbit-schedule-bench/2026-09-09-ecffe7aa-signed`

[Manifest](manifest.json), [public signature verification](verify.log) and
[image metadata](image.log) pin the exact new artifact. The unsigned prefix is
byte-identical to the frozen reviewed image. The accepted public key verifies
the RSA signature. The signed length and app-slot fit match the existing proposal.
The existing local signer is mode 0600; deriving its public key produced the exact
accepted public PEM bytes. No private material or new trust key was generated
or disclosed. The historical key-path metadata gap is now resolved cryptographically.

Tools were the cached ESP-IDF 6.0.2 image
`sha256:0d8c9773d48a327233f9c1d7c654ff0bcf133ae24503ea2e97a57cfe02b8cb67`
with espsecure/esptool 5.3.0, network disabled and no USB/serial mounts. Separate
verification used only the accepted public key. Owned containers were removed;
the signing cleanup check initially rejected Docker's lowercase absence wording,
then the exact destroy event and name absence were verified. It was not a failed
signature or retained container. This profile verifies signatures on update;
it does not add hardware Secure Boot enforcement.

The existing Orbit technical owner independently recomputed the signed, unsigned
prefix and accepted public-key hashes, checked the span, and reviewed the public
verification and cleanup evidence. [Review record](review.json): **GO for the
frozen off-board package and a supervised conditional installation decision**.
This review does not authorize touching the device or establish physical acceptance.

## Accelerated sequence and remaining blockers

**Earliest defensible slot: the next supervised USB window today, after Dennis's
explicit device approval.** Allow **90 minutes, with up to two hours reserved**
for fresh full-device backups, verification and restoration. This is a planning
allowance, not measured completion time; uncertain board state may stop the
session before any write. Availability has been requested separately and does
not itself authorize flashing.

1. **Before touching USB:** the signed build and accepted recovery app are fixed.
   Dennis approves the specific temporary application-only bench session and
   interruption of ordinary Orbit use. No approval is inferred from acceleration
   or previous iPhone installation requests.
2. **Before any write:** in that supervised window, verify board MAC
   `28:84:85:44:6b:10`, chip/security/partition state, both OTA records, selected
   VALID ota_0 and the current accepted a84 application. Take and independently
   verify a fresh full 16 MiB backup. Check NVS integrity/capacity and verified
   absence of `orbit_bench_v1/state` before seeding. Construct and review the exact
   expected full-image overlay locally. Any mismatch stops the proposed write.
3. **Install and observe:** only the pinned signed app span may change. Explicit
   no-reset controls surround each operation after entering download state.
   Verify the complete expected image before normal boot. One yellow press seeds
   **six preset names, all due after 15 seconds**. Observe real speaker and apron
   vibration, acknowledge exactly one displayed alarm with blue, then restart:
   the same ACK remains pending and the other five due alarms return with NEEDS SYNC.
4. **Restore before finishing:** take a new post-bench backup, preserving the NVS
   changes and capture journal. Restore only the accepted signed a84 application
   into the same verified span and verify the complete expected image before boot.
   Confirm ordinary startup/authentication and Dennis's Talk-release/audio check.

The [existing reviewed installation and restoration procedure](../orbit-schedule-bench-installation-proposal-2026-09-09.md)
remains authoritative for exact checks, stop conditions and preserved regions.
Do not use the factory multi-region helper, erase NVS, change the selector,
substitute an old full backup or rely on automatic health rollback of this
offline application. A successful session includes restoration of normal Orbit.

## What this does and what comes next

This is the fastest available physical proof of named timer rendering, actual
alarms, motor behavior, correct acknowledgement and restart persistence. It is
**not yet a user-created or voice-created timer demo**, repeated-cycle acceptance,
hotspot recovery, battery measurement or the 48-hour fixed-candidate soak.

Codex leads the bench. The existing gateway contributor is validating the basic
timer runtime in parallel; Service/KitchenMEP publication, lists, advanced relay
and design expansion are deferred from this immediate path. The actual
user-created named-timer milestone still needs the compatible gateway/firmware
path and its end-to-end test. Keep the installed iPhone retest separate. No
unattended Claude worker or physical acceptance is claimed.
