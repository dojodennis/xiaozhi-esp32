# Orbit reply-ring motion validation

## Outcome

Orbit's approved crest remains the calm idle face. Listening, thinking and
speaking retain its center star and use three fixed rings with slow opacity-only
motion. Microphone sampling, Talk control, state mapping and command handling are
unchanged.

## Before

The removed speaking formula expanded each ring by up to 70 pixels over 0.9
seconds and then reset it in one frame. At 30 Hz, that produced a measured
68-pixel wrap jump. Listening used a 4 Hz edge wobble, only about 7.6 display
frames per cycle. Speaking time also advanced only while fresh playback samples
were present, so normal audio gaps froze the motion. Resizing and centering all
three LVGL arcs on every frame amplified the visible instability.

## After

- Ring radii stay at 120, 154 and 184 pixels: zero geometry delta across frames.
- Listening uses a shared 2.4-second breath; speaking uses a 2.2-second
  phase-shifted luminance wave; thinking uses a 2.8-second wave.
- Voice energy changes opacity only, and the speaking clock continues through
  natural PCM gaps.
- Adjacent 33 ms speaking frames are contract-tested at no more than eight alpha
  steps, including the old 899/900 ms reset boundary.
- The center star remains at 224/255 opacity, above the rings, while a 360 ms
  smootherstep transition blends crest, rings and color.
- LVGL ring layout is updated only if geometry actually changes; normal active
  frames now update color and opacity only.

## Evidence

The deterministic host preview compares the removed formula with the new motion
contract at the display's 30 Hz cadence:

- `docs/orbit-reply-ring-motion-before-after.gif`
- `docs/orbit-reply-ring-motion-storyboard.png`

Regenerate both artifacts with:

```sh
python3 scripts/render_orbit_reply_motion.py
```

The preview uses the repository's byte-verified crest masks and the same motion
constants as `crest_motion.h`. It isolates ring behavior with a constant audio
level; it is not an AMOLED framebuffer capture.

Host tests cover fixed geometry, audio response, frame-to-frame continuity,
transition continuity and unchanged Talk/audio wiring. A successful board build
validates the LVGL integration. No device was flashed for this change, so the
remaining acceptance step is a supervised install followed by an on-panel
listening/reply observation.

## Verification

- Targeted Orbit motion, StopWatch and Talk-interruption tests: 17 passed.
- Complete firmware host suite: 144 passed.
- Provisions StopWatch bench build: 2208/2208 steps passed with ESP-IDF 6.0.2;
  the application partition retains 27% free space.
- `xiaozhi.bin` SHA-256:
  `b38c8860759fa68ccc6c90e3e216ab40726eafd6ba849d5becbb54676476672f`.
- Independent code review: no actionable findings.
