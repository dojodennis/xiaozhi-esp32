#!/usr/bin/env python3
"""Prepare a reversible, explicitly insecure Kitchen Helper bench bundle.

This is a no-hardware tool. It accepts the private schema-v2 output from the
factory registration tool, verifies a dedicated no-eFuse build, generates a
plaintext per-device NVS image, and emits operator commands. It never flashes,
erases, resets, or changes eFuses.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import stat
import sys
import tempfile
from collections.abc import Callable
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import provision_provisions_core_s3 as secure

REPO_ROOT = secure.REPO_ROOT
BENCH_CONFIGS = {
    secure.BOARD_PROFILE: (
        REPO_ROOT / "main/boards/m5stack/provisions-core-s3/bench_profile.json"
    ),
    secure.BOARD_PROFILE_LITE: (
        REPO_ROOT / "main/boards/m5stack/provisions-core-s3/bench_profile.json"
    ),
    secure.BOARD_PROFILE_STOPWATCH: (
        REPO_ROOT / "main/boards/m5stack/stopwatch/bench_profile.json"
    ),
}
BENCH_CONFIG_TYPES = {
    BENCH_CONFIGS[secure.BOARD_PROFILE]: secure.BOARD_PROFILE,
    BENCH_CONFIGS[secure.BOARD_PROFILE_STOPWATCH]: "m5stack-stopwatch",
}
BENCH_CONFIG_PROFILE_SETS = {
    BENCH_CONFIGS[secure.BOARD_PROFILE]: secure.CORE_S3_BOARD_PROFILES,
    BENCH_CONFIGS[secure.BOARD_PROFILE_STOPWATCH]: frozenset(
        (secure.BOARD_PROFILE_STOPWATCH,)
    ),
}
BENCH_PARTITION_TABLE = secure.PILOT_PARTITION_TABLE
RECOVERY_IMAGE_SIZE = 16 * 1024 * 1024

BENCH_REQUIRED_OPTIONS = frozenset(
    (
        "CONFIG_PARTITION_TABLE_CUSTOM=y",
        'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/provisions/16m.csv"',
        "CONFIG_BOOTLOADER_SKIP_VALIDATE_ALWAYS=n",
        "CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON=n",
        "CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP=n",
        "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y",
        "CONFIG_PROVISIONS_GATEWAY_REQUIRED=y",
        "CONFIG_WAKE_WORD_DISABLED=y",
        "CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y",
        "CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME=y",
        "CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n",
        "CONFIG_SECURE_BOOT=n",
        "CONFIG_SECURE_BOOT_V2_ENABLED=n",
        "CONFIG_SECURE_FLASH_ENC_ENABLED=n",
        "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=n",
        "CONFIG_NVS_ENCRYPTION=n",
        "CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC=n",
        "CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=n",
        "CONFIG_APP_ANTI_ROLLBACK=n",
    )
)
BENCH_REQUIRED_DEFINES = frozenset(
    (
        "#define CONFIG_PARTITION_TABLE_CUSTOM 1",
        '#define CONFIG_PARTITION_TABLE_CUSTOM_FILENAME "partitions/provisions/16m.csv"',
        "#define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1",
        "#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1",
        "#define CONFIG_WAKE_WORD_DISABLED 1",
        "#define CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT 1",
        "#define CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME 1",
    )
)
BENCH_FORBIDDEN_DEFINES = frozenset(
    (
        "#define CONFIG_BOOTLOADER_SKIP_VALIDATE_ALWAYS 1",
        "#define CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON 1",
        "#define CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP 1",
        "#define CONFIG_SECURE_BOOT 1",
        "#define CONFIG_SECURE_BOOT_V2_ENABLED 1",
        "#define CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES 1",
        "#define CONFIG_SECURE_FLASH_ENC_ENABLED 1",
        "#define CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE 1",
        "#define CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT 1",
        "#define CONFIG_FLASH_ENCRYPTION_ENABLED 1",
        "#define CONFIG_NVS_ENCRYPTION 1",
        "#define CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC 1",
        "#define CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK 1",
        "#define CONFIG_APP_ANTI_ROLLBACK 1",
        "#define CONFIG_EFUSE_VIRTUAL 1",
    )
)


@dataclass(frozen=True)
class RecoveryImage:
    path: Path
    size: int
    sha256: str
    device: int
    inode: int


def _validate_bench_contract(profile: str) -> None:
    if profile not in secure.BOARD_PROFILES:
        raise secure.ProvisioningError("bench hardware profile is not approved")
    config_path = BENCH_CONFIGS[profile]
    try:
        config = json.loads(config_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        raise secure.ProvisioningError(
            "reversible bench configuration is unavailable"
        ) from None
    if (
        config.get("type") != BENCH_CONFIG_TYPES[config_path]
        or config.get("target") != secure.CHIP
    ):
        raise secure.ProvisioningError(
            "reversible bench configuration identity is invalid"
        )
    builds = config.get("builds")
    expected_profiles = BENCH_CONFIG_PROFILE_SETS[config_path]
    if not isinstance(builds, list):
        raise secure.ProvisioningError("reversible bench build set is invalid")
    by_name = {build.get("name"): build for build in builds if isinstance(build, dict)}
    if set(by_name) != expected_profiles:
        raise secure.ProvisioningError("reversible bench build identities have changed")
    for build_profile, build in by_name.items():
        options = build.get("sdkconfig_append")
        if not isinstance(options, list):
            raise secure.ProvisioningError("reversible bench build options are invalid")
        option_set = set(options)
        expected = BENCH_REQUIRED_OPTIONS | {
            secure.BOARD_PROFILE_SDKCONFIG_OPTIONS[build_profile]
        }
        if build_profile in secure.CORE_S3_BOARD_PROFILES:
            expected |= {
                "CONFIG_SPIRAM_MODE_QUAD=y",
                "CONFIG_CAMERA_GC0308=n",
                "CONFIG_ESP_VIDEO_ENABLE_DVP_VIDEO_DEVICE=n",
            }
        else:
            expected |= {
                "CONFIG_SPIRAM=y",
                "CONFIG_SPIRAM_MODE_OCT=y",
                "CONFIG_SPIRAM_SPEED_80M=y",
            }
        if not expected.issubset(option_set):
            raise secure.ProvisioningError(
                "reversible bench safety options are incomplete"
            )
        other_board_options = {
            option
            for other_profile, option in secure.BOARD_PROFILE_SDKCONFIG_OPTIONS.items()
            if other_profile != build_profile
        }
        if other_board_options.intersection(option_set):
            raise secure.ProvisioningError(
                "reversible bench build is not bound to one hardware identity"
            )

    # The bench flow deliberately keeps the production 16 MB OTA layout. The
    # encrypted nvs_keys slot remains erased and is never included in flash commands.
    secure._validate_firmware_contract(profile)


def _required_profile_defines(profile: str) -> set[str]:
    return (
        set(BENCH_REQUIRED_DEFINES)
        | {secure.BOARD_PROFILE_CONFIG_DEFINES[profile]}
        | set(secure.BOARD_PROFILE_REQUIRED_SDKCONFIG_DEFINES[profile])
    )


def _validate_bench_artifacts(
    firmware: secure.FirmwareInput, profile: str
) -> tuple[dict[str, dict[str, Any]], dict[str, Any]]:
    try:
        root_metadata = firmware.artifact_directory.lstat()
    except OSError:
        raise secure.ProvisioningError(
            "firmware.artifact_directory is unavailable"
        ) from None
    if not stat.S_ISDIR(root_metadata.st_mode) or stat.S_ISLNK(root_metadata.st_mode):
        raise secure.ProvisioningError(
            "firmware.artifact_directory must be a real directory"
        )

    project_path = secure._safe_artifact_file(
        firmware.artifact_directory, Path("project_description.json")
    )
    try:
        project = json.loads(
            secure._read_bounded_text(project_path, "bench build metadata")
        )
    except json.JSONDecodeError:
        raise secure.ProvisioningError("bench build metadata is invalid") from None
    if (
        not isinstance(project, dict)
        or project.get("project_name") != "xiaozhi"
        or project.get("target") != secure.CHIP
        or project.get("project_version") != firmware.version
    ):
        raise secure.ProvisioningError(
            "bench build metadata does not match the registration output"
        )

    sdkconfig_path = secure._safe_artifact_file(
        firmware.artifact_directory, Path("config/sdkconfig.h")
    )
    sdkconfig_lines = set(
        secure._read_bounded_text(sdkconfig_path, "bench sdkconfig").splitlines()
    )
    if not _required_profile_defines(profile).issubset(sdkconfig_lines):
        raise secure.ProvisioningError(
            "build is not the approved reversible bench profile"
        )
    other_profiles = {
        define
        for other_profile, define in secure.BOARD_PROFILE_CONFIG_DEFINES.items()
        if other_profile != profile
    }
    if other_profiles.intersection(sdkconfig_lines):
        raise secure.ProvisioningError(
            "bench build identity does not match the registered hardware"
        )
    if secure.BOARD_PROFILE_FORBIDDEN_SDKCONFIG_DEFINES[profile].intersection(
        sdkconfig_lines
    ):
        raise secure.ProvisioningError(
            "bench build PSRAM mode does not match the registered hardware"
        )
    enabled_forbidden = BENCH_FORBIDDEN_DEFINES.intersection(sdkconfig_lines)
    if enabled_forbidden:
        raise secure.ProvisioningError(
            "build could skip image validation, encrypt secrets, or enable security eFuses"
        )
    if any(
        line.startswith("#define CONFIG_SECURE_BOOT_SIGNING_KEY ")
        for line in sdkconfig_lines
    ):
        raise secure.ProvisioningError(
            "bench build metadata references a private signing key"
        )

    artifacts: dict[str, dict[str, Any]] = {}
    for artifact in secure.FIRMWARE_ARTIFACTS:
        source = secure._safe_artifact_file(
            firmware.artifact_directory, artifact.relative_path
        )
        digest, size = secure._hash_file(source)
        if digest != firmware.approved_sha256[artifact.key]:
            raise secure.ProvisioningError(
                "bench firmware artifact hash is not independently approved"
            )
        if size == 0 or size > artifact.maximum_size:
            raise secure.ProvisioningError("bench firmware artifact size is invalid")
        if artifact.key == "application":
            secure._validate_signed_application_identity(source, profile)
        artifacts[artifact.key] = {
            "source": source,
            "relative_path": artifact.relative_path.as_posix(),
            "offset": artifact.offset,
            "size": size,
            "sha256": digest,
        }

    public_key = firmware.signing_public_key
    try:
        key_metadata = public_key.lstat()
    except OSError:
        raise secure.ProvisioningError(
            "approved signing public key is unavailable"
        ) from None
    if not stat.S_ISREG(key_metadata.st_mode) or stat.S_ISLNK(key_metadata.st_mode):
        raise secure.ProvisioningError("approved signing public key path is unsafe")
    key_digest, key_size = secure._hash_file(public_key)
    if key_digest != firmware.signing_public_key_sha256:
        raise secure.ProvisioningError("signing public key hash is not approved")
    if not 128 <= key_size <= 16 * 1024:
        raise secure.ProvisioningError("signing public key size is invalid")
    try:
        key_bytes = public_key.read_bytes()
    except OSError:
        raise secure.ProvisioningError(
            "approved signing public key is unavailable"
        ) from None
    if b"-----BEGIN PUBLIC KEY-----" not in key_bytes or b"PRIVATE KEY" in key_bytes:
        raise secure.ProvisioningError(
            "signing key input must contain only an approved public key"
        )
    return artifacts, {
        "source": public_key,
        "size": key_size,
        "sha256": key_digest,
    }


def _bench_artifact_verifier_command(
    firmware_directory: Path, expected_version: str, expected_profile: str
) -> list[str]:
    expected_marker = secure.SIGNED_HARDWARE_IDENTITY_MARKERS[expected_profile]
    all_markers = tuple(
        secure.SIGNED_HARDWARE_IDENTITY_MARKERS[profile]
        for profile in sorted(secure.BOARD_PROFILES)
    )
    script = f"""set -eu
