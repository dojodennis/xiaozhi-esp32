import hashlib
import importlib.util
import io
import json
import os
import stat
import sys
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/prepare_provisions_reversible_bench.py"
SPEC = importlib.util.spec_from_file_location("provisions_bench", SCRIPT)
bench = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = bench
SPEC.loader.exec_module(bench)

secure = bench.secure
DEVICE_UUID = "50000000-0000-4000-8000-000000000001"
KEY_UUID = "60000000-0000-4000-8000-000000000001"
TOKEN = f"pvd1_{KEY_UUID}." + ("A" * 43)
SSID = "Private Demo 2G"
PASSWORD = "not-a-real-password"


def digest(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def file_digest(path: Path) -> str:
    checksum = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            checksum.update(chunk)
    return checksum.hexdigest()


def make_recovery(parent: Path) -> tuple[Path, str]:
    path = parent / "verified-full-factory-16mb.bin"
    with path.open("wb") as recovery:
        recovery.truncate(bench.RECOVERY_IMAGE_SIZE)
    return path, file_digest(path)


def make_artifacts(parent: Path, profile: str) -> tuple[Path, dict[str, str], Path]:
    root = parent / "bench-build"
    (root / "bootloader").mkdir(parents=True)
    (root / "partition_table").mkdir()
    (root / "config").mkdir()
    contents = {
        "bootloader": b"valid-unsigned-bench-bootloader",
        "partition_table": b"valid-provisions-partition-table",
        "ota_data": b"valid-ota-data",
        "application": (
            b"valid-externally-signed-bench-app\x00"
            + secure.SIGNED_HARDWARE_IDENTITY_MARKERS[profile]
        ),
        "assets": b"valid-assets",
    }
    for artifact in secure.FIRMWARE_ARTIFACTS:
        (root / artifact.relative_path).write_bytes(contents[artifact.key])
    (root / "project_description.json").write_text(
        json.dumps(
            {
                "project_name": "xiaozhi",
                "project_version": "2.4.2",
                "target": "esp32s3",
            }
        ),
        encoding="utf-8",
    )
    defines = bench._required_profile_defines(profile)
    (root / "config/sdkconfig.h").write_text(
        "\n".join(sorted(defines)) + "\n", encoding="utf-8"
    )
    public_key = parent / "approved-public-key.pem"
    public_key.write_bytes(
        b"-----BEGIN PUBLIC KEY-----\n" + (b"A" * 256) + b"\n-----END PUBLIC KEY-----\n"
    )
    return root, {key: digest(value) for key, value in contents.items()}, public_key


def document(
    artifacts: Path,
    hashes: dict[str, str],
    public_key: Path,
    profile: str,
) -> dict:
    return {
        "schema_version": 2,
        "hardware_profile": profile,
        "device_uuid": DEVICE_UUID,
        "hardware_serial": "44:1b:f6:e3:a0:5c",
        "device_credential": TOKEN,
        "wifi": {
            "networks": [
                {
                    "role": "primary",
                    "ssid": SSID,
                    "password": PASSWORD,
                    "band": "2.4GHz",
                }
            ]
        },
        "serial_port": "/dev/cu.usbmodem1101",
        "firmware": {
            "version": "2.4.2",
            "artifact_directory": str(artifacts),
            "sha256": hashes,
            "signing_public_key": {
                "path": str(public_key),
                "sha256": digest(public_key.read_bytes()),
            },
        },
    }


def request_for(parent: Path, profile: str = secure.BOARD_PROFILE_LITE):
    artifacts, hashes, public_key = make_artifacts(parent, profile)
    request = secure.parse_provisioning_document(
        json.dumps(document(artifacts, hashes, public_key, profile)).encode()
    )
    return request, artifacts


def fake_generator(request: secure.ProvisioningInput):
    def generate(work_directory: Path) -> None:
        image = bytearray(b"\xff" * secure.NVS_PARTITION_SIZE)
        image[128 : 128 + len(request.device_credential)] = (
            request.device_credential.encode()
        )
        (work_directory / "device-nvs.bin").write_bytes(image)

    return generate


def fake_inspector(request: secure.ProvisioningInput):
    return lambda _work_directory: bench._expected_nvs_entries(request)


def no_artifact_verification(_directory: Path, _version: str, _profile: str) -> None:
    return None


class ReversibleBenchTests(unittest.TestCase):
    def generate(self, request, output):
        recovery_path, recovery_sha256 = make_recovery(output.parent)
        return bench.generate_bench_bundle(
            request,
            output,
            recovery_path,
            recovery_sha256,
            generator=fake_generator(request),
            inspector=fake_inspector(request),
            artifact_verifier=no_artifact_verification,
            now=lambda: datetime(2026, 8, 28, 12, 0, tzinfo=timezone.utc),
        )

    def test_profiles_are_explicit_no_efuse_manual_ptt_builds(self):
        for profile, path in bench.BENCH_CONFIGS.items():
            config = json.loads(path.read_text(encoding="utf-8"))
            build = next(item for item in config["builds"] if item["name"] == profile)
            options = set(build["sdkconfig_append"])
            self.assertTrue(bench.BENCH_REQUIRED_OPTIONS.issubset(options))
            self.assertIn(secure.BOARD_PROFILE_SDKCONFIG_OPTIONS[profile], options)
            self.assertIn("CONFIG_PROVISIONS_GATEWAY_REQUIRED=y", options)
            self.assertIn("CONFIG_WAKE_WORD_DISABLED=y", options)
            self.assertIn("CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y", options)
            self.assertIn("CONFIG_SECURE_BOOT=n", options)
            self.assertIn("CONFIG_SECURE_FLASH_ENC_ENABLED=n", options)
            self.assertIn("CONFIG_NVS_ENCRYPTION=n", options)
            self.assertIn("CONFIG_BOOTLOADER_SKIP_VALIDATE_ALWAYS=n", options)

    def test_secure_production_profiles_remain_separate_and_required(self):
        for path in set(secure.PILOT_CONFIGS.values()):
            config = json.loads(path.read_text(encoding="utf-8"))
            for build in config["builds"]:
                options = set(build["sdkconfig_append"])
                self.assertIn("CONFIG_SECURE_BOOT=y", options)
                self.assertIn("CONFIG_SECURE_FLASH_ENC_ENABLED=y", options)
                self.assertIn("CONFIG_NVS_ENCRYPTION=y", options)

    def test_factory_registration_output_is_consumed_without_translation(self):
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            request, _artifacts = request_for(parent)
            self.assertEqual(request.hardware_profile, secure.BOARD_PROFILE_LITE)
            self.assertEqual(request.device_uuid, DEVICE_UUID)
            self.assertEqual(request.device_credential, TOKEN)
            self.assertEqual(request.wifi_networks[0].ssid, SSID)

    def test_all_hardware_profiles_generate_private_plaintext_bundles(self):
        for profile in sorted(secure.BOARD_PROFILES):
            with self.subTest(
                profile=profile
            ), tempfile.TemporaryDirectory() as temporary:
                parent = Path(temporary)
                request, _artifacts = request_for(parent, profile)
                output = parent / "private-output"
                manifest = self.generate(request, output)

                self.assertTrue(manifest["bench_only"])
                self.assertFalse(manifest["commercial_pilot_approved"])
                self.assertFalse(manifest["firmware"]["hardware_secure_boot"])
                self.assertFalse(manifest["firmware"]["flash_encryption"])
                self.assertFalse(manifest["firmware"]["nvs_encryption"])
                self.assertTrue(
                    manifest["plaintext_nvs"][
                        "contains_extractable_wifi_and_device_credentials"
                    ]
                )
                recovery = manifest["external_recovery_image"]
                self.assertEqual(recovery["size"], bench.RECOVERY_IMAGE_SIZE)
                self.assertTrue(Path(recovery["path"]).is_file())
                self.assertFalse(
                    any(
                        path.name == Path(recovery["path"]).name
                        for path in output.rglob("*")
                    )
                )
                self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o700)
                for directory, _subdirectories, files in os.walk(output):
                    self.assertEqual(
                        stat.S_IMODE(Path(directory).stat().st_mode), 0o700
                    )
                    for name in files:
                        self.assertEqual(
                            stat.S_IMODE((Path(directory) / name).stat().st_mode),
                            0o600,
                        )

    def test_bundle_has_no_nvs_key_and_flash_never_writes_nvs_key_slot(self):
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            request, _artifacts = request_for(parent)
            output = parent / "output"
            manifest = self.generate(request, output)
            self.assertFalse(
                any("nvs-key" in path.name.lower() for path in output.rglob("*"))
            )
            flash = manifest["operator_gate"]["flash_command"]
            self.assertNotIn("0x10000", flash)
            self.assertIn("0x9000", flash)
            self.assertIn(
                "verify-flash", manifest["operator_gate"]["verify_flash_command"]
            )
            self.assertNotIn("burn", flash.lower())
            for command_key in (
                "read_mac_command",
                "efuse_summary_command",
                "erase_command",
                "flash_command",
                "verify_flash_command",
            ):
                command = manifest["operator_gate"][command_key]
                self.assertIn("--before no-reset", command)
                self.assertIn("--after no-reset", command)
                self.assertNotIn("default-reset", command)

    def test_redacted_evidence_and_commands_do_not_copy_secrets(self):
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            request, _artifacts = request_for(parent)
            output = parent / "output"
            self.generate(request, output)
            for path in output.glob("*.json"):
                text = path.read_text(encoding="utf-8")
                self.assertNotIn(TOKEN, text)
                self.assertNotIn(PASSWORD, text)
                self.assertNotIn(SSID, text)
            commands = next(output.glob("*commands.txt")).read_text(encoding="utf-8")
            self.assertNotIn(TOKEN, commands)
            self.assertNotIn(PASSWORD, commands)
            self.assertIn("physically extractable", commands)
            self.assertIn("revoke", commands)
            self.assertIn("erase the full flash", commands)

    def test_each_efuse_or_validation_security_define_is_rejected(self):
        for forbidden in sorted(bench.BENCH_FORBIDDEN_DEFINES):
            with self.subTest(
                forbidden=forbidden
            ), tempfile.TemporaryDirectory() as temporary:
                parent = Path(temporary)
                request, artifacts = request_for(parent)
                sdkconfig = artifacts / "config/sdkconfig.h"
                sdkconfig.write_text(
                    sdkconfig.read_text(encoding="utf-8") + forbidden + "\n",
                    encoding="utf-8",
                )
                with self.assertRaisesRegex(
                    secure.ProvisioningError,
                    "skip image validation, encrypt secrets, or enable security eFuses",
                ):
                    bench._validate_bench_artifacts(
                        request.firmware, request.hardware_profile
                    )

    def test_mismatched_board_identity_marker_and_hash_fail_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            request, artifacts = request_for(parent)
            app = artifacts / "xiaozhi.bin"
            app.write_bytes(
                app.read_bytes()
                + secure.SIGNED_HARDWARE_IDENTITY_MARKERS[
                    secure.BOARD_PROFILE_STOPWATCH
                ]
            )
            with self.assertRaises(secure.ProvisioningError):
                bench._validate_bench_artifacts(
                    request.firmware, request.hardware_profile
                )

    def test_private_signing_key_can_never_be_copied_as_public_material(self):
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            request, _artifacts = request_for(parent)
            public_key = request.firmware.signing_public_key
            private_key = (
                b"-----BEGIN PRIVATE KEY-----\n"
                + (b"A" * 256)
                + b"\n-----END PRIVATE KEY-----\n"
            )
            public_key.write_bytes(private_key)
            firmware = secure.FirmwareInput(
                version=request.firmware.version,
                artifact_directory=request.firmware.artifact_directory,
                approved_sha256=request.firmware.approved_sha256,
                signing_public_key=public_key,
                signing_public_key_sha256=digest(private_key),
            )
            with self.assertRaisesRegex(
                secure.ProvisioningError, "only an approved public key"
            ):
                bench._validate_bench_artifacts(firmware, request.hardware_profile)

    def test_wrong_nvs_size_schema_or_exposure_fails_and_removes_output(self):
        failures = (
            (
                lambda work: (work / "device-nvs.bin").write_bytes(b"short"),
                fake_inspector,
            ),
            (fake_generator, lambda _request: lambda _work: []),
            (
                lambda _request: lambda work: (work / "device-nvs.bin").write_bytes(
                    b"\xff" * secure.NVS_PARTITION_SIZE
                ),
                fake_inspector,
            ),
        )
        for generator_factory, inspector_factory in failures:
            with tempfile.TemporaryDirectory() as temporary:
                parent = Path(temporary)
                request, _artifacts = request_for(parent)
                output = parent / "output"
                generator = (
                    generator_factory(request)
                    if generator_factory is fake_generator
                    or getattr(generator_factory, "__code__", None)
                    and generator_factory.__code__.co_argcount == 1
                    and "_request" in generator_factory.__code__.co_varnames
                    else generator_factory
                )
                recovery_path, recovery_sha256 = make_recovery(parent)
                with self.assertRaises(secure.ProvisioningError):
                    bench.generate_bench_bundle(
                        request,
                        output,
                        recovery_path,
                        recovery_sha256,
                        generator=generator,
                        inspector=inspector_factory(request),
                        artifact_verifier=no_artifact_verification,
                    )
                self.assertFalse(output.exists())

    def test_recovery_image_requires_real_full_16mb_file_and_exact_hash(self):
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            recovery_path, recovery_sha256 = make_recovery(parent)
            recovery = bench._validate_recovery_image(recovery_path, recovery_sha256)
            self.assertEqual(recovery.size, bench.RECOVERY_IMAGE_SIZE)
            self.assertEqual(recovery.sha256, recovery_sha256)

            with self.assertRaisesRegex(
                secure.ProvisioningError, "SHA-256 is not approved"
            ):
                bench._validate_recovery_image(recovery_path, "a" * 64)

            short_path = parent / "partial.bin"
            short_path.write_bytes(b"partial")
            with self.assertRaisesRegex(secure.ProvisioningError, "full 16 MB"):
                bench._validate_recovery_image(short_path, file_digest(short_path))

            symlink_path = parent / "linked-recovery.bin"
            symlink_path.symlink_to(recovery_path)
            with self.assertRaisesRegex(secure.ProvisioningError, "not a link"):
                bench._validate_recovery_image(symlink_path, recovery_sha256)

    def test_cleanup_never_removes_output_without_exact_created_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            output = parent / "replacement"
            output.mkdir()
            marker = output / "belongs-to-another-invocation.txt"
            marker.write_text("retain", encoding="utf-8")
            metadata = output.lstat()
            wrong_identity = (metadata.st_dev, metadata.st_ino + 1)
            bench._cleanup_created_output(output, wrong_identity)
            self.assertEqual(marker.read_text(encoding="utf-8"), "retain")

    def test_input_and_output_must_be_private_and_outside_repository(self):
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            request, artifacts = request_for(parent)
            source = parent / "device.json"
            source.write_text(
                json.dumps(
                    document(
                        artifacts,
                        request.firmware.approved_sha256,
                        request.firmware.signing_public_key,
                        request.hardware_profile,
                    )
                ),
                encoding="utf-8",
            )
            source.chmod(0o644)
            with self.assertRaisesRegex(secure.ProvisioningError, "0600"):
                secure.load_private_input(source)
            with self.assertRaisesRegex(secure.ProvisioningError, "outside"):
                bench._validate_output_location(
                    ROOT / "unsafe-output", request.firmware.artifact_directory
                )

    def test_verifier_uses_pinned_offline_container_and_checks_signature(self):
        command = bench._bench_artifact_verifier_command(
            Path("/private/bundle/firmware"),
            "2.4.2",
            secure.BOARD_PROFILE_LITE,
        )
        serialized = " ".join(command)
        self.assertIn(secure.PINNED_IDF_IMAGE, command)
        self.assertIn("--network=none", command)
        self.assertIn("verify-signature", serialized)
        self.assertIn("image-info", serialized)
        self.assertIn("bench-partitions.csv", serialized)

    def test_cli_never_prints_credentials_or_tracebacks(self):
        stderr = io.StringIO()
        with mock.patch.object(
            bench.secure,
            "load_private_input",
            side_effect=secure.ProvisioningError(f"bad {TOKEN} {PASSWORD}"),
        ), mock.patch("sys.stderr", stderr):
            result = bench.main(
                [
                    "--input",
                    "/private/input.json",
                    "--output-dir",
                    "/private/out",
                    "--recovery-image",
                    f"/private/{TOKEN}/recovery.bin",
                    "--recovery-sha256",
                    "a" * 64,
                ]
            )
        self.assertEqual(result, 2)
        failure = stderr.getvalue()
        self.assertEqual(
            failure,
            "Bench bundle failed: private validation rejected; no bundle retained\n",
        )
        self.assertNotIn(TOKEN, failure)
        self.assertNotIn(PASSWORD, failure)
        self.assertNotIn("Traceback", failure)


if __name__ == "__main__":
    unittest.main()
