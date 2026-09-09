# Crest restoration candidate — 9 September 2026

The calm crest and audio-responsive rings replace the regressed centered P. Dictation has a visible status/action panel; after durable local capture it briefly shows Recorded on Orbit. Raw transcripts are not rendered. The existing timer board, alarm priority, dictation journal and capture feedback remain in place.

Source b660d2e restores the approved crest motion into the current display class, preserving later timer and capture behavior. Legacy normal/reply objects remain allocated but hidden to reduce integration risk. Source8767acf adds the exact installed capture profile and corrects the audio-meter override to preserve the current codec's boolean playback result. The 35-second staged-conversation reply ownership versus 6-second crest caption remains a separate limitation; the current dictation receipt uses aligned1.8-second timing.

## Exact package

- Source: 8767acf71ed31eea4485dac260ef28fff66f409a.
- Signed application SHA-256: 014e1cce2c5d87670fe6a77f8bf6f20408593e173b12510b5dd64eb4b2211823.
- Signed size: 3,215,360 bytes; app-only span0x020000–0x331000, within ota_0.
- ELF SHA-256: 843caee8b8e1bc89a8b8a9d5d6a1426faf689a338b77f158c3d709b4e818dd3e.
- Private artifact: /Users/dojo/.codex/device-builds/orbit-capture-feedback/2026-09-09/crest/application.bin.

The author suite passed308 checks. The final source passed19 focused checks and independent source review. IDF6.0.2 compilation and RSA signature verification passed. Profile, SDK configuration, dependency lock, signing public key and partition table exactly match the installed capture package. Build had no network or USB access. Final independent artifact review also passed: every manifest hash/size, the signed prefix, embedded ELF, RSA signature and app-partition span were checked. Nothing has been flashed.

## Installation gate

Present this exact artifact and scope for supervised approval before any new firmware write. Root is the sole device operator. Take and independently verify a fresh current16MiB backup, verify device identity/partition selector and preserve current NVS and recording journals. Construct and review the exact app-only overlay for the new0x020000–0x331000 span; verify every byte outside that span remains identical. Write only the application, then verify the complete expected flash before boot. Do not use a generated merged image.

Record the new ELF and authentication at boot, then check the crest/timer/alarm transition, visible dictation Start/Resume/recording state, durable local Recorded replacement, and no transcript/readback. Haptic and earcon require actual observation. Do not repeat tonight's utterance unless a deliberate new capture is needed; the existing transcript is retained once. Canonical list saving, sync, sharing and the soak remain separate gates.

The current installed signed75a36af07e55a2e7a81e5f6fcb16538881055d2d580dab1e0dc7bbc0bbf7c4c7 is the compatible firmware fallback. Any rollback also uses a fresh current backup and keeps the F3-compatible gateway intake; never replace new data with an old whole-device snapshot or install historicala84 over pending ORBAUD03 recordings. The nominally passive serial open caused a USB_UART_CHIP_RESET tonight, so do not treat reopening it as reset-free.

No further physical presses are requested tonight. The candidate is prepared for the next supervised window. Gateway exception/reconnect improvements and staged-conversation timing remain tracked separately; this package does not claim those fixed.
