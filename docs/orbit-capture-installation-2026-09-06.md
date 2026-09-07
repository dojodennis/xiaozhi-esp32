# Orbit durable capture installation — 6 September 2026

The connected StopWatch now runs the reviewed durable-capture source
`a4376a34cb3ed6b2eef3207c472c16df060819b0`, version 2.4.8. The active gateway is
`caa2f9a`; conversation v6 and capability v9 were deployed before installation.
This update keeps the accepted crest and board profile, adds verified local
recording, exact server receipts, interruption handling and bounded explicit retry.

## Installation proof

The canonical `bench_profile.json` build uses ESP-IDF 6.0.2. Its only SDK-config
difference from the accepted app is `CONFIG_PROVISIONS_LOCAL_CAPTURE=1`.
Dependencies, partition binary, board pins, five crest files and packed assets
match the accepted baseline. Assets end at 2,694,681 bytes; the full reserved
2 MiB journal tail was erased in a fresh device readback. Independent review is GO.

The accepted public key verified the external RSA signature. Only the active ota0
application at `0x20000` was written: 3,084,288 bytes, SHA-256
`0d73ed62289c7482553a0b03ef5508c770b5729c40f5e67000f98a9d7d2bead2`.
The signed image leaves 1,044,480 bytes in its slot.

The fresh full 16 MiB recovery matched the accepted prior image and independently
verified against the device before writing. After the app-only write and hash
check, a full-device comparison passed **before normal boot**. Expected-image
SHA-256: `6ca2174fb02dc608c626b03c41a9bde00e4c606c8fd1d72f711b1445b86c62d6`.
Bootloader, partitions, NVS, OTA selector, assets and eFuses were not written by
the installer. Read-only security checks still report flags zero. USB-Serial/JTAG
reset entered download mode; subsequent write/verification commands used no-reset.
The generated multi-region flasher configuration was not used.

## Runtime proof and limits

Three bounded 40-second passive USB observations reached network connected,
authenticated gateway, activation complete and idle. No microphone activation,
panic, watchdog, stack overflow or capture-failure message was observed. Raw logs
were not retained. Startup warning/error subsystem counts were stable across
boots (SPI/display, Wi-Fi, OTA and optional model loader); these observations do
not establish physical display or speaker quality. Sampled free internal SRAM
remained at least 109,243 bytes, with a minimum-since-boot sample of 98,159 bytes.

First authenticated startup intentionally created the journal key, unused nonce
counter and committed capture-context cache in NVS. A bounded read of that private
partition and the official IDF parser verified selected entry/data CRCs, a
32-byte key, counter zero, committed `ORC1` context and the existing authorized
assignment. A second read after restart proved the same key, counter and context.
No key or unrelated NVS value was printed. This verifies initialization and
restart persistence; there were no microphone recordings in this check.

Real Talk presses, speech recognition, speaker clarity, interruption, offline
recording/replay, storage-full faults and signed-in Provisions/KitchenMEP
continuity remain acceptance gates. Do not equate network canaries or passive
boot with those physical results. The separate reply-ring change is not installed.

## Recovery

The signed application, accepted public key, manifest and redacted observations
are archived locally under `device-builds/provisions-kitchen-helper/2026-09-06/`
`stopwatch-2.4.8-capture-a4376a3`. Full-flash and NVS recovery images are retained
separately in protected `device-backups/.../2026-09-06/capture-a4376a3` storage.
They contain credentials and recording keys; never commit or publish them.

Preserve runtime NVS and journal data during recovery. Do not restore the
preinstallation full image after recording starts: it predates the new key and
recordings. Recover with a validated application-only image that preserves this
journal and its assets boundary. Keep a capture-compatible gateway (`f01be30` or
newer); do not disable durable capture, erase retries or drop forward schema.