PY={secure.IDF_PYTHON}
$PY -m espsecure verify-signature --version 2 --keyfile /bench/signing-public-key.pem /bench/application.bin >/dev/null 2>&1
$PY -m esptool --chip esp32s3 image-info /bench/bootloader.bin >/dev/null 2>&1
$PY -m esptool --chip esp32s3 image-info /bench/application.bin >/work/app-info.txt 2>/dev/null
grep -Fqx 'Project name: xiaozhi' /work/app-info.txt
grep -Fqx 'App version: {expected_version}' /work/app-info.txt
grep -Fqx 'ESP-IDF: v6.0.2' /work/app-info.txt
$PY - <<'PY'
from pathlib import Path
image = Path('/bench/application.bin').read_bytes()
expected = {expected_marker!r}
markers = {all_markers!r}
if image.count(expected) != 1 or any(marker != expected and marker in image for marker in markers):
    raise SystemExit(1)
PY
$PY {secure.PARTITION_GENERATOR} /bench/bench-partitions.csv /work/expected.bin >/dev/null 2>&1
cmp /work/expected.bin /bench/partition_table.bin >/dev/null
"""
    return [
        "docker",
        "run",
        *secure._docker_security_arguments(),
        "--tmpfs",
        "/work:rw,noexec,nosuid,nodev,size=2m",
        "--mount",
        f"type=bind,source={firmware_directory},target=/bench,readonly",
        "--entrypoint",
        "sh",
        secure.PINNED_IDF_IMAGE,
        "-c",
        script,
    ]


def _run_bench_artifact_verifier(
    firmware_directory: Path, expected_version: str, expected_profile: str
) -> None:
    secure._run_secret_processing_command(
        _bench_artifact_verifier_command(
            firmware_directory, expected_version, expected_profile
        ),
        "reversible bench artifact verification failed; reject this build",
    )


def _run_plaintext_nvs_generator(work_directory: Path) -> None:
    command = [
        "docker",
        "run",
        *secure._docker_security_arguments(),
        "--tmpfs",
        "/tmp:rw,noexec,nosuid,nodev,size=16m",
        "--mount",
        f"type=bind,source={work_directory},target=/work",
        "--workdir",
        "/work",
        "--entrypoint",
        secure.IDF_PYTHON,
        secure.PINNED_IDF_IMAGE,
        secure.NVS_GENERATOR,
        "generate",
        "--version",
        "2",
        "/work/input.csv",
        "/work/device-nvs.bin",
        hex(secure.NVS_PARTITION_SIZE),
    ]
    secure._run_secret_processing_command(
        command,
        "pinned plaintext-NVS generator failed; reject the bench bundle",
    )


def _run_plaintext_nvs_inspector(work_directory: Path) -> list[dict[str, Any]]:
    command = [
        "docker",
        "run",
        *secure._docker_security_arguments(),
        "--tmpfs",
        "/tmp:rw,noexec,nosuid,nodev,size=16m",
        "--mount",
        f"type=bind,source={work_directory},target=/work,readonly",
        "--entrypoint",
        secure.IDF_PYTHON,
        secure.PINNED_IDF_IMAGE,
        secure.NVS_TOOL,
        "/work/device-nvs.bin",
        "--dump",
        "minimal",
        "--format",
        "json",
        "--color",
        "never",
    ]
    result = secure._run_secret_processing_command(
        command, "plaintext NVS schema verification failed"
    )
    try:
        entries = json.loads(result.stdout)
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise secure.ProvisioningError(
            "plaintext NVS schema verification failed"
        ) from None
    if not isinstance(entries, list) or not all(
        isinstance(entry, dict) for entry in entries
    ):
        raise secure.ProvisioningError("plaintext NVS schema verification failed")
    return entries


def _expected_nvs_entries(
    request: secure.ProvisioningInput,
) -> list[dict[str, Any]]:
    entries: list[dict[str, Any]] = []
    for index, network in enumerate(request.wifi_networks):
        suffix = "" if index == 0 else str(index)
        entries.extend(
            (
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
            )
        )
    entries.extend(
        (
            {
                "namespace": "board",
                "key": "uuid",
                "encoding": "string",
                "data": request.device_uuid,
                "state": "Written",
                "is_empty": False,
            },
            {
                "namespace": "provisions",
                "key": "device_token",
                "encoding": "string",
                "data": request.device_credential,
                "state": "Written",
                "is_empty": False,
            },
        )
    )
    return entries


def _verify_plaintext_nvs(
    path: Path,
    request: secure.ProvisioningInput,
    work_directory: Path,
    inspector: Callable[[Path], list[dict[str, Any]]] | None,
) -> bytes:
    try:
        image = path.read_bytes()
    except OSError:
        raise secure.ProvisioningError(
            "plaintext NVS generator did not produce an image"
        ) from None
    if len(image) != secure.NVS_PARTITION_SIZE:
        raise secure.ProvisioningError("plaintext NVS image size is invalid")
    entries = (inspector or _run_plaintext_nvs_inspector)(work_directory)
    if entries != _expected_nvs_entries(request):
        raise secure.ProvisioningError(
            "plaintext NVS output failed exact schema verification"
        )
    if request.device_credential.encode("utf-8") not in image:
        raise secure.ProvisioningError(
            "plaintext NVS exposure check did not match the declared bench mode"
        )
    return image


def _validate_recovery_image(path: Path, expected_sha256: str) -> RecoveryImage:
    if not path.is_absolute():
        raise secure.ProvisioningError("recovery image path must be absolute")
    if not secure.SHA256_PATTERN.fullmatch(expected_sha256):
        raise secure.ProvisioningError(
            "recovery image SHA-256 must be canonical lowercase hexadecimal"
        )
    if any(character in str(path) for character in (",", "\r", "\n", "\x00")):
        raise secure.ProvisioningError(
            "recovery image path contains unsupported characters"
        )
    try:
        metadata = path.lstat()
        resolved = path.resolve(strict=True)
    except OSError:
        raise secure.ProvisioningError("recovery image is unavailable") from None
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        raise secure.ProvisioningError(
            "recovery image must be a real regular file, not a link"
        )
    if secure._is_within(resolved, REPO_ROOT):
        raise secure.ProvisioningError(
            "recovery image must be stored outside the repository"
        )

    flags = os.O_RDONLY
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags)
        with os.fdopen(descriptor, "rb", closefd=True) as recovery_file:
            opened = os.fstat(recovery_file.fileno())
            if (opened.st_dev, opened.st_ino) != (
                metadata.st_dev,
                metadata.st_ino,
            ) or not stat.S_ISREG(opened.st_mode):
                raise secure.ProvisioningError(
                    "recovery image changed while it was opened"
                )
            digest = hashlib.sha256()
            size = 0
            while chunk := recovery_file.read(1024 * 1024):
                digest.update(chunk)
                size += len(chunk)
    except secure.ProvisioningError:
        raise
    except OSError:
        raise secure.ProvisioningError(
            "recovery image could not be read safely"
        ) from None
    if size != RECOVERY_IMAGE_SIZE:
        raise secure.ProvisioningError(
            "recovery image must be an exact full 16 MB flash image"
        )
    actual_sha256 = digest.hexdigest()
    if actual_sha256 != expected_sha256:
        raise secure.ProvisioningError("recovery image SHA-256 is not approved")
    return RecoveryImage(
        path=path,
        size=size,
        sha256=actual_sha256,
        device=opened.st_dev,
        inode=opened.st_ino,
    )


def _require_recovery_unchanged(recovery: RecoveryImage) -> None:
    current = _validate_recovery_image(recovery.path, recovery.sha256)
    if (current.device, current.inode) != (recovery.device, recovery.inode):
        raise secure.ProvisioningError(
            "recovery image changed after initial verification"
        )


def _bench_esptool_prefix(request: secure.ProvisioningInput) -> list[str]:
    return [
        "esptool",
        "--chip",
        secure.CHIP,
        "--port",
        request.serial_port,
        "--baud",
        "460800",
        "--before",
        "no-reset",
        "--after",
        "no-reset",
    ]


def _cleanup_created_output(
    output_directory: Path, created_identity: tuple[int, int]
) -> None:
    try:
        metadata = output_directory.lstat()
        resolved = output_directory.resolve(strict=True)
    except OSError:
        return
    if (
        (metadata.st_dev, metadata.st_ino) != created_identity
        or stat.S_ISLNK(metadata.st_mode)
        or not stat.S_ISDIR(metadata.st_mode)
        or metadata.st_uid != os.getuid()
        or secure._is_within(resolved, REPO_ROOT)
    ):
        return
    try:
        shutil.rmtree(output_directory)
    except OSError:
        return


def _validate_output_location(output_directory: Path, artifact_directory: Path) -> None:
    if not output_directory.is_absolute():
        raise secure.ProvisioningError("output directory must be absolute")
    resolved = output_directory.resolve(strict=False)
    if secure._is_within(resolved, REPO_ROOT):
        raise secure.ProvisioningError(
            "output directory must be outside the repository"
        )
    if output_directory.exists() or output_directory.is_symlink():
        raise secure.ProvisioningError("output directory already exists")
    if not output_directory.parent.is_dir():
        raise secure.ProvisioningError("output parent directory does not exist")
    if secure._is_within(resolved, artifact_directory.resolve(strict=False)):
        raise secure.ProvisioningError(
            "output directory must be outside the firmware artifact package"
        )
    if any(character in str(output_directory) for character in (",", "\r", "\n")):
        raise secure.ProvisioningError(
            "output directory contains unsupported characters"
        )
    parent_metadata = output_directory.parent.stat()
    if (
        parent_metadata.st_uid != os.getuid()
        or stat.S_IMODE(parent_metadata.st_mode) & 0o022
    ):
        raise secure.ProvisioningError(
            "output parent must be operator-owned and not group/world writable"
        )


def _render_verifier(
    checksums_name: str,
    checksums: dict[str, dict[str, Any]],
    recovery_image: dict[str, Any],
    artifact_command: list[str],
) -> str:
    return f'''#!/usr/bin/env python3
"""Generated private verifier for one reversible bench bundle."""

import hashlib
import json
import os
import stat
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
EXPECTED = json.loads({json.dumps(checksums, separators=(",", ":"))!r})
RECOVERY = json.loads({json.dumps(recovery_image, separators=(",", ":"))!r})
COMMAND = json.loads({json.dumps(artifact_command, separators=(",", ":"))!r})


def fail():
    print("BENCH PRE-FLASH VERIFICATION FAILED; DO NOT ERASE OR FLASH", file=sys.stderr)
    raise SystemExit(2)


try:
    manifest = json.loads((ROOT / {checksums_name!r}).read_text(encoding="utf-8"))
    if (
        manifest.get("files") != EXPECTED
        or manifest.get("recovery_image") != RECOVERY
        or manifest.get("bench_only") is not True
    ):
        fail()
    for entry in EXPECTED.values():
        relative = Path(entry["path"])
        if relative.is_absolute() or ".." in relative.parts:
            fail()
        path = ROOT / relative
        metadata = path.lstat()
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
            fail()
        digest = hashlib.sha256()
        size = 0
        with path.open("rb") as source:
            while chunk := source.read(1024 * 1024):
                digest.update(chunk)
                size += len(chunk)
        if size != entry["size"] or digest.hexdigest() != entry["sha256"]:
            fail()
    recovery_path = Path(RECOVERY["path"])
    if not recovery_path.is_absolute() or ROOT.resolve() in recovery_path.resolve().parents:
        fail()
    recovery_metadata = recovery_path.lstat()
    if stat.S_ISLNK(recovery_metadata.st_mode) or not stat.S_ISREG(recovery_metadata.st_mode):
        fail()
    recovery_flags = os.O_RDONLY
    if hasattr(os, "O_NOFOLLOW"):
        recovery_flags |= os.O_NOFOLLOW
    recovery_descriptor = os.open(recovery_path, recovery_flags)
    recovery_digest = hashlib.sha256()
    recovery_size = 0
    with os.fdopen(recovery_descriptor, "rb", closefd=True) as recovery_file:
        opened_recovery = os.fstat(recovery_file.fileno())
        if (
            (opened_recovery.st_dev, opened_recovery.st_ino)
            != (recovery_metadata.st_dev, recovery_metadata.st_ino)
            or not stat.S_ISREG(opened_recovery.st_mode)
        ):
            fail()
        while chunk := recovery_file.read(1024 * 1024):
            recovery_digest.update(chunk)
            recovery_size += len(chunk)
    if (
        recovery_size != RECOVERY["size"]
        or recovery_digest.hexdigest() != RECOVERY["sha256"]
    ):
        fail()
    result = subprocess.run(
        COMMAND,
        stdin=subprocess.DEVNULL,
        capture_output=True,
        check=False,
        timeout=90,
    )
    if result.returncode != 0:
        fail()
except (AttributeError, OSError, KeyError, TypeError, ValueError, subprocess.SubprocessError):
    fail()

print("REVERSIBLE BENCH PRE-FLASH VERIFICATION PASSED")
print("WARNING: credentials in this bundle and on the device are plaintext/extractable")
'''


def generate_bench_bundle(
    request: secure.ProvisioningInput,
    output_directory: Path,
    recovery_image_path: Path,
    recovery_image_sha256: str,
    *,
    generator: Callable[[Path], None] | None = None,
    inspector: Callable[[Path], list[dict[str, Any]]] | None = None,
    artifact_verifier: Callable[[Path, str, str], None] | None = None,
    now: Callable[[], datetime] | None = None,
) -> dict[str, Any]:
    recovery_image = _validate_recovery_image(
        recovery_image_path, recovery_image_sha256
    )
    _validate_bench_contract(request.hardware_profile)
    firmware_metadata, key_metadata = _validate_bench_artifacts(
        request.firmware, request.hardware_profile
    )
    _validate_output_location(output_directory, request.firmware.artifact_directory)
    created_output_identity: tuple[int, int] | None = None
    try:
        output_directory.mkdir(mode=0o700)
        created_metadata = output_directory.lstat()
        if not stat.S_ISDIR(created_metadata.st_mode) or stat.S_ISLNK(
            created_metadata.st_mode
        ):
            raise secure.ProvisioningError(
                "created output directory failed exact verification"
            )
        created_output_identity = (
            created_metadata.st_dev,
            created_metadata.st_ino,
        )
        output_directory.chmod(0o700)
        firmware_directory = output_directory / "firmware"
        firmware_directory.mkdir(mode=0o700)
        firmware_directory.chmod(0o700)

        bundled: dict[str, Path] = {}
        for artifact in secure.FIRMWARE_ARTIFACTS:
            destination = firmware_directory / f"{artifact.key}.bin"
            size = secure._copy_approved_file(
                firmware_metadata[artifact.key]["source"],
                destination,
                firmware_metadata[artifact.key]["sha256"],
            )
            if size != firmware_metadata[artifact.key]["size"]:
                raise secure.ProvisioningError(
                    "bench firmware artifact changed while copying"
                )
            bundled[artifact.key] = destination

        public_key = firmware_directory / "signing-public-key.pem"
        if (
            secure._copy_approved_file(
                key_metadata["source"], public_key, key_metadata["sha256"]
            )
            != key_metadata["size"]
        ):
            raise secure.ProvisioningError("signing public key changed while copying")
        partition_digest, partition_size = secure._hash_file(BENCH_PARTITION_TABLE)
        partition_contract = firmware_directory / "bench-partitions.csv"
        if (
            secure._copy_approved_file(
                BENCH_PARTITION_TABLE, partition_contract, partition_digest
            )
            != partition_size
        ):
            raise secure.ProvisioningError("partition contract changed while copying")
        (artifact_verifier or _run_bench_artifact_verifier)(
            firmware_directory, request.firmware.version, request.hardware_profile
        )

        with tempfile.TemporaryDirectory(prefix=".work-", dir=output_directory) as temp:
            work_directory = Path(temp)
            work_directory.chmod(0o700)
            csv_path = work_directory / "input.csv"
            nvs_path = work_directory / "device-nvs.bin"
            secure._write_nvs_csv(csv_path, request)
            try:
                (generator or _run_plaintext_nvs_generator)(work_directory)
            finally:
                secure._best_effort_wipe(csv_path)
            image = _verify_plaintext_nvs(nvs_path, request, work_directory, inspector)
            nvs_path.chmod(0o600)

            prefix = f"{request.hardware_profile}-{request.device_uuid}"
            nvs_name = f"{prefix}-PLAINTEXT-nvs.bin"
            manifest_name = f"{prefix}-bench-evidence.json"
            checksums_name = f"{prefix}-private-checksums.json"
            verifier_name = f"{prefix}-verify-before-flash.py"
            commands_name = f"{prefix}-bench-commands.txt"
            final_nvs = output_directory / nvs_name
            final_manifest = output_directory / manifest_name
            final_checksums = output_directory / checksums_name
            final_verifier = output_directory / verifier_name
            final_commands = output_directory / commands_name

            created = (now or (lambda: datetime.now(timezone.utc)))()
            if created.tzinfo is None:
                raise secure.ProvisioningError(
                    "evidence timestamp must be timezone-aware"
                )
            created_text = (
                created.astimezone(timezone.utc)
                .isoformat(timespec="seconds")
                .replace("+00:00", "Z")
            )
            _require_recovery_unchanged(recovery_image)
            recovery_evidence = {
                "path": str(recovery_image.path),
                "size": recovery_image.size,
                "sha256": recovery_image.sha256,
            }

            checksums: dict[str, dict[str, Any]] = {}
            for artifact in secure.FIRMWARE_ARTIFACTS:
                checksums[artifact.key] = {
                    "path": str(bundled[artifact.key].relative_to(output_directory)),
                    "size": firmware_metadata[artifact.key]["size"],
                    "sha256": firmware_metadata[artifact.key]["sha256"],
                    "flash_offset": hex(artifact.offset),
                }
            checksums.update(
                {
                    "plaintext_nvs": {
                        "path": nvs_name,
                        "size": len(image),
                        "sha256": hashlib.sha256(image).hexdigest(),
                        "flash_offset": hex(secure.NVS_PARTITION_OFFSET),
                    },
                    "signing_public_key": {
                        "path": str(public_key.relative_to(output_directory)),
                        "size": key_metadata["size"],
                        "sha256": key_metadata["sha256"],
                        "flash_offset": None,
                    },
                    "partition_contract": {
                        "path": str(partition_contract.relative_to(output_directory)),
                        "size": partition_size,
                        "sha256": partition_digest,
                        "flash_offset": None,
                    },
                }
            )
            private_checksums = {
                "schema_version": 1,
                "bench_only": True,
                "sensitive": True,
                "device_uuid": request.device_uuid,
                "hardware_profile": request.hardware_profile,
                "files": checksums,
                "recovery_image": recovery_evidence,
            }

            artifact_command = _bench_artifact_verifier_command(
                firmware_directory,
                request.firmware.version,
                request.hardware_profile,
            )
            verifier_text = _render_verifier(
                checksums_name,
                checksums,
                recovery_evidence,
                artifact_command,
            )

            mac_command = secure._shell_command(
                _bench_esptool_prefix(request) + ["read-mac"]
            )
            efuse_command = secure._shell_command(
                [
                    "espefuse",
                    "--chip",
                    secure.CHIP,
                    "--port",
                    request.serial_port,
                    "--before",
                    "no-reset",
                    "--after",
                    "no-reset",
                    "summary",
                ]
            )
            preflash_command = secure._shell_command(["python3", str(final_verifier)])
            erase_command = secure._shell_command(
                _bench_esptool_prefix(request) + ["erase-flash"]
            )
            layout = [
                (0x0, bundled["bootloader"]),
                (0x8000, bundled["partition_table"]),
                (secure.NVS_PARTITION_OFFSET, final_nvs),
                (0xD000, bundled["ota_data"]),
                (0x20000, bundled["application"]),
                (0x800000, bundled["assets"]),
            ]
            pairs = [
                value for offset, path in layout for value in (hex(offset), str(path))
            ]
            flash_command = secure._shell_command(
                _bench_esptool_prefix(request)
                + [
                    "write-flash",
                    "--flash-mode",
                    "dio",
                    "--flash-freq",
                    "80m",
                    "--flash-size",
                    "16MB",
                    *pairs,
                ]
            )
            verify_command = secure._shell_command(
                _bench_esptool_prefix(request) + ["verify-flash", *pairs]
            )

            manifest = {
                "schema_version": 1,
                "artifact": "provisions-kitchen-helper-reversible-bench-bundle",
                "bench_only": True,
                "commercial_pilot_approved": False,
                "created_at": created_text,
                "device": {
                    "profile": request.hardware_profile,
                    "uuid": request.device_uuid,
                    "hardware_serial": request.hardware_serial,
                },
                "credential": {
                    "format": "pvd1",
                    "key_id": request.credential_key_id,
                    "secret": "<redacted>",
                    "at_rest_protection": "none; physically extractable",
                },
                "wifi": {
                    "network_count": len(request.wifi_networks),
                    "ssids": ["<redacted>"] * len(request.wifi_networks),
                    "passwords": "<redacted>",
                    "at_rest_protection": "none; physically extractable",
                },
                "firmware": {
                    "version": request.firmware.version,
                    "hardware_secure_boot": False,
                    "flash_encryption": False,
                    "nvs_encryption": False,
                    "signed_OTA_image_verification": True,
                    "boot_image_validation_skip": False,
                    "eFuse_enablement_in_build": False,
                    "partition_layout": "Provisions 16 MB dual OTA",
                },
                "plaintext_nvs": {
                    "path": nvs_name,
                    "size": len(image),
                    "sha256": hashlib.sha256(image).hexdigest(),
                    "contains_extractable_wifi_and_device_credentials": True,
                },
                "external_recovery_image": recovery_evidence,
                "operator_gate": {
                    "expected_wifi_sta_mac": request.hardware_serial,
                    "read_mac_command": mac_command,
                    "efuse_summary_command": efuse_command,
                    "preflash_verifier_command": preflash_command,
                    "erase_command": erase_command,
                    "flash_command": flash_command,
                    "verify_flash_command": verify_command,
                    "hardware_touched_by_tool": False,
                    "efuse_modified_by_tool": False,
                    "stop_conditions": [
                        "Wi-Fi STA MAC differs from the registered hardware serial",
                        "Secure Boot or flash encryption is already enabled",
                        "UART download restrictions are enabled",
                        "external 16 MB recovery image is unavailable or fails SHA-256 verification",
                        "pre-flash verifier fails",
                        "any erase, flash, or verify command is interrupted or fails",
                    ],
                },
                "mandatory_exit": {
                    "revoke_device_credential": True,
                    "erase_entire_flash": True,
                    "restore_verified_factory_backup_or_approved_secure_firmware": True,
                    "retain_on_customer_or_yacht": False,
                },
            }
            command_text = f"""PROVISIONS KITCHEN HELPER REVERSIBLE BENCH COMMANDS — NOTHING EXECUTED

