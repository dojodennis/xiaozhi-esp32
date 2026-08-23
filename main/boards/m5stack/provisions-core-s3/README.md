# Provisions Kitchen Helper CoreS3

Gate 1 board profile for the full M5Stack CoreS3. It keeps the stock CoreS3
microphone, speaker, display, PMIC, battery reporting and charging support. It
does not initialize touch, camera or a wake word.

Connect a M5Stack Unit Button U027 to Port.B. Its active-low signal is GPIO8.
Hold the button to talk and release it to stop the microphone turn.

The build is locked to the preview namespace at
`app.provisions-app.com/kitchen-helper/preview/v1`. It accepts the device bearer
only from the untracked `provisions.device_token` NVS value. Do not put a token,
Wi-Fi SSID or Wi-Fi password in this repository.

Build with ESP-IDF 6.0.2:

```sh
python3 scripts/build.py m5stack/provisions-core-s3 \
  --name provisions-kitchen-helper-core-s3 \
  --language en-US \
  --wake-word disabled
```

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

Obtain the CoreS3 Wi-Fi STA MAC with a read-only `esptool read-mac` operation and
register that exact lowercase value with the backend. Prepare a JSON file outside
this repository with mode `0600`. Paste the server-issued device UUID and `pvd1`
credential; do not put secrets on the command line or in an environment variable:

```json
{
  "schema_version": 1,
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

This profile still requires a physical CoreS3/U027 smoke test for button
polarity, audio, battery, charging, display, reconnects and short Talk presses.
