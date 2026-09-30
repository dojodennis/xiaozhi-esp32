# Orbit Wi-Fi QR validation

Workstream: WIFI-05, owner Codex. [Project plan](https://github.com/dojodennis/provisions-ios/blob/codex/orbit-wifi-qr-review/docs/PLAN-orbit-wifi-qr-2026-09-26.md).

## Current snapshot — 30 September 2026

Dennis accepted the iPhone Camera QR join and captive-page setup flow after the
readable, nonhex password correction in `106641c`. He reported the reconnecting
message and return to the normal face. He then requested a separate check that
Orbit uses the selected Wi-Fi, rather than a previously saved network.

The approved follow-up now prioritizes the first persisted profile for Provisions
builds. Saving a new network already inserts it first; reselecting an existing
network now moves it first in the same persistence operation. The station tries
that profile before stronger alternatives, preserving signal order within the
preferred network and among fallback networks. If absent or unable to connect,
existing bounded retries proceed to the other saved profiles. This preference
applies on connection/reconnection; an established fallback is not interrupted
merely because the preferred network later returns.

A credential-free diagnostic distinguishes preferred from fallback connections.
The selected SSID can be verified privately against the persisted first profile;
network names and passwords remain absent from the diagnostic. Physical restart,
selected-network retention and fallback acceptance remain pending on this change.

Blue cancellation, voice after provisioning, expiry, and other outstanding
checks in the project plan remain separate from this accepted phone flow.
The PR remains unmerged.

## Evidence

- Before `106641c`, Camera joining failed while entering the displayed password
  through Settings worked. The correction uses a 16-character password with a
  nonhex first character. This removes QR-format ambiguity; the exact iPhone
  parser failure was not independently proven.
- Nine focused Wi-Fi tests and the ESP-IDF v6.0.2 build passed for the correction.
- User report: Camera QR join, captive page, reconnecting message and return to
  normal all worked in the requested test.
- Connection test: `components/esp-wifi-connect/wifi_configuration_ap.cc`,
  `ConnectToWifi` and `IpEventHandler`; saving is gated in `orbit_wifi_portal.cc`.
- Subsequent saved-network selection:
  `components/esp-wifi-connect/wifi_station.cc`, `HandleScanResult`.

## Change log

- **2026-09-29 — WIFI-05 — Codex:** Saved Dennis's phone-flow acceptance for
  `106641c`, distinguishing it from verification of the network used afterward.
  Recorded the existing strongest-saved-network behavior so acceptance cannot
  silently imply a network switch. Documentation-only change; no firmware change.
  Next action: confirm the selected SSID and whether it differs from the prior
  network, then verify the active network. Detailed installation evidence stays
  in the private workstream record.

- **2026-09-30 — WIFI-05 — Codex:** Implemented Dennis's approved preference for
  the selected setup network. Existing selections move first, including password
  updates, with in-memory rollback on a failed save. Persisted ordering uses the
  existing NVS schema. Provisions scans prefer that network ahead of stronger
  alternatives; other builds retain signal-first selection. Added private-safe
  preferred/fallback connection diagnostics. Eleven focused Wi-Fi tests passed,
  including actual persistence/load code, reselection, failed-save rollback,
  stronger competing networks, missing preference, multiple access points and
  the non-Provisions path. ESP-IDF v6.0.2 target build passed. Physical acceptance
  is separate; blue cancellation remains unverified. No merge authorized.
