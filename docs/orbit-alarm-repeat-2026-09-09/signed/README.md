# Signed diagnostic retest package

The exact reviewed diagnostic image is signed and verified off the device. No
device was opened, reset or written. Normal a84 voice remains the accepted
installation. This package is for a new supervised decision, not a completed test.

| Input | Pinned value |
| --- | --- |
| Reviewed source | `201dbbce9b076fd26155d42b618d7bd2d048eeb2` (runtime `fa0b92f8ec5d0601d1aa7b6df691ef7d2004ba66`) |
| Unsigned application | 3,080,192 bytes; `701d115b5dfca263757a0195090bbd7bdbe4fc39864c37cbd58648de5f0f178d` |
| Signed application | 3,084,288 bytes; `ff8a26d6cc0c9c395a4d8d0ecfa06bf0af32b9b5e691d78cdedf83a762f1ebca` |
| Accepted public PEM | `09d1e0fd2dfb13640b8a99fcd7e284b6b24e6a06003bce4e3c72a49004364250` |
| App-only span | `0x020000–0x311000`, exclusive end |
| Restore application | Accepted a84, 3,084,288 bytes; `32473218a5c311c164815cc1ad0535eff0e182f178f0b7af59d657da13418f7a` |

[Manifest](manifest.json), [signature verification](verify.log), [image metadata](image.log)
and [exact proposal/templates](retest-proposal.json). The signed prefix exactly
matches the reviewed unsigned payload. The existing mode-0600 signer's derived
public PEM matches the accepted key byte for byte. Public-only RSA block-0
verification and ESP32-S3 checksum/hash inspection passed with cached IDF 6.0.2 /
espsecure 5.3.0. All owned signing/verification containers were removed; network
was disabled and no device was mounted. No key was created or private material
published. This retains signed-on-update trust, not hardware Secure Boot.

The immutable local signed application is:
`/Users/dojo/.codex/device-builds/orbit-alarm-repeat/2026-09-09/signed/application.bin`.
Its exact retained unsigned prefix is
`/Users/dojo/.codex/device-builds/orbit-alarm-repeat/2026-09-09/bench/xiaozhi.bin`.
Use that file only. The worktree's current `build/xiaozhi.bin` is a normal-profile
validation build. The earlier default-language image is rejected. Neither is the
retest input. Reuse the [317 checks, both builds and source review](../validation.json).

Independent package review recomputed the inputs, prefix, accepted key/recovery
hashes and write span and accepted the fresh-backup/preserve-state/recovery plan.
[Review: GO for the off-board package and proposed supervised retest](review.json).
[Fourteen package consistency checks](package-checks.json) also passed. Neither
result authorizes a device operation or establishes acoustic acceptance.

## Supervised session

Reserve roughly 60–90 minutes for backup, verification, the short acoustic check
and restoration. This is a planning allowance; the earlier session took about
41 minutes and included serial-read retries. Ordinary Orbit use is interrupted
during this temporary installation. Dennis must approve this new session; the
previous session ended with working voice restored.

1. After approval, Codex alone owns the actual verified USB port. Obtain a fresh
   protected 16 MiB backup and independently verify it against the stopped device.
   Verify MAC `28:84:85:44:6b:10`, expected chip/security/partition map, accepted a84
   application/key and CRC-valid selected VALID ota_0. Any mismatch stops this
   proposal. A port path or earlier backup is not current identity/state evidence.
   Prefer sixteen complete 1 MiB reads from the same stopped state, then assemble
   and verify the entire image. On a read interruption, recheck identity without
   resetting and retry only incomplete chunks. A reset invalidates that snapshot;
   never mix phases or reuse chunks from the earlier session.
2. Verify current NVS integrity and the existing 356-byte bench record, hash
   `7f64b646ba2f2748d0a89d7240f38ae7f6d452a1c39e5c73f6938761574daae9`.
   It contains Rice's pending acknowledgement and the other five due alerts.
   Decode with the frozen codec. Preserve current unrelated records and capture
   journal. Missing/changed/corrupt state requires review; never erase or reseed.
3. Review the exact full-image overlay derived from that fresh backup. Only the
   pinned signed app span may change. Preserve bootloader, partition table, NVS,
   both selectors, keys, remainder of both app slots and assets/journal. After
   entering download state, all operations—including security reads—use explicit
   `--before no-reset --after no-reset`. Verify the entire expected image before
   boot. No factory helper, OTA route, whole-slot erase or eFuse write is allowed.
4. Boot into BENCH with the existing five due alerts and NEEDS SYNC. **Do not press
   yellow or blue.** Observe 45 seconds of repeating output and correlate each
   boot-local sequence/attempt with timestamped clear, faint or silent reports.
   Retain start/write/drain counts, PCM levels, errors and lost-event counts.
   An incomplete/lost trace does not pass software delivery. Driver output and
   drain still do not pass physical sound. No new ACK is needed for this retest.
5. If that first window completes and Dennis is ready, perform one controlled
   restart and another 45-second observation, recording a distinct boot boundary.
   Do not infer an independent timer cycle or future-clock recovery. If there is
   a fatal boot/storage/face problem, preserve evidence and proceed to recovery;
   do not improvise a state reset. Alarm/trace failures remain failed observations.
6. Before finishing, obtain and independently verify a **new post-bench full
   backup**. Overlay only accepted a84 on its current NVS/journal; independently
   review the restore overlay and verify the complete expected device image before
   normal boot. Confirm authenticated startup and Dennis's Talk/release response
   within the existing voice allowance. Stop the passive reader normally. Never
   restore an older full-device backup over new data or leave the offline bench
   installed as a completed session.

This replaces the old seed/blue-ACK instructions for this diagnostic retest only.
The [prior installation/restoration procedure](https://github.com/dojodennis/xiaozhi-esp32/blob/da3f280067eb1fc3fea34c5726289fd25c3abbfc/docs/orbit-schedule-bench-installation-proposal-2026-09-09.md)
still supplies the region and recovery details. The current proposal deliberately
starts from existing state. It does not pass apron strength, voice-created timers,
hotspot recovery, battery, the 48-hour soak or a purchase gate. Signing this image
has not fixed or proved the weak physical sound.
