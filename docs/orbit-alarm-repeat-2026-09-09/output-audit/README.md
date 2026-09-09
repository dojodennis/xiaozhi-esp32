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

## Review status

The source and prior-snapshot audit is complete. The independent C152 vendor
contract comparison and review of this proposed readback remain pending. The
board pin and voltage statements above describe repository configuration; they
are not a newly verified schematic or physical measurement. No install-ready
candidate or new device approval is claimed.

## Next comparison, before another installation request

Prepare a bench-only readback candidate using the same preserved alarm state and
known clip. Keep existing gain, mute policy, pins, audio ownership and persistent
state unchanged. No new instrumented image is built or signed by this audit.

1. After codec activation, capture requested volume, the real ES8311 volume register
   `0x32`, mute bits in `0x31`, and each read's success/failure. Read cached targets
   and actual registers separately; a failed read must never become a zero value.
2. Capture M5IOE1 mode, drive, output-latch and input-level evidence for pins 3 and
   10 with checked return values. Label this as expander evidence, not measured
   amplifier supply voltage.
3. Associate snapshots with the current playback generation and existing
   start/write/drain counters. Serialize reads with codec lifecycle, retain data
   under the owning mutex, then publish outside audio/I2C locks. Do not add serial
   writes inside the audio loop. This is an output diagnosis, not a latency test.
4. Compare readback with the pinned configuration and vendor board reference.
   A volume/mute or expander mismatch directs a specific software investigation.
   Matching registers with weak sound leave the analog path unproved; the next
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
