# Orbit alarm repeat: offline evidence

Date: 2026-09-09  
Frozen source: `45772685c082a55ef26673da078979a96facdbf6`  
Asset: `main/assets/common/exclamation.ogg`  
Asset SHA-256: `61d0a91d7658e62c2ee0a940bd8b99e2062c4b760848f0ba2d793c852def4716`

## Reproduction

Run:

```sh
python3 repeat_decode_audit.py --output repeat_decode_evidence.json
```

The script parses the Ogg pages, skips `OpusHead`/`OpusTags`, and feeds all 15
audio packets through one host `libopus` decoder ten times without resetting it.
It also asks FFmpeg to loop the Ogg ten times and resample the result to the
board's 24 kHz mono output format. The complete machine-readable result is in
`repeat_decode_evidence.json`.

## Result

- Asset: 1663 bytes, Opus mono, FFprobe stream rate 48 kHz, duration 0.895563 s.
- Same decoder: 15/15 packets decoded on every pass, 14,400 samples per pass,
  peak 24,363 (-2.574 dBFS), RMS 9,387.86 (-10.858 dBFS) on all ten passes.
- FFmpeg loop/resample: ten non-empty passes at 24 kHz; peak remained -2.567 to
  -2.568 dBFS and RMS remained -10.825 dBFS (the final sample count varies by
  one due to stream-loop rounding).
- Host local-feedback suite at the frozen source also passed all 8 tests,
  including packet delivery, cancel/replace, final drain, decode/output/demux
  failures, reset/stop, frame integrity, and empty-tail drain behavior.

## Toolchain and limitations

- macOS arm64; Python 3.14.6.
- `/opt/homebrew/bin/ffmpeg` and `ffprobe` 8.1; FFmpeg has `libopus` enabled.
- `/opt/homebrew/lib/libopus.0.dylib`, `libopus 1.6.1`.
- This is host evidence only. It does not exercise the ESP Opus wrapper, ESP
  audio-effects resampler, ES8311, DMA, PA_EN, speaker, enclosure, power, or
  physical audibility. It cannot rule out a runtime/device output failure.

## Known cached ESP-IDF recipe locations

The prior canonical build log records the isolated builder recipe (no build was
run for this evidence):

- ESP-IDF 6.0 at `/opt/esp/idf`.
- Python environment at `/opt/esp/python_env/idf6.0_py3.12_env/bin/python`.
- CMake/Ninja build directory inside the builder: `/workspace/build`.
- `CCACHE_ENABLE=True`.
- Defaults: `sdkconfig.defaults;build/xiaozhi-build.sdkconfig.defaults`.
- Board: `provisions-kitchen-helper-stopwatch`; target `esp32s3`.
- Existing artifact/evidence bundle:
  `/Users/dojo/.codex/device-builds/provisions-kitchen-helper/2026-09-06/stopwatch-2.4.8-talk-release-a84ab070/`.

Those paths are recipe/cache evidence from `canonical-build.log`, not proof that
the builder or cache is currently mounted on this host.
