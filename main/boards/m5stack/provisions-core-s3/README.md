# Provisions Kitchen Helper CoreS3 family

Gate 1 has one maintained firmware architecture and two non-interchangeable
hardware/OTA identities:

| Build identity | Intended use | U027 white wire | Battery/base |
| --- | --- | --- | --- |
| `provisions-kitchen-helper-core-s3` | first sacrificial bench test | Port.B / GPIO8 | 500 mAh DIN base; bench only |
| `provisions-kitchen-helper-core-s3-lite` | later client rollout | Port.A / GPIO1 | internal 200 mAh; no DIN base |

Both products use the same internal microphone, speaker, 320x240 display,
AXP2101 PMIC, AW9523B IO expander and audio/display pin map. The build guard
changes only the external Talk pin and physical profile metadata. Hold the
active-low M5Stack Unit Button U027 to talk and release it to end the turn.

Touch, wake word and every camera software path are unavailable. The GC0308 is
never initialized, its reset remains asserted by the IO expander, and OTA URLs
must contain the exact compiled build identity. Fit an opaque physical lens
cover as an additional visible privacy control; it is not a substitute for the
firmware guard.

Kconfig, CMake `BOARD_NAME`, the signed application identity marker and the OTA
path are bound to the same profile. Factory and pre-flash verification reject a
mutable `sdkconfig.h` sidecar that attempts to relabel a signed application.

For a fixed kitchen installation, use protected, continuously rated USB-C 5 V
power with cable strain relief. Treat either battery as short backup only. The
Lite magnetic back cover can attach to a removable non-conductive bracket, but
must remain outside splash, steam and direct heat zones with room for the side
Port.A cable. Do not add the DIN base to the client enclosure.

The build is locked to the preview namespace at
`app.provisions-app.com/kitchen-helper/preview/v1`. It accepts the device bearer
only from the untracked `provisions.device_token` NVS value. Do not put a token,
Wi-Fi SSID or Wi-Fi password in this repository.

Build both profiles with ESP-IDF 6.0.2:

```sh
python3 scripts/build.py m5stack/provisions-core-s3 \
  --name provisions-kitchen-helper-core-s3 \
  --language en-US \
  --wake-word disabled

python3 scripts/build.py m5stack/provisions-core-s3 \
  --name provisions-kitchen-helper-core-s3-lite \
  --language en-US \
  --wake-word disabled
```

## Reversible bench-only provisioning

`bench_profile.json` is an explicit, temporary acceptance path for the first
physical CoreS3/Lite and StopWatch loop. It is not the pilot security profile.
It keeps the exact Provisions hardware identity, gateway-only/manual-Talk UX,
camera exclusion and 16 MB dual-OTA layout, disables every boot image-validation
skip option, and requires externally signed app images for OTA verification. It
deliberately disables hardware Secure Boot, flash encryption, NVS encryption and
anti-rollback eFuse use. It contains no eFuse enablement path.

Build a CoreS3 family image with the dedicated profile, never `config.json`:

```sh
python3 scripts/build.py m5stack/provisions-core-s3 \
  --config bench_profile.json \
  --name provisions-kitchen-helper-core-s3-lite \
  --language en-US \
  --wake-word disabled
```

Use `provisions-kitchen-helper-core-s3` only for the matching full CoreS3. Sign
the secure-padded application outside the build environment with the approved
RSA-3072 key. The bundle tool verifies that signature and the exact public-key
hash from the factory registration output. It also verifies the build metadata,
app hardware marker, all independently supplied artifact hashes and the exact
partition binary. An ordinary development build, secure-pilot build, wrong-board
build, unsigned app or any build that enables an eFuse-backed security feature
is rejected.

Package the externally signed application, unsigned bench bootloader, partition
table, OTA data, assets, `project_description.json` and `config/sdkconfig.h` in
the same artifact-directory layout documented in the secure factory section
below. The approved hash record must describe these final packaged bytes.

The factory registration tool's private schema-v2 `device.json` can be consumed
directly. It and the output must remain outside this repository. The input must
be operator-owned mode `0600`; the output parent must be private and the output
must not already exist. A separately retained, verified full 16 MB factory
backup is mandatory before the tool will authorize an erase. Supply its absolute
path and independently recorded lowercase SHA-256; a partial dump, symlink,
wrong hash or repository-local image is rejected:

