# Supervised alarm retest: sound remains too quiet

Dennis's 9 September retest reproduced inadequate alarm volume. He first described
silence, then clarified that some sound was present but far too quiet. He also
reported consistently weak normal voice. The corrected finding supersedes absolute
silence. **Acoustic usability fails; the cause is not yet isolated.**

Normal a84 voice was restored before the session ended. Its complete expected
16 MiB image was verified, startup authenticated, and Dennis heard “I'm here, what
do you need?” at 18:05:08 UTC. He said it was still not loud enough. All Orbit
serial readers closed normally by 18:05:50 UTC. The installed phone was untouched.

## What the diagnostic established

The exact signed diagnostic `ff8a26d6…1ebca` booted at 17:53:57 UTC with ELF prefix
`497ba506d`. It used the existing five due alarms and Rice's pending acknowledgement.
No yellow/blue gesture, new acknowledgement, seed or erase was performed. The first
45-second observation completed; the optional second restart was omitted when
Dennis requested restoration. The full passive capture contains 44 complete clips
across about 85 seconds of trace events, not 44 independent alarm cycles.

An independent reparse of the raw console found:

| Measurement | Result |
| --- | --- |
| Ordered trace events | 132: start, first write and drain for each of 44 clips |
| Decoded / written packets per clip | 15 / 15 |
| Decoded / submitted samples per clip | 21,600 / 21,600 at the configured 24 kHz |
| Output PCM peak | 24,380, approximately −2.57 dBFS |
| Output PCM RMS | 9,387.87, approximately −10.86 dBFS |
| Decode/output errors, drops, failed writes and lost trace events | 0 |
| Gap from drain to the next start | 1,013–1,038 ms; mean 1,022.65 ms |
| Physical alarm | Some sound, far too quiet |
| Restored normal voice | Intelligible, still not loud enough |

[Raw counter lines](bench-1-counters.log), [parsed events and clips](bench-1-trace.json),
[independent trace review](trace-independent-review.json),
[corrected physical observation](physical-sound-correction.json), and
[final normal-voice observation](restored-voice-physical-observation.json).

This proves repeat decoding, driver submission and drain completion for this
capture. It does not prove acoustic output strength or isolate gain, mute state,
amplifier, codec, routing or the physical speaker. The roughly one-second gap is
intentional. No clip, gain, PA setting or playback policy was changed by this
instrumentation. The earlier host decode evidence and the compiled-out output-fence
finding remain valid; they need not be re-diagnosed.

## Data preservation and recovery

Dennis explicitly approved the new supervised session. Root alone owned the
verified ESP32-S3 v0.2 / 16 MiB device, MAC `28:84:85:44:6b:10`. Independent reviews
preceded both writes. Only `0x020000–0x311000` changed, first to the exact diagnostic
and then to the accepted a84 application. Both complete expected images were
verified before their deliberate boot boundaries.

The initial backup needed a reviewed transport adaptation: retain two complete
1 MiB reads and assemble the remaining range from 56 successful 256 KiB reads.
Two interrupted 1 MiB attempts and two interrupted 256 KiB attempts were excluded;
each interruption was followed by a successful identity check of the same running
stub without reset. The new post-bench backup used 64 complete 256 KiB reads with
no failed attempt. Both phases have exact contiguous coverage and whole-device
digest verification. No reads were mixed across phases or resets.

| Full image | SHA-256 |
| --- | --- |
| Fresh pre-install backup | `017c6360634660d5ac2489ddb5afac1cc1ecfdc144420b54ab0c97fa323999be` |
| Verified installed image / fresh post-bench backup | `e9bbafae5dccbe1ee0da65033c46469946c5f69a2e1c14b542aa5ab18399b44b` |
| Verified restored image | `017c6360634660d5ac2489ddb5afac1cc1ecfdc144420b54ab0c97fa323999be` |

The restore image was constructed from the **new post-bench backup** and the
accepted application. Its equality with the initial backup follows from unchanged
data; no old whole-device image was written. Partition MD5, selected VALID ota_0
sequence 1 / CRC `0x4743989a`, NVS integrity, all 16 unrelated logical records,
bootloader, keys, selectors, app remainders, assets and capture journal were
preserved. The frozen codec accepts the unchanged 356-byte bench state
`7f64b646ba2f2748d0a89d7240f38ae7f6d452a1c39e5c73f6938761574daae9`.

[Install review](install-independent-review.json), [restore review](restore-independent-review.json),
[recovery completion](restoration-complete.json), and [artifact hashes](evidence-manifest.json).
Raw flash, NVS values, credentials and the normal runtime console remain private.
Existing 317 host checks and both final builds were retained; this report changes
no runtime code and does not claim a new software fix.

## Next bounded investigation

Codex owns an offline audit of the shared output path: compare the accepted a84
and diagnostic ES8311 volume, mute, amplifier-enable and output configuration
against the actual C152 board contract. Establish which software settings are
applied and which physical stages have not been measured. Then prepare a reviewed
comparison using one known signal and explicit gain/amplifier/codec observations
before attributing the fault to the speaker or proposing replacement hardware.

No additional device action was requested at the end of this session. Leave the
restored board available for ordinary use; any later instrumented installation
needs its own concrete review and supervised window. Alarm strength, apron haptics,
voice-created timers, hotspot/battery behavior and the fixed-candidate 48-hour soak
remain unpassed. Timers stay first; shopping and prep/work lists each need both
quick capture and reliable deliberate app sharing. Notifications follow those core
three capabilities.
