# Orbit one-way capture: ready for supervised test

The narrow adapter is built, signed and independently reviewed. Hold Talk to
dictate, then release: after nonempty audio is durably retained, Orbit shows
**RECORDED / ON ORBIT**, plays a nonverbal earcon and gives a short haptic.
It does not wait for or speak a conversational reply. The main Shopping/Wanted/
Reminders architecture, persistence, statuses and slice order are unchanged.

## Exact candidates

| Role | Source | Signed application SHA-256 |
| --- | --- | --- |
| Capture cue | b608ee43089acd8a19a5393c36a3a5c2a44e4854 (author 3d02ac4) | 75a36af07e55a2e7a81e5f6fcb16538881055d2d580dab1e0dc7bbc0bbf7c4c7 |
| Compatible fallback, no cue patch | 45772685c082a55ef26673da078979a96facdbf6 | 146d37fa69c571d23327c6581e56cdce7280b9e1873fb3bdffe768bdab2b22b0 |

Both use the exact capture profile, accepted 16 MiB layout, validation, rollback
and existing signed-update public key. Both are 3,084,288 bytes and occupy only
0x020000–0x311000. The fallback is built and verified, but has not itself passed
physical acceptance. The partition file is 3,072 bytes; its hash matches the
accepted 4,096-byte device sector only after documented FF padding.

The main candidate passed all **307 host tests**, the IDF 6.0.2 build and RSA
verification. The fallback passed the same profile build and RSA verification.
The source reviewer approved the cue's durability boundary, stale-press handling,
nonblocking haptic deadline and timer priority. Hardware timing and feel are not
proved by those tests. See the two manifests and review.json for exact evidence.

## Gateway activation

The companion package in provisions-voice-gateway is
`docs/orbit-capture-adapter-2026-09-09`. It pins image
`sha256:1fe9e691c560644aee7985bbda2b1307839d13b70e67441ee8a2cf15eef54a10`
and an exact source overlay on the existing dependency image. Its 48 dictation
tests and actual-image health/Opus checks passed. Read-only verification found
the backend dictation functions already deployed; the live gateway lacks them.
Only that gateway image changes. Environment, proxy, schema and flags stay as
currently configured. Live negotiation and capture remain untested.

## Supervised test and recovery

Use `installation-proposal.json` for exact artifact paths, commands and gates.
Codex lead owns the only serial connection. A fresh full-device backup, selector,
layout and current NVS audit precede the app-only overlay. Verify the whole
expected flash before boot. Preserve all data outside the app span; never flash
an old whole-device backup over new data.

For this guided test: double blue enters dictation; single blue requests Start.
The lead verifies the real Start ACK, since the actual RoundLcd currently has no
readiness panel. Then hold yellow, dictate a shopping note and release. Check
Recorded, earcon and haptic without speech. After the existing journal receipt,
repeat a prep/work note. These are retained dictation records; this test does
**not** prove a canonical shopping/work save or Apple Reminders sync. Those
receipts remain separate gates in the unchanged main plan.

The compatible 457 fallback preserves and processes new ORBAUD03/v3 slots.
Historical a84 reads only ORBAUD02/v2: it quarantines new slots and cannot replay
them. Therefore do not automatically restore a84 after new captures. Retain an
F3-compatible firmware and gateway intake until terminal receipt cleanup. Any
fallback overlay uses a fresh current backup. A successfully accepted candidate
may remain installed; the old speaker retest's mandatory a84 restoration does
not apply to this different firmware test.

Timer behavior remains accepted for its milestone. Speaker diagnostics are
paused. Nothing in this package claims a new physical pass, complete list
functionality, phone sharing, notification support or a 48-hour soak.
