# OS-07-B-CONN: startup status and Service readability

10 October 2026. Implementation owner: existing Codex Orbit release chat
`01a0f11b-127e-70f0-a693-fbc2b909a4bc`. This is a correction to the current
connection-repair acceptance milestone. Canonical plan:
`/Users/dojo/Documents/Orbit-Service-2026-09-30/orbit-service-plan.md`.
[Existing portfolio](https://app.notion.com/p/3eb0cd892d8b81fcb4b2ee85aa36c1c0).

Dennis reports about three seconds of “Connection lost” at startup, a return to
the crest, then apparently working Service. White status/help over the crest is
unreadable. This is partial physical feedback, not a confirmed MCU restart or a
sustained-connection pass.

## Correction

The installed firmware's Service painter treats initial disconnected state as
Lost. The patch shows “Connecting to Dojo” for the first connection, bounded to
15 seconds; after that it shows “Unable to connect to Dojo”. A disconnect after
an authenticated connection still shows loss immediately, including within that
15-second window. Recovery and recording states retain precedence. Only display
state changes; reconnect intervals, recording ownership, storage, pairing and
protocol behavior remain unchanged.

Service reserves the upper portion for a half-size crest/activity rings and the
lower portion for status/help on black. The two-line sent confirmation uses the
larger font. Status wrapping and measured extents prevent clipping; shorter
instructions preserve destination paging/confirmation controls. Guest desk-review
information moves to the review heading. Leaving Service restores the original
Chef geometry immediately, including when animation is paused.

## Scope and validation

- Exact installed source base: `060be9fed6a2de6717709d41db5aa5b619470931`.
- Branch: `codex/orbit-startup-crest-20261010` in `dojodennis/xiaozhi-esp32`.
- No gateway/backend/Dojo app, branding asset, button mapping, saved recording,
  cleanup or guest-data change. Existing accepted recording/routing checks stay
  retired; OS-07-CLEAN-01 and wider identity/session checks remain separate.
- Host suite: `python3 -m unittest discover -s scripts/tests -v`.
- Actual Service/maintenance tests include startup at0/3/15 seconds, delayed
  recovery, authentication at1 second/loss at2 seconds, genuine disconnect,
  recovery precedence, retained recordings and existing reconnect behavior.
- Actual LVGL/fonts: `python3 scripts/tests/render_orbit_service.py <output>`.
  Eleven synthetic screens check text width/height, circular bounds, separation
  from artwork, and synchronous Chef geometry restoration. This extracts the
  production Service creation block and render/state methods; platform locks,
  clock and unrelated screen arbitration are stubs. It is not a device test.
- Canonical unsigned build: ESP-IDF6.0.2, `python3 scripts/build.py
  m5stack/stopwatch --config capture_profile.json
  --name provisions-kitchen-helper-stopwatch --language en-US`.
  Match the accepted SDK configuration, dependency lock, partition, profile and
  assets to the installed release before treating it as a release candidate.
- Frozen installed source and signed artifact remain untouched. Updated source,
  unsigned build, independent review, signing, installation and physical
  acceptance are separate evidence gates.

Current main does not match the installed Service lineage (fresh main
`ffbfe36f08c5e77b71de5f8f221edd830a9e400a`;1052/1 divergent commits).
A source review against the exact installed base avoids pulling unrelated
changes into this bounded release. No main merge is authorized or performed.

## Change log

- 10 Oct · Codex: reproduced startup loss classification and crest/text overlap
  from installed source; prepared the bounded correction in an isolated clone.
  Independent merge-ready reviewers covered security/billing, layout/idioms and
  complexity/cruft. They verified and prompted fixes for the89px confirmation
  in an86px label, narrowed destination-help wrapping, and paused-renderer Chef
  geometry restoration. Concrete LVGL/font checks now cover these cases.
  Local build/test/review receipts are under
  `/Users/dojo/yacht-Provisions-ios/Provisions/tmp/orbit-startup-crest-20261010`.
  Source delivery is not a new install or physical sign-off.

## Remaining acceptance

After the exact reviewed candidate is signed and installed with the existing
app-only recovery procedure under its own authorization, Dennis checks this
same Service startup once: “Connecting to Dojo” should become a readable crest
status without the initial false loss warning. Then complete the existing
90-second same-valid-shift connection/status observation. Report any actual
restart, continuing loss, mismatched phase or unreadable text. Do not re-pair or
repeat previously accepted note-save checks as part of this correction.
