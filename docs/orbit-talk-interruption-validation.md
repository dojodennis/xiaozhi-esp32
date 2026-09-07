# Orbit Talk interruption — 6 September 2026

The accepted StopWatch firmware at `63957d4` ignored Talk while a response was
pending. This repair starts another capture and rejects replies belonging to the
interrupted turn. It is based on that exact installed source and preserves the
accepted crest and hardware profile.

Gateway and firmware negotiate bounded turn IDs. A physical press also has a
separate local generation, so a quick press/release or a press during a blocked
start cannot reactivate an abandoned answer. Queued state and speech callbacks
check the current connection, session, turn and press. A capture that never
started sends no phantom release and cannot leave Working active.

All 144 host tests pass, including compiled tests of the actual C++ fence and
application methods. They reproduce overlapping presses during the protocol send
and microphone startup, stale queued releases, quick taps and cancelled capture.
Independent review is GO. ESP-IDF 6.0.2 builds of the default Provisions and generic
StopWatch variants succeed and compile both WebSocket and MQTT sources. The
connected device uses the separate `bench_profile.json`; installation preflight
rejected the default variant because its partition table and signed-update
settings differ. No device bytes were written. Build the actual device candidate
using the board README's canonical command:

```sh
python3 scripts/build.py m5stack/stopwatch --config bench_profile.json \
  --name provisions-kitchen-helper-stopwatch --language en-US --wake-word disabled
```

Matching gateway `27512a7` passes 882 tests, Ruff and strict mypy.
A successful build does not prove microphone/speaker acoustics or the user's
end-to-end experience.

The exact bench-profile build now passes with the accepted `dependencies.lock`.
Its generated SDK configuration and partition table match the installed build
byte for byte. The accepted crest, motion, display and board source also match.
The signed-update check is retained. The separate default-profile artifact was
rejected and was never installed.

## Installed application

Gateway `27512a7` was deployed first and remains healthy. On the connected
StopWatch, only the application at `0x20000` was written: 3,018,752 bytes, SHA256
`be5ed6110713a28d3f59217eda9ba72d4b4560923179e0f1d7e55d08d30cc512`.
Source is `e8cb600` (implementation `d938601`), version 2.4.8, ESP-IDF 6.0.2.
The existing trusted RSA signature verified before installation.

Before writing, the full 16 MB recovery matched the device. After the app-only
write and its data-hash check, a separate full-device verification matched all
16,777,216 expected bytes, SHA256
`97721bb9a9b0b3e8fe4c3bb51bce5619f9b458b71d0137d8325ce9eae26f7c69`.
Bootloader, partitions, NVS, OTA selector, assets and eFuses remain unchanged.
The reversible bench security flags remain zero; no eFuse was written.

A 25-second passive USB observation confirmed Wi-Fi, network connection,
authenticated gateway, activation completion and idle. No microphone was
activated and no raw logs were saved. The existing optional model-partition
warning remains; no other error subsystem appeared. Free internal SRAM remained
at least 140,563 bytes in the captured samples, with a boot minimum of 124,563.

Local artifacts are archived under
`~/.codex/device-builds/provisions-kitchen-helper/2026-09-06/stopwatch-2.4.8-talk-d938601`.
Private pre/post full-flash recovery images are retained separately under the
same date in `device-backups`. Never publish those recovery images or flash the
generated merged build. The prior app requires its paired compatible gateway;
restore the accepted app before rolling the service back to `99d7a7d`.

Physical microphone/speaker use, actual Talk interruption under acoustic input,
and signed-in owning-app acceptance remain unproved. This installed repair does
not complete offline capture, stable product descriptions or cross-app dialogue.
