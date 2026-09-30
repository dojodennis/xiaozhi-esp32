# ORBIT-STOCK-01 — Dedicated onboard stock entry

Current: installed on Ring 2 as 30acebf; DOWNLOAD mode, physical reset and Stock acceptance pending. Owner: Codex. Related: ORBIT-ADD-01 and
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

Change log — 30 September 2026 — ORBIT-STOCK-01 — Codex: installed signed
30acebf on Ring 2, replacing 235d9c4. Verified exact device/security, prior app,
active slot, partition and preferred Koji profile before writing. Fresh 16 MB
recovery verified; app-only write at 0x20000 followed by app and whole-device
verification. All other flash bytes, including saved Wi-Fi, are unchanged.
Signed image 4,001,792 bytes, 126,976 bytes OTA headroom. Protected package:
orbit-stock-menu-30acebf. Device remains in DOWNLOAD mode pending one brief
physical reset. Boot/Wi-Fi and dedicated Stock menu/spoken-answer acceptance
remain open. Ring 1 unchanged; no merge or gateway deployment.
