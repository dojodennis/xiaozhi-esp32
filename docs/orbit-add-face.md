# ORBIT-ADD-01 — Keep List/Notes capture on the face

Current: implemented, rendered and built; awaiting review/visual acceptance. Codex owner. Dennis requested this on
30 September 2026 as part of the existing Orbit menu workstream. Related:
MENU-KV-01/02 and WIFI-05. Based on menu candidate 4f96977 (including Wi-Fi
3519f4b); installed Ring 2 remains corrected 96888cc. This is not installed.

Outcome: empty List and Notes retain the exact Home crest and quiet destination
label. Press yellow and speak, release to send, using the existing recording path.
The short pocket-lock chord window remains; no long-press gate is introduced.
Menu yellow five-second Wi-Fi entry, filled lists, timers and lock stay unchanged.
Tap-to-toggle recording is an optional question pending Dennis's answer; default
is the existing press/hold/release interaction. No audio-pipeline changes planned.

Acceptance: render actual production assets/fonts and compare Home geometry;
verify empty/list/empty and capture transitions; run button/chord/Wi-Fi checks;
build the StopWatch firmware with verified QR configuration and dependency lock;
inspect flash headroom. Independent review and hardware acceptance remain separate.
No merge or installation in this task. Keep existing physical tests pending.

For Dennis: no device action yet. Inspect the rendered preview when ready;
after installation, verify empty List and Notes retain the crest and yellow starts
recording without another confirmation. Hardware acceptance is pending.

Change log — 30 September 2026 — Codex — ORBIT-ADD-01: began replacement of the
blank hold-to-add screen with the existing face crest and a small capture cue.

## Engineering evidence — 30 September 2026

![Home, empty List and empty Notes](images/orbit-add-face.png)

- Native LVGL uses the actual renderer, crest assets and production fonts. Pixels
  outside the two caption rows match Home exactly. List/Notes/re-entry restores
  full crest opacity; existing six-menu-page, timer/removal and switch checks pass.
- Eleven Wi-Fi tests, three crest tests, two button-chord tests and one theme
  test pass. The actual yellow callback test verifies recording starts after the
  existing 150 ms chord window outside the menu, stops on release, and retains
  the separate five-second Wi-Fi gesture and pocket lock.
- ESP-IDF 6.0.2 StopWatch build passes with CONFIG_LV_USE_QRCODE=y, the verified
  Wi-Fi sdkconfig and unchanged dependency lock (dl_fft 0.7.0). QR create/update
  symbols are linked. Unsigned image 3,997,696 bytes; projected signed size
  4,001,792, leaving 126,976 bytes in ota_0. No signing or flashing performed.
- Self-checked by Codex. Independent review and physical microphone/display
  acceptance are pending. Focused checks do not resolve the four previously
  recorded broad-suite failures on the base candidate.

Change log — 30 September 2026 — ORBIT-ADD-01 — Codex: replaced the empty prompt
with the unchanged Home crest, a List/Notes label and quiet yellow-button cue.
Removed the separate animated arrow on this empty screen; filled lists restore
normal text layout. Existing press-to-record/release-to-send behavior remains.
Rendered actual pixels, passed 17 focused checks and built the QR-enabled target.
Source branch: codex/orbit-add-face, based on 4f96977. Not installed or merged.
