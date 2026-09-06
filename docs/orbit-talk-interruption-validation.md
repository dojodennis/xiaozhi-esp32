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

The matching gateway must be deployed first. Device installation must retain the
existing signing identity and use the verified app-only recovery process; never
flash the generated merged image. Current deployment/device evidence will be
added here after validation.
