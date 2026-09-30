# Owned Provisions Wi-Fi override

Source: `78/esp-wifi-connect` 3.2.2, upstream commit
`c24b97c194e6b4a1d7be0237b3c28980661cac1e`.
Registry component hash: `3f18f4242db04ea25043980bf9a9412cd83826c53f0d131ffb4343d3b0cf4568`.
The upstream manifest declares MIT; its shipped README and manifest are retained.

This directory is intentionally maintained source, not a generated component cache.
The explicit `main/idf_component.yml` override selects it. Do not patch a managed copy.

Provisions builds use the separate bounded WPA2 portal in `orbit_wifi_portal.cc`:
root/scan/submit/exit and captive detection only, validated credentials, escaped JSON,
same-host and per-session request token checks, no credential logs, and a worker-owned
exit. Successful association must obtain an IP before saving. The AP password lives
in RAM. Stock boards retain their existing web UI and open setup behavior.

Other shared corrections: create the scan timer before scanning; copy full 32-byte
SSID/64-byte PSK values safely; propagate save failures. Expiry/cancel/save ordering
and credential boundaries have native tests in `scripts/tests/test_orbit_wifi_session.py`.

Physical tests of the radio, browser, heap, and reconnect behavior remain mandatory.

28 September WIFI-04 review corrections: setup owns a temporary STA netif so
credential tests can obtain DHCP before saving. Manager lifecycle serialization
is separate from callback-state locking; IDF callback unregister never waits while
holding the state mutex. Scan callbacks are drained before deleting their timer.
DNS validates bounded questions and constructs bounded replies; shutdown joins
the worker before closing its socket or freeing the owner. Regression tests execute
the actual DNS parser, manager lifecycle and AP setup/cleanup methods with host
hardware stubs in `scripts/tests/test_orbit_wifi_review.py`.
