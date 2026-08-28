import hashlib
import importlib.util
import json
import os
import stat
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SCRIPT_PATH = ROOT / "scripts/provision_provisions_core_s3.py"
SPEC = importlib.util.spec_from_file_location("provisions_factory", SCRIPT_PATH)
provisioning = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = provisioning
SPEC.loader.exec_module(provisioning)

DEVICE_UUID = "50000000-0000-4000-8000-000000000001"
KEY_UUID = "60000000-0000-4000-8000-000000000001"
DEVICE_CREDENTIAL = f"pvd1_{KEY_UUID}." + ("A" * 43)
WIFI_SSID = "Factory Demo 2G"
WIFI_PASSWORD = "not-a-real-password"
FALLBACK_SSID = "Factory Backup 2G"
FALLBACK_PASSWORD = "not-a-real-fallback"


def digest(content):
    return hashlib.sha256(content).hexdigest()


def make_release_artifacts(parent, profile=provisioning.BOARD_PROFILE):
    artifact_directory = parent / "pilot-release"
    (artifact_directory / "bootloader").mkdir(parents=True)
    (artifact_directory / "partition_table").mkdir()
    (artifact_directory / "config").mkdir()
    contents = {
        "bootloader": b"approved-externally-signed-bootloader",
        "partition_table": b"approved-partition-table",
        "ota_data": b"approved-ota-data",
        "application": (
            b"approved-externally-signed-application\x00"
            + provisioning.SIGNED_HARDWARE_IDENTITY_MARKERS[profile]
        ),
        "assets": b"approved-assets",
    }
    for artifact in provisioning.FIRMWARE_ARTIFACTS:
        (artifact_directory / artifact.relative_path).write_bytes(
            contents[artifact.key]
        )
    (artifact_directory / "project_description.json").write_text(
        json.dumps(
            {
                "project_name": "xiaozhi",
                "project_version": "2.4.2",
                "target": "esp32s3",
            }
        ),
        encoding="utf-8",
    )
    (artifact_directory / "config/sdkconfig.h").write_text(
        "\n".join(
            sorted(
                provisioning.PILOT_SHARED_SDKCONFIG_DEFINES
                | {provisioning.BOARD_PROFILE_CONFIG_DEFINES[profile]}
                | provisioning.BOARD_PROFILE_REQUIRED_SDKCONFIG_DEFINES[profile]
            )
        )
        + "\n",
        encoding="utf-8",
    )
    public_key = parent / "approved-signing-public-key.pem"
    public_key.write_bytes(
        b"-----BEGIN PUBLIC KEY-----\n" + (b"A" * 256) + b"\n-----END PUBLIC KEY-----\n"
    )
    return (
        artifact_directory,
        {key: digest(value) for key, value in contents.items()},
        public_key,
    )


def valid_document(
    artifact_directory=Path("/private/approved-pilot-release"),
    hashes=None,
    public_key=Path("/private/approved-signing-public-key.pem"),
    fallback=False,
    hardware_profile=None,
):
    networks = [
        {
            "role": "primary",
            "ssid": WIFI_SSID,
            "password": WIFI_PASSWORD,
            "band": "2.4GHz",
        }
    ]
    if fallback:
        networks.append(
            {
                "role": "fallback",
                "ssid": FALLBACK_SSID,
                "password": FALLBACK_PASSWORD,
                "band": "2.4GHz",
            }
        )
    document = {
        "schema_version": 1 if hardware_profile is None else 2,
        "device_uuid": DEVICE_UUID,
        "hardware_serial": "24:6f:28:12:34:56",
        "device_credential": DEVICE_CREDENTIAL,
        "wifi": {"networks": networks},
        "serial_port": "/dev/cu.usbmodem-demo",
        "firmware": {
            "version": "2.4.2",
            "artifact_directory": str(artifact_directory),
            "sha256": hashes
            or {key: "a" * 64 for key in provisioning.FIRMWARE_HASH_KEYS},
            "signing_public_key": {
                "path": str(public_key),
                "sha256": digest(public_key.read_bytes())
                if public_key.exists()
                else "b" * 64,
            },
        },
    }
    if hardware_profile is not None:
        document["hardware_profile"] = hardware_profile
    return document


def parse(document):
    return provisioning.parse_provisioning_document(
        json.dumps(document).encode("utf-8")
    )


def expected_nvs_entries(request):
    entries = []
    for index, network in enumerate(request.wifi_networks):
        suffix = "" if index == 0 else str(index)
        entries.extend(
            [
                {
                    "namespace": "wifi",
                    "key": f"ssid{suffix}",
                    "encoding": "string",
                    "data": network.ssid,
                    "state": "Written",
                    "is_empty": False,
                },
                {
                    "namespace": "wifi",
                    "key": f"password{suffix}",
                    "encoding": "string",
                    "data": network.password,
                    "state": "Written",
                    "is_empty": False,
                },
            ]
        )
    entries.extend(
        [
            {
                "namespace": "board",
                "key": "uuid",
                "encoding": "string",
                "data": DEVICE_UUID,
                "state": "Written",
                "is_empty": False,
            },
            {
                "namespace": "provisions",
                "key": "device_token",
                "encoding": "string",
                "data": DEVICE_CREDENTIAL,
                "state": "Written",
                "is_empty": False,
            },
        ]
    )
    return entries


