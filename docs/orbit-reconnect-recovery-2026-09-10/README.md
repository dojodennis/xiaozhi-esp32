# Orbit reconnect recovery — source and target validation

Confirmed firmware keeps reconnecting after five failed gateway opens. Both reconnect paths use a saturating attempt counter and capped backoff: a base of at most 16 maintenance ticks plus 0–3 ticks of jitter. Connection time and maintenance scheduling add elapsed time; this is not a wall-clock latency guarantee. Worker-admission failures back off without advancing the gateway-health counter.

The existing five-failure rollback gate remains for an unverified OTA image. Authentication, local recordings, manual capture, alarms, channel cleanup and the single-worker guard retain their existing ownership.

Runtime source: `ceaec7e98594c2a8b925407fb87f5fd14ef42682`, based on `8767acf71ed31eea4485dac260ef28fff66f409a`.

Independent source review passed. The author ran all 312 host tests; the lead independently passed the four new executable/source checks, and the reviewer passed those four plus 22 core checks and one manual-retry check. The extracted maintenance and worker methods cover recovery after seven failures, pending-update rollback at five, bounded arithmetic, single-flight and failure cleanup. Physical recovery is not yet verified.

The C152 `m5stack/stopwatch` target compiled on ESP-IDF 6.0.2 with `capture_profile.json`, English and wake word disabled. Profile, SDK configuration, dependency lock and partition hashes exactly match the frozen crest build. The application source was explicitly invalidated after cache copying, and the log confirms its recompilation. No network or host USB device was available to the build.

The archived output is **unsigned build evidence**, not an installation package. See [build-evidence.json](build-evidence.json) for exact hashes. No signing, flash, live gateway change or configuration change occurred. The reviewed signed crest artifact `014e1cce…` and its pending approval remain unchanged. A future installation needs its own exact signed artifact, current backup and preservation checks; this record does not authorize a device write.