DEMO-ONLY INSECURE PATH. Wi-Fi and device credentials are physically extractable.
Never use this bundle for a commercial pilot or leave it on customer/yacht hardware.

Expected Wi-Fi STA MAC: {request.hardware_serial}
External recovery image: {recovery_image.path}
External recovery size: {recovery_image.size} bytes
External recovery SHA-256: {recovery_image.sha256}

1. Manually put the exact device in ROM download mode (green indication on
StopWatch). Every esptool command uses --before no-reset. Read the MAC; STOP on mismatch:
{mac_command}

2. Read eFuses; STOP if Secure Boot, flash encryption, or UART download restrictions are enabled:
{efuse_command}

3. Verify every bundled byte, the external recovery image, signed app, build
identity, version, and partition layout:
{preflash_command}

4. Authorized operator only — erase the exact connected device:
{erase_command}

5. Flash only this complete no-eFuse bench set. The nvs_keys slot stays erased:
{flash_command}

6. Before reset or boot, verify every flashed region:
{verify_command}

7. Reset manually for the bench test. This flow contains no burn-eFuse command.

MANDATORY AFTER TEST: revoke this device credential, erase the full flash, restore
the verified factory backup or an approved secure build, and retain evidence.
"""

            secure._write_private_text(
                work_directory / manifest_name,
                json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            )
            secure._write_private_text(
                work_directory / checksums_name,
                json.dumps(private_checksums, indent=2, sort_keys=True) + "\n",
            )
            secure._write_private_text(work_directory / verifier_name, verifier_text)
            secure._write_private_text(work_directory / commands_name, command_text)

            os.replace(nvs_path, final_nvs)
            os.replace(work_directory / manifest_name, final_manifest)
            os.replace(work_directory / checksums_name, final_checksums)
            os.replace(work_directory / verifier_name, final_verifier)
            os.replace(work_directory / commands_name, final_commands)

        for directory, subdirectories, files in os.walk(output_directory):
            Path(directory).chmod(0o700)
            for name in subdirectories:
                (Path(directory) / name).chmod(0o700)
            for name in files:
                (Path(directory) / name).chmod(0o600)
        return manifest
    except Exception:
        if created_output_identity is not None:
            _cleanup_created_output(output_directory, created_output_identity)
        raise


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Prepare a plaintext, no-eFuse Kitchen Helper bench bundle. "
            "This tool never touches hardware and its output is never pilot-safe."
        )
    )
    parser.add_argument(
        "--input",
        required=True,
        type=Path,
        help="absolute operator-owned 0600 schema-v2 registration output",
    )
    parser.add_argument(
        "--output-dir",
        required=True,
        type=Path,
        help="new absolute private directory outside the repository",
    )
    parser.add_argument(
        "--recovery-image",
        required=True,
        type=Path,
        help="absolute path to the independently verified full 16 MB backup",
    )
    parser.add_argument(
        "--recovery-sha256",
        required=True,
        help="independently recorded lowercase SHA-256 of the recovery image",
    )
    return parser


def main(arguments: list[str] | None = None) -> int:
    options = _build_parser().parse_args(arguments)
    try:
        request = secure.load_private_input(options.input)
        manifest = generate_bench_bundle(
            request,
            options.output_dir,
            options.recovery_image,
            options.recovery_sha256,
        )
    except secure.ProvisioningError:
        print(
            "Bench bundle failed: private validation rejected; no bundle retained",
            file=sys.stderr,
        )
        return 2
    except Exception:  # noqa: BLE001 - suppress secret-bearing runtime state
        print(
            "Bench bundle failed: unexpected internal error; no bundle retained",
            file=sys.stderr,
        )
        return 3
    print("Reversible bench bundle prepared; no hardware or eFuses were changed.")
    print("WARNING: demo-only plaintext credentials; revoke and wipe after testing.")
    print(f"Device UUID: {manifest['device']['uuid']}")
    print(f"Hardware serial: {manifest['device']['hardware_serial']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
