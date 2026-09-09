# Output audit: the requested volume is already 100

The saved output-volume target in the verified retest snapshot is **100**. Both
accepted a84 and the diagnostic retain the same 90 minimum / 100 maximum startup
policy. Raising that software target is not a supported next fix. The actual DAC
register and physical amplifier output have not been measured.

The retained a84 build log identifies `esp_codec_dev 1.5.11` and `m5ioe1 1.0.9`;
its hash matches the accepted manifest. The diagnostic's frozen dependency lock
also selects `esp_codec_dev 1.5.11`. Relevant local package sources match their
component checksum manifest. [Reproducible audit](audit_output_config.py) and
[source evidence](source-audit.json).

## The missing acknowledgement

In this codec library, `esp_codec_dev_set_out_vol()` calls the codec's volume
callback but discards its return value and reports success. The mute setter does
the same. The cached volume/mute getters return requested state, not register
readback. Consequently, the application's successful setter result and the retest's
clean audio delivery counters do not prove that the requested hardware settings
were applied. This is a verified evidence gap, not proof that a write failed on
Dennis's board.

The board owns amplifier enable through M5IOE1 pin 10 and codec power through pin 3.
`GPIO_NUM_NC` in the codec constructor means the generic codec PA routine does not
control that expander pin. The board requests both pins high. M5IOE1 internally
checks mode/output-register writes, but its void convenience wrappers discard the
error result. An expander output-latch readback still does not measure the external
rail or amplifier output.

Using the existing configured 5 V PA, 3.3 V DAC and zero explicit PA-gain values,
the pinned library predicts these settings:

| Requested volume | Default curve | Predicted DAC register 0x32 | Quantized DAC gain |
| --- | --- | --- | --- |
| 90 | −5 dB | `0xbc` | −1.5 dB |
| 100, the saved target | 0 dB | `0xc6` | +3.5 dB |

These are source-derived predictions. They are not register, rail-voltage,
clipping or acoustic measurements. The configured voltage values are assumptions
in the driver; they must be checked against the board contract and actual hardware.

## Vendor comparison and review

The [official C152 specification](https://docs.m5stack.com/en/core/StopWatch)
identifies an AW8737A amplifier and 8-ohm / 1 W speaker. Its audio pin map agrees
with the repository. G3 and G10 are active-high controls; use the existing named
macros when reading them. Their library indices are 2 and 9, not literal 3 and 10.

Two official implementations use different clock and gain paths. The pinned
[M5Unified StopWatch callback](https://github.com/m5stack/M5Unified/blob/db7268821ed1fc29512575f12d735dfde2b1eff0/src/M5Unified.cpp)
uses BCLK-derived clock registers `0xb5` / `0x18`, writes DAC volume `0xef`, and
combines that with its own software scaling and 44.1 kHz speaker configuration.
It enables G3, waits 10 ms, programs the codec, then enables G10. The pinned
[UIFlow board initialization](https://github.com/m5stack/uiflow-micropython/blob/619e8d1897abb9cd7636e76325ae0fb5cf975ded/m5stack/boards/M5STACK_StopWatch/board_init.c)
selects MCLK, opens the codec at 48 kHz and requests volume 60; its initial IDF5 I2S
configuration uses 16 kHz stereo slots. These are not interchangeable volume or
sample-rate baselines. [Pinned source hashes and comparison](vendor-reference.json).

Our `use_mclk=false` differs from UIFlow but agrees with M5Unified's use of BCLK.
The pinned driver explicitly supports this mode with an eightfold pre-multiplier:
nominal 24 kHz × 32-bit stereo frame × 8 gives 6.144 MHz, or 256 times the sample
rate. This is a configuration calculation, not a clock measurement. There is no
basis to declare the current clock mode defective or flip it automatically.

Independent review confirmed the requested-100 finding, the ignored volume/mute
callback results and the vendor pin/enable contract. Root also checked immutable
vendor snapshots and qualified the initial clock-mismatch hypothesis against the
second official implementation and the pinned driver's BCLK support. Physical
rails, register values, amplifier output and speaker health remain unmeasured.
No install-ready candidate or new device approval is claimed.

## Next comparison, before another installation request

Prepare a bench-only readback candidate using the same preserved alarm state and
known clip. Keep existing gain, mute policy, pins, audio ownership and persistent
state unchanged. No new instrumented image is built or signed by this audit.

1. After codec activation, capture requested volume, the real ES8311 volume register
   `0x32`, mute bits in `0x31`, clock registers `0x01` / `0x02`, and each read's
   success/failure. Read cached targets
   and actual registers separately; a failed read must never become a zero value.
2. Capture M5IOE1 mode, drive, output-latch and input-level evidence for pins 3 and
   10 through the existing named macros (API indices 2 and 9), with checked return values. Label this as expander evidence, not measured
   amplifier supply voltage.
3. Associate snapshots with the current playback generation and existing
   start/write/drain counters. Serialize reads with codec lifecycle, retain data
   under the owning mutex, then publish outside audio/I2C locks. Do not add serial
   writes inside the audio loop. This is an output diagnosis, not a latency test.
4. Compare readback with the pinned configuration and vendor board reference.
   A volume/mute or expander mismatch directs a specific software investigation.
   Retain BCLK-derived mode for the first readback. An MCLK-based comparison would
   change only that variable in a separately reviewed candidate and verify actual
   clocks; it is not a presumed fix. Matching registers with weak sound leave the
   analog path unproved; the next
   step would be a separate measured codec/amplifier/speaker comparison.
5. Review the candidate and exact preservation/restoration package before asking
   Dennis for another supervised window. Use a fresh full backup and app-only
   overlay, preserve current NVS/journal, and restore normal voice from a new
   post-bench backup before ending that later session. Do not reuse today's
   installation authorization.

The completed retest remains [the physical evidence](../device-retest/README.md):
44 clips had clean decode/submission/drain, and Dennis confirmed sound was present
but far too quiet. Normal a84 is restored and all readers are closed. No device,
phone, provider, firmware build or runtime change occurred during this audit.
