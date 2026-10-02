# ORBIT-STOCK-01 — Dedicated onboard stock entry

## ORBIT-THINK-01 — Command progress, 2 October 2026

Owner: Codex. Dennis authorized implementation and offline release preparation.
Outcome: the existing processing animation also says “Thinking” while a command
is pending. Saving, queued retry and microphone preparation retain accurate
labels. Related: ORBIT-STOCK-01 and OS-06; based on the reviewed Stock/Service
candidate plus host-failure repairs at `1a48c5e`, not a replacement firmware base.
Exclusions: protocol, retries, provider calls, transcripts, device flashing and
Claude's EN visual design. Main integration remains separate from this candidate.

Acceptance: production status-method transitions, same-state caption refresh,
cancel/error/reply exit, real LVGL/font rendering without clipping, full host
suite, accepted QR-enabled target build, independent review, pushed source and
verified offline package. Physical Ring 2 readability and microphone acceptance
remain with Dennis when the existing connected-device handoff is available.

Current: source reviewed GO; 357/357 host tests, production status-method
ASan/UBSan checks, native LVGL caption/menu checks and the QR-enabled StopWatch
build pass. Independent reviewers: `fix_security_review` (security/privacy and
provider effects) and `fix_simplicity_review` (display idioms, complexity, cruft).
The native caption sizing check caught a two-line microphone caption exceeding
74 points; the reviewed single-line “Preparing mic” fits. Historical
350/354 results below are superseded by the 1 October repair checkpoint's
356/356 host pass; they are retained as the original Stock evidence.

Change log — 2 October 2026 — ORBIT-THINK-01 — Codex: add allowlisted progress
captions in the existing StopWatch/C152 display paths and refresh them when
Saving changes to Working without an animation-state change. Final host run:
357 passed in111.5s; native LVGL/fonts fit all four progress captions. Changed
C++ lines formatted with repository clang-format settings. No provider call,
protocol/retry change or device operation. Release package records the exact
committed build and offline signature; installation remains deferred.

## Historical Stock implementation checkpoint — 30 September 2026

Current: implemented; navigation and native rendering pass; first target build passes. Owner: Codex. Related: ORBIT-ADD-01 and
shared-line tool parity row 4. Dennis approved installation on 30 September 2026.
The prior Home-only stock test instructions are superseded by this menu flow.

Outcome: fourth menu entry Stock, in the existing crest style. Blue opens the
menu; swipe to Stock, tap yellow to open, hold yellow to ask and release to send;
blue returns to the menu. Reuse the existing general voice question and scoped
stock lookup, without List/Notes capture flags or a second inventory.

Exclusions: no stock writes, reassignment, gateway deployment, merge or Ring 1
installation. Current approved target remains M/Y MONACO DEMO. The existing
three illustrated theme pages remain; Stock uses the crest in both themes.

Acceptance: execute production navigation methods through all four pages and
both directions; verify Stock has neither List nor Notes capture intent; native
render Stock and check timer/menu regressions; build QR-enabled StopWatch with
known dependency/configuration; inspect signed flash size; guarded Ring 2 app-only
install with fresh full backup and whole-device verification. Source self-check,
independent review, installation, boot and physical answer acceptance are separate.

For Dennis (after installation): normal application mode, blue → swipe to Stock →
tap yellow. Hold yellow, ask “How much yuzu juice do we have on board?”, release.
Expected recorded demo stock: three units of Yuzu Juice 1 L, last updated 22 September.
Report spoken answer and whether blue returns to the menu. Blocks physical
menu/stock acceptance. No phone app is needed.

Change log — 30 September 2026 — ORBIT-STOCK-01 — Codex: Dennis requested a
separate stock menu and authorized installation. Added fourth navigation target
and native crest presentation; existing voice lookup is unchanged. Checks and
installation pending. Shared authority/evidence: Provisions docs/orbit-product-context.md.

Change log — 30 September 2026 — ORBIT-STOCK-01 — Codex: production navigation
methods pass queued four-page cycling, both swipe directions, Stock confirmation,
blue return and Wi-Fi gesture ownership under ASan/UBSan. The native LVGL renderer
passes Stock/Home geometry and voice-caption restoration plus existing menu/timer
checks. ESP-IDF 6.0.2 StopWatch build passes. Full host suite and exact clean-commit
rebuild are being finalized. No independent review or hardware acceptance claimed.

Validation snapshot: 350/354 full host checks pass. The same four pre-existing
failures recorded on the installed/base firmware remain (audio-output ownership
assertion, Lite erasure count, obsolete upload harness symbol, incomplete blue
button harness). No new full-suite failure. Native Stock/Menu and caption
restoration checks pass; Stock exits expanded timer details and keeps ringing
alarm priority. Self-review only; independent review/integration remain open.
