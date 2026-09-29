# Orbit Wi-Fi QR validation

Workstream: WIFI-05, owner Codex. [Project plan](https://github.com/dojodennis/provisions-ios/blob/codex/orbit-wifi-qr-review/docs/PLAN-orbit-wifi-qr-2026-09-26.md).

## Current snapshot — 29 September 2026

Dennis accepted the iPhone Camera QR join and captive-page setup flow after the
readable, nonhex password correction in `106641c`. He reported the reconnecting
message and return to the normal face. He then requested a separate check that
Orbit uses the selected Wi-Fi, rather than a previously saved network.

The code tests the submitted SSID/password and waits for a station IP address
before saving and returning success. After setup exits, the normal station scan
orders all matching saved networks by signal strength. Therefore a successful
setup proves the selected credentials worked during the connection test, but
returning to the normal face does not prove the selected network remains in use.
A post-setup observation confirmed normal boot, Wi-Fi connection, activation and idle. The exact network after reconnection remains unverified because the privacy-preserving logs omit SSIDs.

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