def inspector_for(request):
    return lambda _work_directory: expected_nvs_entries(request)


def fake_generator(work_directory):
    (work_directory / "device-nvs.bin").write_bytes(
        b"\xa5" * provisioning.NVS_PARTITION_SIZE
    )
    (work_directory / "keys").mkdir()
    (work_directory / "keys/nvs_keys.bin").write_bytes(
        b"\x5a" * provisioning.NVS_KEYS_PARTITION_SIZE
    )


def accept_signed_artifacts(
    _firmware_directory, _expected_version, _expected_profile
):
    return None


class ProvisioningInputTests(unittest.TestCase):
    def test_accepts_primary_and_optional_fallback_contract(self):
        request = parse(valid_document(fallback=True))
        self.assertEqual(request.device_uuid, DEVICE_UUID)
        self.assertEqual(request.credential_key_id, KEY_UUID)
        self.assertNotEqual(request.device_uuid, request.credential_key_id)
        self.assertEqual(
            [network.role for network in request.wifi_networks],
            ["primary", "fallback"],
        )
        self.assertNotIn(DEVICE_CREDENTIAL, repr(request))
        self.assertNotIn(WIFI_PASSWORD, repr(request))
        self.assertEqual(
            provisioning._nvs_rows(request),
            [
                ["key", "type", "encoding", "value"],
                ["wifi", "namespace", "", ""],
                ["ssid", "data", "string", WIFI_SSID],
                ["password", "data", "string", WIFI_PASSWORD],
                ["ssid1", "data", "string", FALLBACK_SSID],
                ["password1", "data", "string", FALLBACK_PASSWORD],
                ["board", "namespace", "", ""],
                ["uuid", "data", "string", DEVICE_UUID],
                ["provisions", "namespace", "", ""],
                ["device_token", "data", "string", DEVICE_CREDENTIAL],
            ],
        )

    def test_schema_two_binds_each_non_default_hardware_identity(self):
        document = valid_document(hardware_profile=provisioning.BOARD_PROFILE_LITE)
        request = parse(document)
        self.assertEqual(request.hardware_profile, provisioning.BOARD_PROFILE_LITE)

        document = valid_document(
            hardware_profile=provisioning.BOARD_PROFILE_STOPWATCH
        )
        request = parse(document)
        self.assertEqual(
            request.hardware_profile, provisioning.BOARD_PROFILE_STOPWATCH
        )

        document["hardware_profile"] = "unapproved-core-s3"
        with self.assertRaisesRegex(provisioning.ProvisioningError, "profile"):
            parse(document)

    def test_rejects_unsupported_or_ambiguous_inputs(self):
        invalid_documents = []

        extra = valid_document()
        extra["tenant_id"] = DEVICE_UUID
        invalid_documents.append(extra)

        wrong_schema = valid_document()
        wrong_schema["schema_version"] = 2
        invalid_documents.append(wrong_schema)

        uppercase_uuid = valid_document()
        uppercase_uuid["device_uuid"] = "5000000A-0000-4000-8000-000000000001"
        invalid_documents.append(uppercase_uuid)

        wrong_uuid_version = valid_document()
        wrong_uuid_version["device_uuid"] = "50000000-0000-3000-8000-000000000001"
        invalid_documents.append(wrong_uuid_version)

        uppercase_mac = valid_document()
        uppercase_mac["hardware_serial"] = "24:6F:28:12:34:56"
        invalid_documents.append(uppercase_mac)

        multicast_mac = valid_document()
        multicast_mac["hardware_serial"] = "25:6f:28:12:34:56"
        invalid_documents.append(multicast_mac)

        malformed_token = valid_document()
        malformed_token["device_credential"] = DEVICE_CREDENTIAL + "x"
        invalid_documents.append(malformed_token)

        wrong_band = valid_document()
        wrong_band["wifi"]["networks"][0]["band"] = "5GHz"
        invalid_documents.append(wrong_band)

        long_ssid = valid_document()
        long_ssid["wifi"]["networks"][0]["ssid"] = "x" * 33
        invalid_documents.append(long_ssid)

        weak_password = valid_document()
        weak_password["wifi"]["networks"][0]["password"] = "short"
        invalid_documents.append(weak_password)

        fallback_first = valid_document(fallback=True)
        fallback_first["wifi"]["networks"][0]["role"] = "fallback"
        invalid_documents.append(fallback_first)

        duplicate_fallback = valid_document(fallback=True)
        duplicate_fallback["wifi"]["networks"][1]["ssid"] = WIFI_SSID
        invalid_documents.append(duplicate_fallback)

        too_many_networks = valid_document(fallback=True)
        too_many_networks["wifi"]["networks"].append(
            {
                "role": "fallback",
                "ssid": "Third",
                "password": "password-three",
                "band": "2.4GHz",
            }
        )
        invalid_documents.append(too_many_networks)

        relative_port = valid_document()
        relative_port["serial_port"] = "cu.usbmodem-demo"
        invalid_documents.append(relative_port)

        non_release = valid_document()
        non_release["firmware"]["version"] = "2.4.2-dirty"
        invalid_documents.append(non_release)

        relative_artifacts = valid_document()
        relative_artifacts["firmware"]["artifact_directory"] = "build"
        invalid_documents.append(relative_artifacts)

        uppercase_hash = valid_document()
        uppercase_hash["firmware"]["sha256"]["application"] = "A" * 64
        invalid_documents.append(uppercase_hash)

        for index, document in enumerate(invalid_documents):
            with (
                self.subTest(case=index),
                self.assertRaises(provisioning.ProvisioningError),
            ):
                parse(document)

    def test_rejects_duplicate_json_fields(self):
        raw = (
            '{"schema_version":1,"schema_version":1,"device_uuid":"'
            + DEVICE_UUID
            + '"}'
        ).encode("utf-8")
        with self.assertRaisesRegex(provisioning.ProvisioningError, "duplicate"):
            provisioning.parse_provisioning_document(raw)

    def test_version_contract_matches_firmware_boundaries(self):
        for accepted in ("0.0.0", "2.4.2", "65535.65535.65535"):
            document = valid_document()
            document["firmware"]["version"] = accepted
            with self.subTest(accepted=accepted):
                self.assertEqual(parse(document).firmware.version, accepted)

        for rejected in (
            "02.4.2",
            "2.04.2",
            "2.4.02",
            "65536.0.0",
            "99999.0.0",
            "1.2",
            "1.2.3.4",
        ):
            document = valid_document()
            document["firmware"]["version"] = rejected
            with (
                self.subTest(rejected=rejected),
                self.assertRaises(provisioning.ProvisioningError),
            ):
                parse(document)

    def test_private_input_must_be_outside_repo_owned_and_mode_0600(self):
        with tempfile.TemporaryDirectory() as temporary:
            input_path = Path(temporary) / "factory-input.json"
            input_path.write_text(json.dumps(valid_document()), encoding="utf-8")
            input_path.chmod(0o600)
            self.assertEqual(
                provisioning.load_private_input(input_path).device_uuid, DEVICE_UUID
            )

            input_path.chmod(0o640)
            with self.assertRaisesRegex(provisioning.ProvisioningError, "0600"):
                provisioning.load_private_input(input_path)

            input_path.chmod(0o600)
            link_path = Path(temporary) / "linked-input.json"
            link_path.symlink_to(input_path)
            with self.assertRaises(provisioning.ProvisioningError):
                provisioning.load_private_input(link_path)