```sh
python3 scripts/prepare_provisions_reversible_bench.py \
  --input '/absolute/private/path/device.json' \
  --output-dir '/absolute/private/path/reversible-bench-bundle' \
  --recovery-image '/absolute/private/path/full-factory-16mb.bin' \
  --recovery-sha256 '<independently-recorded-lowercase-sha256>'
```

The preparation tool never touches hardware. It uses the digest-pinned ESP-IDF
6.0.2 container with networking disabled to generate and inspect the plaintext
NVS image. It emits exact read-MAC/eFuse preflights, full erase, complete flash
and per-region verify commands, plus a private pre-flash verifier and redacted
evidence. It never copies the recovery image into the bundle; private evidence
records only its external path, exact size and approved hash. Every bench
`esptool` command assumes the operator has manually entered ROM download mode
and therefore uses `--before no-reset`; on StopWatch, wait for the green
indication before running it. The `nvs_keys` partition stays erased and no
burn-eFuse command exists.

This path provides reversibility, not device security. Anyone with physical
access can extract the Wi-Fi password and device bearer from flash, replace the
bootloader, or bypass signed-OTA checks. Use it only on controlled development
hardware with a short-lived device credential. Never give it to a customer,
take it onto a live yacht, or call it a commercial pilot. Immediately after the
bench acceptance, revoke the credential, erase the entire flash, restore the
verified factory backup or approved secure firmware, and retain the evidence.

## Offline factory provisioning

The secure pilot factory flow is a single pre-first-boot write containing the
externally signed firmware set, encrypted per-device NVS data and its private
`nvs_keys` partition. Do not boot secure pilot firmware and then try to add NVS:
release flash encryption can make that UART flow unavailable. The preparation
tool does not contact Provisions, print secrets, flash hardware or modify eFuses.

Install the exact generator image once. Its immutable digest is enforced by the
tool and runtime pulls are disabled:

```sh
docker pull \
  'espressif/idf:v6.0.2@sha256:0d8c9773d48a327233f9c1d7c654ff0bcf133ae24503ea2e97a57cfe02b8cb67'
```

The release package must be produced in a clean, isolated pilot build directory
using `pilot_profile.json`, then signed outside the repository and build
container. The production private key must never enter either. Copy the signed
files and their build metadata into an approved release-artifact directory with
this layout:

```text
bootloader/bootloader.bin
partition_table/partition-table.bin
config/sdkconfig.h
ota_data_initial.bin
xiaozhi.bin
generated_assets.bin
project_description.json
```

The factory input binds that package to independently approved hashes and an
approved public verification key. Raw unsigned build output, a generic/dev
profile, an automatic in-container private-key build, a mismatched partition
table or a dirty/non-release version is rejected.

Obtain the device Wi-Fi STA MAC with a read-only `esptool read-mac` operation and
register that exact lowercase value with the backend. Prepare a JSON file outside
this repository with mode `0600`. Paste the server-issued device UUID and `pvd1`
credential; do not put secrets on the command line or in an environment variable:

```json
{
  "schema_version": 2,
  "hardware_profile": "provisions-kitchen-helper-core-s3-lite",
  "device_uuid": "<server-issued lowercase UUID v4>",
  "hardware_serial": "<lowercase Wi-Fi STA MAC>",
  "device_credential": "<server-issued pvd1 credential>",
  "wifi": {
    "networks": [
      {
        "role": "primary",
        "ssid": "<known yacht or demo network SSID>",
        "password": "<network password>",
        "band": "2.4GHz"
      },
      {
        "role": "fallback",
        "ssid": "<optional known hotspot SSID>",
        "password": "<hotspot password>",
        "band": "2.4GHz"
      }
    ]
  },
  "serial_port": "/dev/cu.<connected-CoreS3-port>",
  "firmware": {
    "version": "<canonical three-part approved release version>",
    "artifact_directory": "/absolute/path/to/externally-signed-release-artifacts",
    "sha256": {
      "bootloader": "<approved lowercase SHA-256>",
      "partition_table": "<approved lowercase SHA-256>",
      "ota_data": "<approved lowercase SHA-256>",
      "application": "<approved lowercase SHA-256>",
      "assets": "<approved lowercase SHA-256>"
    },
    "signing_public_key": {
      "path": "/absolute/path/to/approved-public-key.pem",
      "sha256": "<approved lowercase SHA-256 of the public-key file>"
    }
  }
}
```

