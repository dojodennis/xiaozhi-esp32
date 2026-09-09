# Supervised Orbit timer bench — 9 September 2026

**Supervised session complete; normal voice restored.** Six preset alerts, physical vibration, one exact
acknowledgement and the restart face were observed. The alarm was initially
audible but later absent or faint, so **audio acceptance has not passed**.
The accepted application passed full-image verification and authenticated startup.
After the final Talk/Hello/release check, Dennis heard the intelligible reply
“I'm here, what do you need.” The passive reader was closed without requesting
a reset, leaving normal Orbit in place. No overall device/purchase acceptance is claimed.

Dennis confirmed connection after the concrete supervised temporary installation,
six preset timer sound/vibration/restart check and restoration explanation.
[Authorization context](authorization.json). Codex is the sole device owner.
The [signed package](../orbit-schedule-bench-signed-2026-09-09/README.md) and
[installation/restoration procedure](../orbit-schedule-bench-installation-proposal-2026-09-09.md)
define the unchanged scope. No phone, broader gateway deployment or purchase is
part of this session.

## Fresh checks and exact application write

Fresh USB identity is the expected ESP32-S3 v0.2, MAC `28:84:85:44:6b:10`,
with 16 MiB flash. The actual port is `/dev/cu.usbmodem2101`. Hardware Secure Boot
and flash encryption are disabled, matching this signed-on-update profile.
Local serial tooling is esptool 5.3.1. The first read deliberately entered download
mode; all later inspection/write/verify commands explicitly preserved download
state. No security setting or eFuse was changed.

A fresh protected full backup was independently verified against the stopped
device using its full-image digest. Backup SHA-256:
`81be6aa58198a1d59c913ecfb6ba1b750246122a3c7aa6158c7f45c0416dfcd2`.
The installed a84 application, bootloader and partition region match their accepted
hashes. The table's MD5 is valid. ota_0 is selected with sequence 1, state VALID,
CRC `0x4743989a`; the second selector is erased. [Partition/OTA evidence](partition-ota.json).

The official cached ESP-IDF 6.0.2 NVS parser and integrity checks found no error.
The only notice is an unused existing assets namespace. Bench namespace/state
are absent. Actual bitmap counts include variable-length payload slots:
105 written of 504; one fully empty page, one FULL page with no live slots,
and 39 empty slots on the active page. A conservative 3,567-byte blob uses 112
payload slots, up to two chunk headers and one index. Existing data, two complete
copies, one new namespace and a reserved spare page require 462 slots, leaving 42.
The independent reviewer checked IDF's data-before-index allocation and compaction
selection of the zero-live page. This establishes the allocation precondition;
actual commit/restart behavior still needs physical evidence.
[NVS summary](nvs-before-summary.json), [independent review](prewrite-independent-review.json).

The write changed only the aligned **`0x020000–0x311000`** application span,
using the fixed 3,084,288-byte signed image
`e8e8811c3d6c80bc0119e14aea1c89fbd2b6265a1f561bca91a310612540c7b2`.
The local expected image preserves every byte outside that span. Full 16 MiB
device verification passed before normal boot, at 13:19:44 UTC. Expected image:
`dce5ceb17d35d12099980a9d5b5231c3afb792e398cc195d6c884ed40faa3f66`.
[Overlay](install-overlay.json), [exact commands](install-commands.json),
[execution result](install-execution.json), [full verification](install-verify.log).

## Physical observation and restoration

Normal boot reports the expected ELF prefix `b5a013453` and isolated BENCH marker.
The initial console contains no fatal marker. GPIO 0 conflict and display command
override warnings are retained in the [initial observation](bench-initial-observation.json).
Driver startup does not prove a usable face, audible alarm or felt vibration.

Dennis reported an audible alarm and `Rice, Sauce plus four more` after one yellow
press, then confirmed felt vibration. One instructed blue press removed Rice;
the face became `Sauce, Bread plus three more`. He subsequently clarified that
only vibration remained. That report reached Codex after the controlled restart
had already started; it is retained as a pre-restart observation.