class ProvisioningBundleTests(unittest.TestCase):
    def make_request(self, temporary, fallback=False, hardware_profile=None):
        profile = hardware_profile or provisioning.BOARD_PROFILE
        artifact_directory, hashes, public_key = make_release_artifacts(
            Path(temporary), profile
        )
        document = valid_document(
            artifact_directory,
            hashes,
            public_key,
            fallback=fallback,
            hardware_profile=hardware_profile,
        )
        return parse(document), artifact_directory, public_key

    def generate(self, request, output_directory):
        return provisioning.generate_bundle(
            request,
            output_directory,
            generator=fake_generator,
            schema_inspector=inspector_for(request),
            firmware_verifier=accept_signed_artifacts,
            now=lambda: datetime(2026, 8, 23, 12, 0, tzinfo=timezone.utc),
        )

    def test_runtime_safety_defines_are_explicit_and_required(self):
        required_runtime_defines = {
            "#define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1",
            "#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1",
            "#define CONFIG_WAKE_WORD_DISABLED 1",
        }
        self.assertTrue(
            required_runtime_defines.issubset(provisioning.PILOT_SDKCONFIG_DEFINES)
        )

        core_profile = json.loads(
            (
                ROOT
                / "main/boards/m5stack/provisions-core-s3/pilot_profile.json"
            ).read_text(encoding="utf-8")
        )
        stopwatch_profile = json.loads(
            (
                ROOT / "main/boards/m5stack/stopwatch/pilot_profile.json"
            ).read_text(encoding="utf-8")
        )
        required_settings = {
            "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y",
            "CONFIG_PROVISIONS_GATEWAY_REQUIRED=y",
            "CONFIG_WAKE_WORD_DISABLED=y",
        }
        self.assertEqual(
            {build["name"] for build in core_profile["builds"]},
            set(provisioning.CORE_S3_BOARD_PROFILES),
        )
        self.assertEqual(
            {build["name"] for build in stopwatch_profile["builds"]},
            {provisioning.BOARD_PROFILE_STOPWATCH},
        )
        for build in core_profile["builds"] + stopwatch_profile["builds"]:
            profile_settings = set(build["sdkconfig_append"])
            self.assertTrue(required_settings.issubset(profile_settings))
            self.assertIn(
                provisioning.BOARD_PROFILE_SDKCONFIG_OPTIONS[build["name"]],
                profile_settings,
            )

        with tempfile.TemporaryDirectory() as temporary:
            request, artifact_directory, _public_key = self.make_request(temporary)
            sdkconfig_path = artifact_directory / "config/sdkconfig.h"
            for missing_define in sorted(required_runtime_defines):
                with self.subTest(missing_define=missing_define):
                    sdkconfig_path.write_text(
                        "\n".join(
                            sorted(
                                provisioning.PILOT_SDKCONFIG_DEFINES
                                - {missing_define}
                            )
                        )
                        + "\n",
                        encoding="utf-8",
                    )
                    with self.assertRaisesRegex(
                        provisioning.ProvisioningError, "profile"
                    ):
                        provisioning._validate_firmware_artifacts(request.firmware)

    def test_pilot_contract_rejects_cross_hardware_psram_mode(self):
        cases = (
            (
                provisioning.BOARD_PROFILE,
                provisioning.CORE_S3_PILOT_CONFIG,
                "CONFIG_SPIRAM_MODE_OCT=y",
                provisioning.BOARD_PROFILE,
                provisioning.CORE_S3_BOARD_PROFILES,
            ),
            (
                provisioning.BOARD_PROFILE_STOPWATCH,
                provisioning.STOPWATCH_PILOT_CONFIG,
                "CONFIG_SPIRAM_MODE_QUAD=y",
                "m5stack-stopwatch",
                frozenset((provisioning.BOARD_PROFILE_STOPWATCH,)),
            ),
        )
        with tempfile.TemporaryDirectory() as temporary:
            temporary = Path(temporary)
            for index, (
                profile,
                source_path,
                forbidden_option,
                config_type,
                allowed_profiles,
            ) in enumerate(cases):
                with self.subTest(profile=profile):
                    config = json.loads(source_path.read_text(encoding="utf-8"))
                    selected = next(
                        build for build in config["builds"]
                        if build["name"] == profile
                    )
                    selected["sdkconfig_append"].append(forbidden_option)
                    tampered_path = temporary / f"tampered-{index}.json"
                    tampered_path.write_text(
                        json.dumps(config), encoding="utf-8"
                    )
                    with mock.patch.dict(
                        provisioning.PILOT_CONFIGS, {profile: tampered_path}
                    ), mock.patch.dict(
                        provisioning.PILOT_CONFIG_TYPES,
                        {tampered_path: config_type},
                    ), mock.patch.dict(
                        provisioning.PILOT_CONFIG_PROFILE_SETS,
                        {tampered_path: allowed_profiles},
                    ):
                        with self.assertRaisesRegex(
                            provisioning.ProvisioningError, "PSRAM"
                        ):
                            provisioning._validate_firmware_contract(profile)

    def test_one_network_bundle_is_private_encrypted_redacted_and_no_reset(self):
        with tempfile.TemporaryDirectory() as temporary:
            request, artifact_directory, public_key = self.make_request(temporary)
            output_directory = Path(temporary) / "bundle"
            manifest = self.generate(request, output_directory)

            regular_artifacts = [
                path for path in output_directory.rglob("*") if path.is_file()
            ]
            self.assertEqual(len(regular_artifacts), 13)
            self.assertEqual(stat.S_IMODE(output_directory.stat().st_mode), 0o700)
            self.assertEqual(
                stat.S_IMODE((output_directory / "firmware").stat().st_mode), 0o700
            )
            self.assertTrue(
                all(
                    stat.S_IMODE(path.stat().st_mode) == 0o600
                    for path in regular_artifacts
                )
            )

            manifest_path = next(output_directory.glob("*-manifest.json"))
            private_manifest_path = next(
                output_directory.glob("*-private-checksums.json")
            )
            verifier_path = next(output_directory.glob("*-verify-before-flash.py"))
            command_path = next(output_directory.glob("*.txt"))
            nvs_path = next(output_directory.glob("*-encrypted-nvs.bin"))
            key_path = next(output_directory.glob("*-nvs-keys.bin"))
            manifest_text = manifest_path.read_text(encoding="utf-8")
            private_manifest_text = private_manifest_path.read_text(encoding="utf-8")
            verifier_text = verifier_path.read_text(encoding="utf-8")
            command_text = command_path.read_text(encoding="utf-8")
            nvs_key_digest = digest(key_path.read_bytes())
            for sensitive in (
                DEVICE_CREDENTIAL,
                WIFI_SSID,
                WIFI_PASSWORD,
                str(artifact_directory),
                str(public_key),
                nvs_key_digest,
            ):
                self.assertNotIn(sensitive, manifest_text)
                self.assertNotIn(sensitive, command_text)
            self.assertIn(nvs_key_digest, private_manifest_text)
            self.assertIn(nvs_key_digest, verifier_text)
            self.assertNotIn(DEVICE_CREDENTIAL.encode(), nvs_path.read_bytes())
            self.assertNotIn(WIFI_PASSWORD.encode(), nvs_path.read_bytes())

            self.assertEqual(manifest["created_at"], "2026-08-23T12:00:00Z")
            self.assertEqual(
                manifest["device"]["profile"], provisioning.BOARD_PROFILE
            )
            self.assertEqual(
                manifest["firmware"]["profile"], provisioning.BOARD_PROFILE
            )
            self.assertEqual(manifest["wifi"]["network_count"], 1)
            self.assertEqual(manifest["wifi"]["networks"][0]["role"], "primary")
            self.assertEqual(manifest["nvs"]["encryption"], "XTS-AES")
            self.assertNotIn("sha256", manifest["nvs_keys"])
            private_manifest = json.loads(private_manifest_text)
            self.assertEqual(
                set(private_manifest["files"]),
                {
                    "bootloader",
                    "partition_table",
                    "ota_data",
                    "application",
                    "assets",
                    "encrypted_nvs",
                    "nvs_keys",
                    "signing_public_key",
                    "pilot_partition_contract",
                },
            )
            self.assertEqual(
                private_manifest["files"]["nvs_keys"]["sha256"], nvs_key_digest
            )
            self.assertFalse(
                private_manifest["external_approval_gate"][
                    "trust_root_independence_provided_by_bundle"
                ]
            )
            signing = manifest["firmware"]["signing"]
            self.assertEqual(signing["required_mode"], "external_offline")
            self.assertTrue(signing["bootloader_signature_verified"])
            self.assertTrue(signing["application_signature_verified"])
            self.assertFalse(signing["private_key_reference_in_build_metadata"])
            self.assertFalse(signing["signing_custody_verified_by_tool"])
            gate = manifest["operator_gate"]
            self.assertFalse(gate["flash_performed"])
            self.assertFalse(gate["efuse_modified"])
            self.assertFalse(gate["first_boot_authorized"])

            flash_command = gate["flash_command"]
            self.assertEqual(flash_command.count("write-flash"), 1)
            self.assertIn("--after no-reset", flash_command)
            self.assertNotIn("hard-reset", flash_command)
            self.assertNotIn("merged-binary", flash_command)
            for offset in (
                "0x0",
                "0x8000",
                "0x9000",
                "0xd000",
                "0x10000",
                "0x20000",
                "0x800000",
            ):
                self.assertIn(offset, flash_command.split())
            self.assertIn("espefuse", gate["efuse_summary_command"])
            self.assertIn("verify-before-flash.py", gate["preflash_verifier_command"])
            self.assertLess(
                command_text.index("PRE-FLASH VERIFICATION PASSED"),
                command_text.index("erase-flash"),
            )
            self.assertIn("STOP", command_text)

    def test_lite_bundle_requires_and_records_the_lite_signed_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            request, artifact_directory, _public_key = self.make_request(
                temporary, hardware_profile=provisioning.BOARD_PROFILE_LITE
            )
            output_directory = Path(temporary) / "lite-bundle"
            manifest = self.generate(request, output_directory)
            self.assertEqual(
                manifest["device"]["profile"], provisioning.BOARD_PROFILE_LITE
            )
            self.assertEqual(
                manifest["firmware"]["profile"], provisioning.BOARD_PROFILE_LITE
            )

            sdkconfig_path = artifact_directory / "config/sdkconfig.h"
            sdkconfig_path.write_text(
                "\n".join(sorted(provisioning.PILOT_SDKCONFIG_DEFINES)) + "\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(provisioning.ProvisioningError, "profile"):
                provisioning._validate_firmware_artifacts(
                    request.firmware, request.hardware_profile
                )

            lite_defines = provisioning.PILOT_SHARED_SDKCONFIG_DEFINES | {
                provisioning.BOARD_PROFILE_CONFIG_DEFINES[
                    provisioning.BOARD_PROFILE_LITE
                ],
                "#define CONFIG_CAMERA_GC0308 1",
            } | provisioning.BOARD_PROFILE_REQUIRED_SDKCONFIG_DEFINES[
                provisioning.BOARD_PROFILE_LITE
            ]
            sdkconfig_path.write_text(
                "\n".join(sorted(lite_defines)) + "\n", encoding="utf-8"
            )
            with self.assertRaisesRegex(provisioning.ProvisioningError, "forbidden"):
                provisioning._validate_firmware_artifacts(
                    request.firmware, request.hardware_profile
                )

            wrong_memory = (
                provisioning.PILOT_SHARED_SDKCONFIG_DEFINES
                | {
                    provisioning.BOARD_PROFILE_CONFIG_DEFINES[
                        provisioning.BOARD_PROFILE_LITE
                    ],
                    "#define CONFIG_SPIRAM_MODE_OCT 1",
                }
                | provisioning.BOARD_PROFILE_REQUIRED_SDKCONFIG_DEFINES[
                    provisioning.BOARD_PROFILE_LITE
                ]
            )
            sdkconfig_path.write_text(
                "\n".join(sorted(wrong_memory)) + "\n", encoding="utf-8"
            )
            with self.assertRaisesRegex(provisioning.ProvisioningError, "PSRAM"):
                provisioning._validate_firmware_artifacts(
                    request.firmware, request.hardware_profile
                )

    def test_stopwatch_bundle_requires_and_records_the_stopwatch_signed_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            request, artifact_directory, _public_key = self.make_request(
                temporary,
                hardware_profile=provisioning.BOARD_PROFILE_STOPWATCH,
            )
            output_directory = Path(temporary) / "stopwatch-bundle"
            manifest = self.generate(request, output_directory)
            self.assertEqual(
                manifest["device"]["profile"],
                provisioning.BOARD_PROFILE_STOPWATCH,
            )
            self.assertEqual(
                manifest["firmware"]["profile"],
                provisioning.BOARD_PROFILE_STOPWATCH,
            )

            sdkconfig_path = artifact_directory / "config/sdkconfig.h"
            lite_defines = provisioning.PILOT_SHARED_SDKCONFIG_DEFINES | {
                provisioning.BOARD_PROFILE_CONFIG_DEFINES[
                    provisioning.BOARD_PROFILE_LITE
                ]
            } | provisioning.BOARD_PROFILE_REQUIRED_SDKCONFIG_DEFINES[
                provisioning.BOARD_PROFILE_LITE
            ]
            sdkconfig_path.write_text(
                "\n".join(sorted(lite_defines)) + "\n", encoding="utf-8"
            )
            with self.assertRaisesRegex(provisioning.ProvisioningError, "profile"):
                provisioning._validate_firmware_artifacts(
                    request.firmware, request.hardware_profile
                )

            stopwatch_defines = provisioning.PILOT_SHARED_SDKCONFIG_DEFINES | {
                provisioning.BOARD_PROFILE_CONFIG_DEFINES[
                    provisioning.BOARD_PROFILE_STOPWATCH
                ],
                "#define CONFIG_CAMERA_GC0308 1",
            } | provisioning.BOARD_PROFILE_REQUIRED_SDKCONFIG_DEFINES[
                provisioning.BOARD_PROFILE_STOPWATCH
            ]
            sdkconfig_path.write_text(
                "\n".join(sorted(stopwatch_defines)) + "\n", encoding="utf-8"
            )
            with self.assertRaisesRegex(provisioning.ProvisioningError, "forbidden"):
                provisioning._validate_firmware_artifacts(
                    request.firmware, request.hardware_profile
                )

            wrong_memory = (
                provisioning.PILOT_SHARED_SDKCONFIG_DEFINES
                | {
                    provisioning.BOARD_PROFILE_CONFIG_DEFINES[
                        provisioning.BOARD_PROFILE_STOPWATCH
                    ],
                    "#define CONFIG_SPIRAM_MODE_QUAD 1",
                }
                | provisioning.BOARD_PROFILE_REQUIRED_SDKCONFIG_DEFINES[
                    provisioning.BOARD_PROFILE_STOPWATCH
                ]
            )
            sdkconfig_path.write_text(
                "\n".join(sorted(wrong_memory)) + "\n", encoding="utf-8"
            )
            with self.assertRaisesRegex(provisioning.ProvisioningError, "PSRAM"):
                provisioning._validate_firmware_artifacts(
                    request.firmware, request.hardware_profile
                )

    def test_signed_application_identity_rejects_sdkconfig_sidecar_relabel(self):
        with tempfile.TemporaryDirectory() as temporary:
            artifact_directory, hashes, public_key = make_release_artifacts(
                Path(temporary), provisioning.BOARD_PROFILE
            )
            sdkconfig_path = artifact_directory / "config/sdkconfig.h"
            lite_defines = provisioning.PILOT_SHARED_SDKCONFIG_DEFINES | {
                provisioning.BOARD_PROFILE_CONFIG_DEFINES[
                    provisioning.BOARD_PROFILE_LITE
                ]
            } | provisioning.BOARD_PROFILE_REQUIRED_SDKCONFIG_DEFINES[
                provisioning.BOARD_PROFILE_LITE
            ]
            sdkconfig_path.write_text(
                "\n".join(sorted(lite_defines)) + "\n", encoding="utf-8"
            )
            request = parse(
                valid_document(
                    artifact_directory,
                    hashes,
                    public_key,
                    hardware_profile=provisioning.BOARD_PROFILE_LITE,
                )
            )
            with self.assertRaisesRegex(
                provisioning.ProvisioningError, "signed application identity"
            ):
                provisioning._validate_firmware_artifacts(
                    request.firmware, request.hardware_profile
                )

    def test_two_network_bundle_contains_only_upstream_fallback_keys(self):
        with tempfile.TemporaryDirectory() as temporary:
            request, _artifact_directory, _public_key = self.make_request(
                temporary, fallback=True
            )
            manifest = self.generate(request, Path(temporary) / "bundle")
            self.assertEqual(manifest["wifi"]["network_count"], 2)
            self.assertEqual(
                [network["role"] for network in manifest["wifi"]["networks"]],
                ["primary", "fallback"],
            )
            self.assertEqual(
                manifest["nvs"]["namespaces"]["wifi"],
                ["ssid", "password", "ssid1", "password1"],
            )

    def test_private_preflash_verifier_rejects_tampered_key_before_flash(self):
        with tempfile.TemporaryDirectory() as temporary:
            request, _artifact_directory, _public_key = self.make_request(temporary)
            output_directory = Path(temporary) / "bundle"
            self.generate(request, output_directory)
            key_path = next(output_directory.glob("*-nvs-keys.bin"))
            key_path.write_bytes(b"\x00" * provisioning.NVS_KEYS_PARTITION_SIZE)
            verifier_path = next(output_directory.glob("*-verify-before-flash.py"))

            result = subprocess.run(
                [sys.executable, str(verifier_path)],
                stdin=subprocess.DEVNULL,
                capture_output=True,
                check=False,
                timeout=10,
            )
            self.assertEqual(result.returncode, 2)
            self.assertIn(b"DO NOT ERASE OR FLASH", result.stderr)
            self.assertNotIn(DEVICE_CREDENTIAL.encode(), result.stderr)

    def test_private_preflash_verifier_passes_complete_untampered_bundle(self):
        with tempfile.TemporaryDirectory() as temporary:
            request, _artifact_directory, _public_key = self.make_request(temporary)
            output_directory = Path(temporary) / "bundle"
            self.generate(request, output_directory)
            verifier_path = next(output_directory.glob("*-verify-before-flash.py"))
            fake_bin = Path(temporary) / "fake-bin"
            fake_bin.mkdir()
            fake_docker = fake_bin / "docker"
            fake_docker.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            fake_docker.chmod(0o700)
            environment = dict(os.environ)
            environment["PATH"] = f"{fake_bin}{os.pathsep}{environment['PATH']}"

            result = subprocess.run(
                [sys.executable, str(verifier_path)],
                stdin=subprocess.DEVNULL,
                capture_output=True,
                check=False,
                timeout=10,
                env=environment,
            )
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            self.assertIn(b"PRE-FLASH VERIFICATION PASSED", result.stdout)
            self.assertIn(
                request.firmware.signing_public_key_sha256.encode(), result.stdout
            )

    def test_generation_failure_leaves_no_secret_bundle(self):
        def fail_generator(_work_directory):
            raise provisioning.ProvisioningError("synthetic generator failure")

        with tempfile.TemporaryDirectory() as temporary:
            request, _artifacts, _public_key = self.make_request(temporary)
            output_directory = Path(temporary) / "bundle"
            with self.assertRaises(provisioning.ProvisioningError):
                provisioning.generate_bundle(
                    request,
                    output_directory,
                    generator=fail_generator,
                    schema_inspector=inspector_for(request),
                    firmware_verifier=accept_signed_artifacts,
                )
            self.assertFalse(output_directory.exists())

    def test_temporary_secret_csv_is_wiped_after_generator_returns(self):
        captured_csv = None

        def observing_generator(work_directory):
            nonlocal captured_csv
            captured_csv = (work_directory / "input.csv").read_bytes()
            fake_generator(work_directory)

        with tempfile.TemporaryDirectory() as temporary:
            request, _artifacts, _public_key = self.make_request(temporary)
            output_directory = Path(temporary) / "bundle"
            with mock.patch.object(provisioning, "_best_effort_wipe") as wipe:
                provisioning.generate_bundle(
                    request,
                    output_directory,
                    generator=observing_generator,
                    schema_inspector=inspector_for(request),
                    firmware_verifier=accept_signed_artifacts,
                )
            self.assertIn(DEVICE_CREDENTIAL.encode(), captured_csv)
            wipe.assert_called_once()

        with tempfile.TemporaryDirectory() as temporary:
            csv_path = Path(temporary) / "input.csv"
            original = DEVICE_CREDENTIAL.encode()
            csv_path.write_bytes(original)
            provisioning._best_effort_wipe(csv_path)
            self.assertEqual(csv_path.read_bytes(), b"\x00" * len(original))

    def test_output_must_be_new_absolute_and_outside_repository(self):
        with tempfile.TemporaryDirectory() as temporary:
            request, _artifacts, _public_key = self.make_request(temporary)
            for output_directory in (Path("relative"), ROOT / "secret-output"):
                with (
                    self.subTest(output_directory=output_directory),
                    self.assertRaises(provisioning.ProvisioningError),
                ):
                    provisioning.generate_bundle(
                        request,
                        output_directory,
                        generator=fake_generator,
                        schema_inspector=inspector_for(request),
                        firmware_verifier=accept_signed_artifacts,
                    )

            insecure_parent = Path(temporary) / "insecure"
            insecure_parent.mkdir(mode=0o700)
            insecure_parent.chmod(0o777)
            try:
                with self.assertRaises(provisioning.ProvisioningError):
                    provisioning.generate_bundle(
                        request,
                        insecure_parent / "bundle",
                        generator=fake_generator,
                        schema_inspector=inspector_for(request),
                        firmware_verifier=accept_signed_artifacts,
                    )
            finally:
                insecure_parent.chmod(0o700)

    def test_firmware_hash_profile_public_key_and_signature_fail_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            artifact_directory, hashes, public_key = make_release_artifacts(
                Path(temporary)
            )
            wrong_hashes = dict(hashes)
            wrong_hashes["application"] = "0" * 64
            request = parse(
                valid_document(artifact_directory, wrong_hashes, public_key)
            )
            with self.assertRaisesRegex(provisioning.ProvisioningError, "hash"):
                provisioning.generate_bundle(
                    request,
                    Path(temporary) / "hash-failure",
                    firmware_verifier=accept_signed_artifacts,
                )

            request = parse(valid_document(artifact_directory, hashes, public_key))
            (artifact_directory / "config/sdkconfig.h").write_text(
                "#define CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_CORE_S3 1\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(provisioning.ProvisioningError, "profile"):
                provisioning.generate_bundle(
                    request,
                    Path(temporary) / "profile-failure",
                    firmware_verifier=accept_signed_artifacts,
                )

        with tempfile.TemporaryDirectory() as temporary:
            request, _artifacts, _public_key = self.make_request(temporary)

            def reject_signature(
                _firmware_directory, _expected_version, _expected_profile
            ):
                raise provisioning.ProvisioningError("signature rejected")

            output_directory = Path(temporary) / "signature-failure"
            with self.assertRaisesRegex(provisioning.ProvisioningError, "signature"):
                provisioning.generate_bundle(
                    request,
                    output_directory,
                    generator=fake_generator,
                    schema_inspector=inspector_for(request),
                    firmware_verifier=reject_signature,
                )
            self.assertFalse(output_directory.exists())

        with tempfile.TemporaryDirectory() as temporary:
            artifact_directory, hashes, public_key = make_release_artifacts(
                Path(temporary)
            )
            document = valid_document(artifact_directory, hashes, public_key)
            document["firmware"]["signing_public_key"]["sha256"] = "0" * 64
            request = parse(document)
            with self.assertRaisesRegex(provisioning.ProvisioningError, "public key"):
                provisioning.generate_bundle(
                    request,
                    Path(temporary) / "public-key-failure",
                    firmware_verifier=accept_signed_artifacts,
                )

        with tempfile.TemporaryDirectory() as temporary:
            request, artifact_directory, _public_key = self.make_request(temporary)
            sdkconfig_path = artifact_directory / "config/sdkconfig.h"
            sdkconfig_path.write_text(
                sdkconfig_path.read_text(encoding="utf-8")
                + "#define CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES 1\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(provisioning.ProvisioningError, "forbidden"):
                provisioning.generate_bundle(
                    request,
                    Path(temporary) / "automatic-signing-failure",
                    firmware_verifier=accept_signed_artifacts,
                )

    def test_plaintext_or_wrong_nvs_schema_fails_closed(self):
        def plaintext_generator(work_directory):
            image = DEVICE_CREDENTIAL.encode() + b"\xff" * (
                provisioning.NVS_PARTITION_SIZE - len(DEVICE_CREDENTIAL)
            )
            (work_directory / "device-nvs.bin").write_bytes(image)
            (work_directory / "keys").mkdir()
            (work_directory / "keys/nvs_keys.bin").write_bytes(
                b"\x5a" * provisioning.NVS_KEYS_PARTITION_SIZE
            )

        with tempfile.TemporaryDirectory() as temporary:
            request, _artifacts, _public_key = self.make_request(temporary)
            with self.assertRaisesRegex(provisioning.ProvisioningError, "exposed"):
                provisioning.generate_bundle(
                    request,
                    Path(temporary) / "plaintext-failure",
                    generator=plaintext_generator,
                    schema_inspector=inspector_for(request),
                    firmware_verifier=accept_signed_artifacts,
                )

        with tempfile.TemporaryDirectory() as temporary:
            request, _artifacts, _public_key = self.make_request(temporary)
            with self.assertRaisesRegex(provisioning.ProvisioningError, "schema"):
                provisioning.generate_bundle(
                    request,
                    Path(temporary) / "schema-failure",
                    generator=fake_generator,
                    schema_inspector=lambda _work: expected_nvs_entries(request)[:-1],
                    firmware_verifier=accept_signed_artifacts,
                )

    @mock.patch.object(provisioning.subprocess, "run")
    def test_generator_is_pinned_encrypted_offline_and_no_secret_arguments(self, run):
        run.return_value = subprocess.CompletedProcess([], 0, b"ignored", b"ignored")
        with tempfile.TemporaryDirectory() as temporary:
            provisioning._run_nvs_generator(Path(temporary))

        command = run.call_args.args[0]
        serialized = " ".join(command)
        self.assertIn(provisioning.PINNED_IDF_IMAGE, command)
        self.assertIn("--pull=never", command)
        self.assertIn("--network=none", command)
        self.assertIn("--read-only", command)
        self.assertIn("--cap-drop=ALL", command)
        self.assertIn("encrypt", command)
        self.assertIn("--keygen", command)
        self.assertIn("--keyfile", command)
        self.assertNotIn("generate", command)
        self.assertNotIn(DEVICE_CREDENTIAL, serialized)
        self.assertNotIn(WIFI_PASSWORD, serialized)
        self.assertIs(run.call_args.kwargs["stdin"], subprocess.DEVNULL)
        self.assertTrue(run.call_args.kwargs["capture_output"])

    @mock.patch.object(provisioning.subprocess, "run")
    def test_artifact_verifier_checks_signatures_and_exact_partition(self, run):
        run.return_value = subprocess.CompletedProcess([], 0, b"", b"")
        provisioning._run_pilot_artifact_verifier(
            Path("/private/pilot-firmware"),
            "2.4.2",
            provisioning.BOARD_PROFILE,
        )

        command = run.call_args.args[0]
        serialized = " ".join(command)
        self.assertIn(provisioning.PINNED_IDF_IMAGE, command)
        self.assertIn("--network=none", command)
        self.assertEqual(serialized.count("verify-signature"), 2)
        self.assertIn("--keyfile", serialized)
        self.assertIn("App version: 2.4.2", serialized)
        self.assertIn(
            provisioning.SIGNED_HARDWARE_IDENTITY_MARKERS[
                provisioning.BOARD_PROFILE
            ].rstrip(b"\x00").decode("ascii"),
            serialized,
        )
        self.assertIn("gen_esp32part.py", serialized)
        self.assertIn("partition_table.bin", serialized)
        self.assertNotIn("merged-binary", serialized)


if __name__ == "__main__":
    unittest.main()