Set `hardware_profile` to the exact signed build being prepared. Use
`provisions-kitchen-helper-core-s3` only for the sacrificial full-CoreS3 bench
unit and `provisions-kitchen-helper-core-s3-lite` for a Lite. Legacy schema 1 is
accepted only as the existing full-CoreS3 identity; all new bundles use schema
2 so the operator choice, sdkconfig board symbol and manifest must agree.

The `2.4GHz` values are operator assertions: an offline tool cannot infer radio
band from an SSID. Supply exactly one primary network and at most one distinct
fallback. Configure and test both as 2.4 GHz before boarding. Upstream
`WifiStation` selects known visible networks by signal strength, so `fallback`
describes provisioning intent, not strict connection priority while both are
visible.

Firmware versions use the same grammar as the device: exactly three decimal
components, each `0`-`65535`, with no leading zero unless the component is
exactly `0`.

Generate a new output directory outside the repository:

```sh
python3 scripts/provision_provisions_core_s3.py \
  --input '/absolute/private/path/factory-input.json' \
  --output-dir '/absolute/private/path/device-provisioning-bundle'
```

The bundle directory and nested firmware directory are private (`0700`); every
file is `0600`. It contains:

- copied, hash-bound bootloader, partition table, OTA data, app and assets;
- the approved public verification key (never a private key);
- a 16 KB XTS-AES-encrypted NVS image;
- a private 4 KB `nvs_keys` image whose content and digest are omitted from logs
  and the evidence manifest;
- a redacted evidence manifest with firmware and encrypted-NVS hashes;
- a private checksum manifest and verifier containing hashes for every flashed
  byte, the `nvs_keys` image, approved public key and partition contract;
- read-only MAC/eFuse preflights, a destructive erase command and one exact
  multi-offset `--after no-reset` flash command.

Nothing in the bundle is authorization to touch hardware. The generated runbook
requires an exact MAC match and a read-only eFuse summary first. Existing Secure
Boot/flash-encryption state, occupied security key/digest blocks, UART download
restrictions or any artifact mismatch are stop conditions. The one flash command
writes offsets `0x0`, `0x8000`, `0x9000`, `0xd000`, `0x10000`, `0x20000` and
`0x800000` without resetting afterward. First boot and eFuse enablement remain a
separate security ceremony and must be recovery-tested on a sacrificial CoreS3.
Immediately before erase, the generated private verifier must pass its complete
hash check, repeat public-key signature/version/partition verification, and print
the public-key fingerprint. That fingerprint must exactly match the separately
approved release record. The private bundle does not provide an independent
trust root.

The persistent namespace contract is deliberately explicit:

- `wifi.ssid`/`wifi.password` hold the primary network;
- optional `wifi.ssid1`/`wifi.password1` hold the fallback-labelled network;
- `board.uuid` becomes the `Client-Id` header;
- `provisions.device_token` becomes the device bearer credential;
- the hardware MAC is not stored in NVS; firmware reads it from the chip for
  the `Device-Id` header.

The device UUID and the UUID embedded in `pvd1_<key UUID>.<secret>` are
different identifiers. The latter is the server-issued credential key ID. The
generator validates both independently and never substitutes one for the other.

The NVS data image is encrypted before it leaves the tool, but the adjacent raw
`nvs_keys` image remains secret. Lost-device protection is not complete until a
controlled first boot enables hardware flash encryption and that state is
verified. Retain the per-device NVS key only in approved secret storage. A later
Wi-Fi change requires a technician-only signed maintenance/OTA path that reuses
that key; release-mode devices cannot use this original plaintext UART factory
flow after first boot. That maintenance path is not implemented here.

## Remaining physical gates

Before the first bench boot, verify the full CoreS3 Wi-Fi MAC, U027 Port.B
polarity/GPIO8, camera reset and opaque cover, display, microphone, speaker,
battery/charging, USB-C supply, reconnect behavior, short Talk releases,
rollback and recovery after interrupted OTA on the sacrificial unit.

Before any Lite client rollout, repeat those checks on a real Lite using U027
Port.A white/GPIO1. Also measure 200 mAh runtime, charging and enclosure
temperature, validate magnetic/bracket retention and cable strain relief in the
intended orientation, inspect splash/steam clearance, and prove that the Lite
accepts only its own signed OTA identity while the backend registry recognizes
that identity. No full-CoreS3 bench result substitutes for these Lite checks.