Before restarting, Codex read and independently device-verified current NVS.
The official parser found no integrity error and all **16 unrelated logical
records remained identical** to the initial backup. The actual frozen firmware
codec and FaceModel accepted the extracted 356-byte record: Rice alone is
acknowledged and pending; Sauce, Bread, Stock, Pasta and Fish are still due.
Clock restoration correctly awaits fresh time. This uses the exact frozen codec
on the host, not a substitute decoder or evidence of physical sound.

After restart, Dennis confirmed Sauce remained displayed, vibration continued,
and a background sound was present but too faint. He also explicitly confirmed
`NEEDS SYNC`. Repeating cadence, the entire five-item summary after restart, and
apron-strength vibration were not confirmed. He did not report another blue press.

The frozen alarm sequencer intentionally leaves a one-second gap after a clip
drains. The pre-restart silence duration was not measured, so that observation
alone does not locate a defect. The post-restart weak sound still leaves audible
alarm acceptance unpassed. A source review initially identified an admission
failure path in `FillLocalFeedbackLocked`, but the archived configuration and
frozen source show its output-fence guard is disabled in this image. **That path
cannot explain this physical result and is excluded from this fix.** Investigate
the actual unguarded repeat/decode/resample/output/drain path, with a focused
regression and measured clip/output evidence. No firmware patch or substitute
image was part of this supervised session.

The first full post-bench read and later final 2 MiB chunk were interrupted by
`No more data to read from the serial port`. Both read-only identity rechecks
succeeded with the same stopped device; no restoration write occurred during
those retries. Seven complete 2 MiB reads plus two final 1 MiB reads formed a new
16 MiB image, then the entire image independently matched the device digest.
The stopped display/alarms during backup were expected download-mode behavior.
[Read interruptions](postbench-read-interruptions.json),
[fresh backup](postbench-backup.json), [whole-image verification](postbench-full-verify.log).

Fresh post-bench NVS passes integrity checks. All 16 unrelated logical records
remain unchanged, and the bench blob is byte-identical to the post-ACK record
across restart. Every flash byte outside the application and NVS is unchanged
from the initial backup, including the capture journal. The independent reviewer
confirmed the restore overlays only the accepted a84 application on this fresh
image and preserves the new NVS. [Restore overlay](restore-overlay.json),
[independent review](restore-independent-review.json).

The accepted 3,084,288-byte a84 application was restored at the same exact span.
The full expected image **`e2043ed10be52dc00c2ba9322b58147ada620ad20094aef46ec5f71119591cea`**
matched the device at 13:43:34 UTC before normal boot. No old full backup was
restored. [Execution](restore-execution.json), [full verification](restore-verify.log).
Normal boot shows expected ELF `3d9044b8c`, an authenticated Provisions gateway
session and activation completion, with no fatal marker. A model-loader warning
is retained. Dennis's final voice test confirmed an intelligible response; he
did not separately rate its loudness or explicitly count replies.
[Startup observation](accepted-initial-observation.json),
[physical restored-voice observation](physical-restored-voice-observation.json),
[reader closure](accepted-boot.json).

Dennis then authorized continued engineering work on the weak bench alarm and
preparation of a reviewed next candidate. That instruction does not authorize a
new firmware installation or a new supervised window. Diagnose clip level, gain,
output arbitration and repeat behavior with targeted proof, preserving today's
timer/vibration/restart evidence and the restored working voice application.

Raw full flash/NVS images, recordings, private values and signing keys are excluded.
The private session archive is
`/Users/dojo/.codex/device-builds/orbit-schedule-bench/2026-09-09-supervised-1308`.
[Published evidence hashes](evidence-manifest.json). The fixture uses six preset
timers, not voice-created timers; live cycles, hotspot recovery, battery and the
fixed-candidate 48-hour soak remain separate gates.
