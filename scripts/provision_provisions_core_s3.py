#!/usr/bin/env python3
"""Prepare a fail-closed Provisions CoreS3 factory provisioning bundle."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import uuid
from collections.abc import Callable
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[1]
BOARD_PROFILE = "provisions-kitchen-helper-core-s3"
BOARD_PROFILE_LITE = "provisions-kitchen-helper-core-s3-lite"
BOARD_PROFILES = frozenset((BOARD_PROFILE, BOARD_PROFILE_LITE))
BOARD_PROFILE_CONFIG_DEFINES = {
    BOARD_PROFILE: "#define CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_CORE_S3 1",
    BOARD_PROFILE_LITE: (
        "#define CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_CORE_S3_LITE 1"
    ),
}
BOARD_PROFILE_SDKCONFIG_OPTIONS = {
    profile: define.removeprefix("#define ").removesuffix(" 1") + "=y"
    for profile, define in BOARD_PROFILE_CONFIG_DEFINES.items()
}
SIGNED_HARDWARE_IDENTITY_MARKERS = {
    profile: f"PROVISIONS_SIGNED_HARDWARE_IDENTITY={profile}".encode("ascii")
    + b"\x00"
    for profile in BOARD_PROFILES
}
CHIP = "esp32s3"
NVS_PARTITION_OFFSET = 0x9000
NVS_PARTITION_SIZE = 0x4000
NVS_KEYS_PARTITION_OFFSET = 0x10000
NVS_KEYS_PARTITION_SIZE = 0x1000
PINNED_IDF_IMAGE = (
    "espressif/idf:v6.0.2@"
    "sha256:0d8c9773d48a327233f9c1d7c654ff0bcf133ae24503ea2e97a57cfe02b8cb67"
)
IDF_PYTHON = "/opt/esp/python_env/idf6.0_py3.12_env/bin/python"
NVS_GENERATOR = (
    "/opt/esp/idf/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
)
NVS_TOOL = "/opt/esp/idf/components/nvs_flash/nvs_partition_tool/nvs_tool.py"
PARTITION_GENERATOR = "/opt/esp/idf/components/partition_table/gen_esp32part.py"
PILOT_CONFIG = REPO_ROOT / "main/boards/m5stack/provisions-core-s3/pilot_profile.json"
PILOT_PARTITION_TABLE = REPO_ROOT / "partitions/provisions/16m.csv"
MAX_INPUT_BYTES = 16 * 1024
MAX_METADATA_BYTES = 1024 * 1024
TOKEN_PATTERN = re.compile(
    r"^pvd1_"
    r"([0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12})"
    r"\.([A-Za-z0-9_-]{43})$"
)
MAC_PATTERN = re.compile(r"^[0-9a-f]{2}(?::[0-9a-f]{2}){5}$")
SERIAL_PORT_PATTERN = re.compile(r"^/dev/(?:cu\.|tty)[A-Za-z0-9._-]+$")
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
RELEASE_VERSION_PATTERN = re.compile(
    r"^(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})$"
)


@dataclass(frozen=True)
class FirmwareArtifact:
    key: str
    relative_path: Path
    offset: int
    maximum_size: int
    signed: bool = False


FIRMWARE_ARTIFACTS = (
    FirmwareArtifact(
        "bootloader", Path("bootloader/bootloader.bin"), 0x0, 0x8000, signed=True
    ),
    FirmwareArtifact(
        "partition_table", Path("partition_table/partition-table.bin"), 0x8000, 0x1000
    ),
    FirmwareArtifact("ota_data", Path("ota_data_initial.bin"), 0xD000, 0x2000),
    FirmwareArtifact(
        "application", Path("xiaozhi.bin"), 0x20000, 0x3F0000, signed=True
    ),
    FirmwareArtifact("assets", Path("generated_assets.bin"), 0x800000, 0x800000),
)
FIRMWARE_HASH_KEYS = {artifact.key for artifact in FIRMWARE_ARTIFACTS}

PILOT_SHARED_SDKCONFIG_DEFINES = {
    "#define CONFIG_PARTITION_TABLE_CUSTOM 1",
    '#define CONFIG_PARTITION_TABLE_CUSTOM_FILENAME "partitions/provisions/16m.csv"',
    "#define CONFIG_SECURE_BOOT 1",
    "#define CONFIG_SECURE_BOOT_V2_ENABLED 1",
    "#define CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME 1",
    "#define CONFIG_SECURE_FLASH_ENC_ENABLED 1",
    "#define CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE 1",
    "#define CONFIG_NVS_ENCRYPTION 1",
    "#define CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC 1",
    "#define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1",
    "#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1",
    "#define CONFIG_WAKE_WORD_DISABLED 1",
}
PILOT_SDKCONFIG_DEFINES = PILOT_SHARED_SDKCONFIG_DEFINES | {
    BOARD_PROFILE_CONFIG_DEFINES[BOARD_PROFILE]
}
FORBIDDEN_PILOT_SDKCONFIG_DEFINES = {
    "#define CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES 1",
    "#define CONFIG_SECURE_BOOT_INSECURE 1",
    "#define CONFIG_SECURE_BOOT_V2_ECDSA_INSECURE 1",
    "#define CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT 1",
    "#define CONFIG_BOOTLOADER_SKIP_VALIDATE_ALWAYS 1",
    "#define CONFIG_CAMERA_GC0308 1",
    "#define CONFIG_ESP_VIDEO_ENABLE_DVP_VIDEO_DEVICE 1",
}


class ProvisioningError(Exception):
    """A safe, operator-facing provisioning failure."""


@dataclass(frozen=True, repr=False)
class FirmwareInput:
    version: str
    artifact_directory: Path
    approved_sha256: dict[str, str]
    signing_public_key: Path
    signing_public_key_sha256: str


@dataclass(frozen=True, repr=False)
class WifiNetwork:
    role: str
    ssid: str
    password: str


@dataclass(frozen=True, repr=False)
class ProvisioningInput:
    hardware_profile: str
    device_uuid: str
    hardware_serial: str
    device_credential: str
    credential_key_id: str
    wifi_networks: tuple[WifiNetwork, ...]
    serial_port: str
    firmware: FirmwareInput


def _is_within(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def _require_exact_keys(value: dict[str, Any], expected: set[str], label: str) -> None:
    if set(value) != expected:
        raise ProvisioningError(f"{label} has missing or unsupported fields")


def _require_string(value: Any, label: str) -> str:
    if not isinstance(value, str):
        raise ProvisioningError(f"{label} must be a string")
    return value


def _require_safe_absolute_path(value: Any, label: str) -> Path:
    raw_path = _require_string(value, label)
    if any(character in raw_path for character in (",", "\r", "\n", "\x00")):
        raise ProvisioningError(f"{label} contains unsupported characters")
    path = Path(raw_path)
    if not path.is_absolute():
        raise ProvisioningError(f"{label} must be absolute")
    return path


def _parse_uuid_v4(value: str, label: str) -> str:
    try:
        parsed = uuid.UUID(value)
    except (ValueError, AttributeError):
        raise ProvisioningError(
            f"{label} must be a canonical lowercase UUID v4"
        ) from None
    if parsed.version != 4 or str(parsed) != value:
        raise ProvisioningError(f"{label} must be a canonical lowercase UUID v4")
    return value


def _is_canonical_release_version(value: str) -> bool:
    match = RELEASE_VERSION_PATTERN.fullmatch(value)
    return match is not None and all(
        int(component) <= 65535 for component in match.groups()
    )


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ProvisioningError("input JSON contains a duplicate field")
        result[key] = value
    return result


def parse_provisioning_document(raw: bytes) -> ProvisioningInput:
    if not raw or len(raw) > MAX_INPUT_BYTES:
        raise ProvisioningError("input JSON size is invalid")
    try:
        document = json.loads(
            raw.decode("utf-8"), object_pairs_hook=_reject_duplicate_keys
        )
    except ProvisioningError:
        raise
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise ProvisioningError("input JSON is invalid") from None

    if not isinstance(document, dict):
        raise ProvisioningError("input JSON must be an object")
    schema_version = document.get("schema_version")
    common_keys = {
        "schema_version",
        "device_uuid",
        "hardware_serial",
        "device_credential",
        "wifi",
        "serial_port",
        "firmware",
    }
    if type(schema_version) is not int or schema_version not in (1, 2):
        raise ProvisioningError("schema_version must be 1 or 2")
    if schema_version == 1:
        _require_exact_keys(document, common_keys, "input JSON")
        hardware_profile = BOARD_PROFILE
    else:
        _require_exact_keys(
            document, common_keys | {"hardware_profile"}, "input JSON"
        )
        hardware_profile = _require_string(
            document["hardware_profile"], "hardware_profile"
        )
        if hardware_profile not in BOARD_PROFILES:
            raise ProvisioningError("hardware_profile is not an approved build identity")

    device_uuid = _parse_uuid_v4(
        _require_string(document["device_uuid"], "device_uuid"), "device_uuid"
    )

    hardware_serial = _require_string(document["hardware_serial"], "hardware_serial")
    if not MAC_PATTERN.fullmatch(hardware_serial):
        raise ProvisioningError(
            "hardware_serial must be a canonical lowercase Wi-Fi MAC"
        )
    mac_bytes = bytes.fromhex(hardware_serial.replace(":", ""))
    if mac_bytes in (b"\x00" * 6, b"\xff" * 6) or mac_bytes[0] & 0x01:
        raise ProvisioningError("hardware_serial must be a unicast hardware MAC")

    device_credential = _require_string(
        document["device_credential"], "device_credential"
    )
    token_match = TOKEN_PATTERN.fullmatch(device_credential)
    if token_match is None:
        raise ProvisioningError("device_credential is malformed")
    credential_key_id = _parse_uuid_v4(token_match.group(1), "credential key UUID")

    wifi = document["wifi"]
    if not isinstance(wifi, dict):
        raise ProvisioningError("wifi must be an object")
    _require_exact_keys(wifi, {"networks"}, "wifi")
    networks = wifi["networks"]
    if not isinstance(networks, list) or not 1 <= len(networks) <= 2:
        raise ProvisioningError("wifi.networks must contain one or two networks")
    wifi_networks: list[WifiNetwork] = []
    expected_roles = ("primary", "fallback")
    for index, network in enumerate(networks):
        if not isinstance(network, dict):
            raise ProvisioningError("each Wi-Fi network must be an object")
        _require_exact_keys(
            network, {"role", "ssid", "password", "band"}, "Wi-Fi network"
        )
        role = _require_string(network["role"], "wifi network role")
        if role != expected_roles[index]:
            raise ProvisioningError(
                "Wi-Fi networks must be ordered primary, then optional fallback"
            )
        if _require_string(network["band"], "wifi network band") != "2.4GHz":
            raise ProvisioningError("each Wi-Fi network band must be exactly 2.4GHz")

        ssid = _require_string(network["ssid"], "wifi network ssid")
        try:
            ssid_bytes = ssid.encode("utf-8")
        except UnicodeEncodeError:
            raise ProvisioningError("Wi-Fi SSID must be valid UTF-8") from None
        if not 1 <= len(ssid_bytes) <= 32 or any(
            ord(character) < 0x20 or ord(character) == 0x7F for character in ssid
        ):
            raise ProvisioningError(
                "Wi-Fi SSID must be 1-32 UTF-8 bytes without controls"
            )

        password = _require_string(network["password"], "wifi network password")
        printable_ascii = all(0x20 <= ord(character) <= 0x7E for character in password)
        password_is_passphrase = 8 <= len(password) <= 63 and printable_ascii
        password_is_raw_psk = len(password) == 64 and bool(
            re.fullmatch(r"[0-9A-Fa-f]{64}", password)
        )
        if not (password_is_passphrase or password_is_raw_psk):
            raise ProvisioningError(
                "Wi-Fi password must be an 8-63 character printable passphrase or 64 hex PSK"
            )
        wifi_networks.append(WifiNetwork(role=role, ssid=ssid, password=password))
    if len({network.ssid for network in wifi_networks}) != len(wifi_networks):
        raise ProvisioningError("primary and fallback Wi-Fi SSIDs must be distinct")

    serial_port = _require_string(document["serial_port"], "serial_port")
    if not SERIAL_PORT_PATTERN.fullmatch(serial_port):
        raise ProvisioningError(
            "serial_port must be an explicit /dev/cu.* or /dev/tty* path"
        )

    firmware = document["firmware"]
    if not isinstance(firmware, dict):
        raise ProvisioningError("firmware must be an object")
    _require_exact_keys(
        firmware,
        {"version", "artifact_directory", "sha256", "signing_public_key"},
        "firmware",
    )
    firmware_version = _require_string(firmware["version"], "firmware.version")
    if not _is_canonical_release_version(firmware_version):
        raise ProvisioningError(
            "firmware.version must be canonical three-part 0-65535 components"
        )
    artifact_directory = _require_safe_absolute_path(
        firmware["artifact_directory"], "firmware.artifact_directory"
    )
    approved_hashes = firmware["sha256"]
    if not isinstance(approved_hashes, dict):
        raise ProvisioningError("firmware.sha256 must be an object")
    _require_exact_keys(approved_hashes, FIRMWARE_HASH_KEYS, "firmware.sha256")
    validated_hashes: dict[str, str] = {}
    for key in sorted(FIRMWARE_HASH_KEYS):
        digest = _require_string(approved_hashes[key], f"firmware.sha256.{key}")
        if not SHA256_PATTERN.fullmatch(digest):
            raise ProvisioningError(
                f"firmware.sha256.{key} must be a lowercase SHA-256"
            )
        validated_hashes[key] = digest

    signing_public_key = firmware["signing_public_key"]
    if not isinstance(signing_public_key, dict):
        raise ProvisioningError("firmware.signing_public_key must be an object")
    _require_exact_keys(
        signing_public_key, {"path", "sha256"}, "firmware.signing_public_key"
    )
    signing_public_key_path = _require_safe_absolute_path(
        signing_public_key["path"], "firmware.signing_public_key.path"
    )
    signing_public_key_sha256 = _require_string(
        signing_public_key["sha256"], "firmware.signing_public_key.sha256"
    )
    if not SHA256_PATTERN.fullmatch(signing_public_key_sha256):
        raise ProvisioningError(
            "firmware.signing_public_key.sha256 must be a lowercase SHA-256"
        )

    return ProvisioningInput(
        hardware_profile=hardware_profile,
        device_uuid=device_uuid,
        hardware_serial=hardware_serial,
        device_credential=device_credential,
        credential_key_id=credential_key_id,
        wifi_networks=tuple(wifi_networks),
        serial_port=serial_port,
        firmware=FirmwareInput(
            version=firmware_version,
            artifact_directory=artifact_directory,
            approved_sha256=validated_hashes,
            signing_public_key=signing_public_key_path,
            signing_public_key_sha256=signing_public_key_sha256,
        ),
    )


def load_private_input(path: Path) -> ProvisioningInput:
    if not path.is_absolute():
        raise ProvisioningError("input path must be absolute")
    if _is_within(path.resolve(strict=False), REPO_ROOT):
        raise ProvisioningError("input JSON must be outside the repository")

    try:
        metadata = path.lstat()
    except OSError:
        raise ProvisioningError("input JSON is unavailable") from None
    if not stat.S_ISREG(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode):
        raise ProvisioningError("input JSON must be a regular file, not a link")
    if metadata.st_uid != os.getuid() or metadata.st_nlink != 1:
        raise ProvisioningError("input JSON must be owned only by the current operator")
    if stat.S_IMODE(metadata.st_mode) & 0o077:
        raise ProvisioningError("input JSON permissions must be 0600 or stricter")

    flags = os.O_RDONLY
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags)
        with os.fdopen(descriptor, "rb", closefd=True) as input_file:
            opened = os.fstat(input_file.fileno())
            if (opened.st_dev, opened.st_ino) != (metadata.st_dev, metadata.st_ino):
                raise ProvisioningError("input JSON changed while it was opened")
            raw = input_file.read(MAX_INPUT_BYTES + 1)
    except ProvisioningError:
        raise
    except OSError:
        raise ProvisioningError("input JSON could not be read safely") from None
    return parse_provisioning_document(raw)


def _validate_firmware_contract(board_profile: str = BOARD_PROFILE) -> None:
    if board_profile not in BOARD_PROFILES:
        raise ProvisioningError("CoreS3 hardware profile is not approved")
    try:
        config = json.loads(PILOT_CONFIG.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        raise ProvisioningError("CoreS3 pilot configuration is unavailable") from None
    if config.get("type") != BOARD_PROFILE or config.get("target") != CHIP:
        raise ProvisioningError(
            "CoreS3 pilot configuration no longer matches this tool"
        )
    builds = config.get("builds")
    if not isinstance(builds, list) or len(builds) != len(BOARD_PROFILES):
        raise ProvisioningError("CoreS3 pilot configuration has an unsafe build set")
    builds_by_name = {
        build.get("name"): build for build in builds if isinstance(build, dict)
    }
    if set(builds_by_name) != BOARD_PROFILES:
        raise ProvisioningError("CoreS3 pilot build identities no longer match")
    for profile, profile_build in builds_by_name.items():
        profile_options = profile_build.get("sdkconfig_append")
        other_profile_options = {
            option
            for other_profile, option in BOARD_PROFILE_SDKCONFIG_OPTIONS.items()
            if other_profile != profile
        }
        if (
            not isinstance(profile_options, list)
            or BOARD_PROFILE_SDKCONFIG_OPTIONS[profile] not in profile_options
            or other_profile_options.intersection(profile_options)
        ):
            raise ProvisioningError("CoreS3 pilot build identity is not profile-bound")
    build = builds_by_name[board_profile]
    sdkconfig_append = build.get("sdkconfig_append")
    required_options = {
        "CONFIG_CAMERA_GC0308=n",
        "CONFIG_ESP_VIDEO_ENABLE_DVP_VIDEO_DEVICE=n",
        "CONFIG_PARTITION_TABLE_CUSTOM=y",
        'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/provisions/16m.csv"',
        "CONFIG_SECURE_BOOT=y",
        "CONFIG_SECURE_BOOT_V2_ENABLED=y",
        "CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME=y",
        "CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n",
        "CONFIG_SECURE_FLASH_ENC_ENABLED=y",
        "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y",
        "CONFIG_NVS_ENCRYPTION=y",
        "CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC=y",
    }
    if not isinstance(sdkconfig_append, list) or not required_options.issubset(
        set(sdkconfig_append)
    ):
        raise ProvisioningError("CoreS3 pilot security options are incomplete")

    try:
        with PILOT_PARTITION_TABLE.open(encoding="utf-8", newline="") as table_file:
            rows = [
                [field.strip() for field in row]
                for row in csv.reader(
                    line
                    for line in table_file
                    if line.strip() and not line.lstrip().startswith("#")
                )
            ]
    except OSError:
        raise ProvisioningError("CoreS3 pilot partition table is unavailable") from None
    expected_rows = [
        ["nvs", "data", "nvs", "0x9000", "0x4000", ""],
        ["otadata", "data", "ota", "0xd000", "0x2000", ""],
        ["phy_init", "data", "phy", "0xf000", "0x1000", ""],
        ["nvs_keys", "data", "nvs_keys", "0x10000", "0x1000", "encrypted"],
        ["ota_0", "app", "ota_0", "0x20000", "0x3f0000", ""],
        ["ota_1", "app", "ota_1", "", "0x3f0000", ""],
        ["assets", "data", "spiffs", "0x800000", "8M", ""],
    ]
    if rows != expected_rows:
        raise ProvisioningError(
            "CoreS3 pilot partition layout no longer matches this tool"
        )


def _safe_artifact_file(artifact_directory: Path, relative_path: Path) -> Path:
    current = artifact_directory
    for component in relative_path.parts:
        current = current / component
        try:
            metadata = current.lstat()
        except OSError:
            raise ProvisioningError(
                "pilot firmware artifact set is incomplete"
            ) from None
        if stat.S_ISLNK(metadata.st_mode):
            raise ProvisioningError("pilot firmware artifact path contains a link")
    try:
        resolved = current.resolve(strict=True)
        artifact_root = artifact_directory.resolve(strict=True)
    except OSError:
        raise ProvisioningError("pilot firmware artifact path is unavailable") from None
    if not _is_within(resolved, artifact_root) or not stat.S_ISREG(metadata.st_mode):
        raise ProvisioningError("pilot firmware artifact path is unsafe")
    return current


def _read_bounded_text(path: Path, label: str) -> str:
    try:
        with path.open("rb") as input_file:
            raw = input_file.read(MAX_METADATA_BYTES + 1)
    except OSError:
        raise ProvisioningError(f"{label} is unavailable") from None
    if len(raw) > MAX_METADATA_BYTES:
        raise ProvisioningError(f"{label} is unexpectedly large")
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        raise ProvisioningError(f"{label} is invalid") from None


def _hash_file(path: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    try:
        with path.open("rb") as input_file:
            while chunk := input_file.read(1024 * 1024):
                digest.update(chunk)
                size += len(chunk)
    except OSError:
        raise ProvisioningError("pilot firmware artifact could not be read") from None
    return digest.hexdigest(), size


def _validate_signed_application_identity(path: Path, board_profile: str) -> None:
    try:
        image = path.read_bytes()
    except OSError:
        raise ProvisioningError("signed application identity is unavailable") from None
    expected_marker = SIGNED_HARDWARE_IDENTITY_MARKERS[board_profile]
    if image.count(expected_marker) != 1 or any(
        marker in image
        for profile, marker in SIGNED_HARDWARE_IDENTITY_MARKERS.items()
        if profile != board_profile
    ):
        raise ProvisioningError(
            "signed application identity does not match the requested hardware profile"
        )


def _validate_firmware_artifacts(
    firmware: FirmwareInput,
    board_profile: str = BOARD_PROFILE,
) -> tuple[dict[str, dict[str, Any]], dict[str, Any]]:
    if board_profile not in BOARD_PROFILES:
        raise ProvisioningError("CoreS3 hardware profile is not approved")
    try:
        artifact_metadata = firmware.artifact_directory.lstat()
    except OSError:
        raise ProvisioningError("firmware.artifact_directory is unavailable") from None
    if not stat.S_ISDIR(artifact_metadata.st_mode) or stat.S_ISLNK(
        artifact_metadata.st_mode
    ):
        raise ProvisioningError("firmware.artifact_directory must be a real directory")

    project_path = _safe_artifact_file(
        firmware.artifact_directory, Path("project_description.json")
    )
    try:
        project = json.loads(_read_bounded_text(project_path, "pilot build metadata"))
    except json.JSONDecodeError:
        raise ProvisioningError("pilot build metadata is invalid") from None
    if (
        not isinstance(project, dict)
        or project.get("project_name") != "xiaozhi"
        or project.get("target") != CHIP
        or project.get("project_version") != firmware.version
    ):
        raise ProvisioningError("pilot build metadata does not match the request")

    sdkconfig_path = _safe_artifact_file(
        firmware.artifact_directory, Path("config/sdkconfig.h")
    )
    sdkconfig_lines = set(
        _read_bounded_text(sdkconfig_path, "pilot sdkconfig").splitlines()
    )
    expected_defines = PILOT_SHARED_SDKCONFIG_DEFINES | {
        BOARD_PROFILE_CONFIG_DEFINES[board_profile]
    }
    if not expected_defines.issubset(sdkconfig_lines):
        raise ProvisioningError("build is not the approved secure pilot profile")
    other_profile_defines = {
        define
        for profile, define in BOARD_PROFILE_CONFIG_DEFINES.items()
        if profile != board_profile
    }
    if other_profile_defines.intersection(sdkconfig_lines):
        raise ProvisioningError("build identity does not match the requested hardware profile")
    if FORBIDDEN_PILOT_SDKCONFIG_DEFINES.intersection(sdkconfig_lines):
        raise ProvisioningError("build enables a forbidden development security mode")
    if any(
        line.startswith("#define CONFIG_SECURE_BOOT_SIGNING_KEY ")
        for line in sdkconfig_lines
    ):
        raise ProvisioningError("build metadata references a private signing key")

    metadata: dict[str, dict[str, Any]] = {}
    for artifact in FIRMWARE_ARTIFACTS:
        path = _safe_artifact_file(firmware.artifact_directory, artifact.relative_path)
        digest, size = _hash_file(path)
        if digest != firmware.approved_sha256[artifact.key]:
            raise ProvisioningError("pilot firmware artifact hash is not approved")
        if size == 0 or size > artifact.maximum_size:
            raise ProvisioningError("pilot firmware artifact size is invalid")
        if artifact.key == "application":
            _validate_signed_application_identity(path, board_profile)
        metadata[artifact.key] = {
            "source": path,
            "relative_path": artifact.relative_path.as_posix(),
            "offset": artifact.offset,
            "size": size,
            "sha256": digest,
            "signed": artifact.signed,
        }

    try:
        public_key_metadata = firmware.signing_public_key.lstat()
    except OSError:
        raise ProvisioningError("approved signing public key is unavailable") from None
    if not stat.S_ISREG(public_key_metadata.st_mode) or stat.S_ISLNK(
        public_key_metadata.st_mode
    ):
        raise ProvisioningError("approved signing public key path is unsafe")
    public_key_digest, public_key_size = _hash_file(firmware.signing_public_key)
    if public_key_digest != firmware.signing_public_key_sha256:
        raise ProvisioningError("signing public key hash is not approved")
    if not 128 <= public_key_size <= 16 * 1024:
        raise ProvisioningError("signing public key size is invalid")

    return metadata, {
        "source": firmware.signing_public_key,
        "size": public_key_size,
        "sha256": public_key_digest,
    }


def _nvs_rows(request: ProvisioningInput) -> list[list[str]]:
    rows = [
        ["key", "type", "encoding", "value"],
        ["wifi", "namespace", "", ""],
    ]
    for index, network in enumerate(request.wifi_networks):
        suffix = "" if index == 0 else str(index)
        rows.extend(
            [
                [f"ssid{suffix}", "data", "string", network.ssid],
                [f"password{suffix}", "data", "string", network.password],
            ]
        )
    rows.extend(
        [
            ["board", "namespace", "", ""],
            ["uuid", "data", "string", request.device_uuid],
            ["provisions", "namespace", "", ""],
            ["device_token", "data", "string", request.device_credential],
        ]
    )
    return rows


def _write_private_text(path: Path, text: str) -> None:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8", newline="") as output_file:
        output_file.write(text)


def _write_nvs_csv(path: Path, request: ProvisioningInput) -> None:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8", newline="") as csv_file:
        writer = csv.writer(csv_file, lineterminator="\n")
        writer.writerows(_nvs_rows(request))


def _best_effort_wipe(path: Path) -> None:
    """Reduce secret residue in the temporary CSV before unlinking it."""
    try:
        size = path.stat().st_size
        with path.open("r+b", buffering=0) as secret_file:
            secret_file.write(b"\x00" * size)
            os.fsync(secret_file.fileno())
    except OSError:
        # The enclosing private temporary directory is still removed on every path.
        pass


def _docker_security_arguments() -> list[str]:
    return [
        "--rm",
        "--pull=never",
        "--network=none",
        "--read-only",
        "--cap-drop=ALL",
        "--security-opt=no-new-privileges",
        "--pids-limit=64",
        "--memory=256m",
        "--cpus=1",
        "--user",
        f"{os.getuid()}:{os.getgid()}",
    ]


def _run_secret_processing_command(
    command: list[str], failure_message: str, *, timeout: int = 90
) -> subprocess.CompletedProcess[bytes]:
    try:
        result = subprocess.run(
            command,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            check=False,
            timeout=timeout,
        )
    except FileNotFoundError:
        raise ProvisioningError("Docker is required but unavailable") from None
    except subprocess.TimeoutExpired:
        raise ProvisioningError(failure_message) from None
    if result.returncode != 0:
        # Output is deliberately suppressed because a command may have read secrets.
        raise ProvisioningError(failure_message)
    return result


def _pilot_artifact_verifier_command(
    firmware_directory: Path, expected_version: str, expected_profile: str
) -> list[str]:
    if expected_profile not in BOARD_PROFILES:
        raise ProvisioningError("CoreS3 hardware profile is not approved")
    identity_markers = tuple(
        SIGNED_HARDWARE_IDENTITY_MARKERS[profile]
        for profile in sorted(BOARD_PROFILES)
    )
    firmware_mount = (
        f"type=bind,source={firmware_directory},target=/pilot-release,readonly"
    )
    verification_script = f"""set -eu
PY=/opt/esp/python_env/idf6.0_py3.12_env/bin/python
$PY -m espsecure verify-signature --version 2 --keyfile /pilot-release/signing-public-key.pem /pilot-release/bootloader.bin >/dev/null 2>&1
$PY -m espsecure verify-signature --version 2 --keyfile /pilot-release/signing-public-key.pem /pilot-release/application.bin >/dev/null 2>&1
$PY -m esptool --chip esp32s3 image-info /pilot-release/application.bin >/work/app-info.txt 2>/dev/null
grep -Fqx 'Project name: xiaozhi' /work/app-info.txt
grep -Fqx 'App version: {expected_version}' /work/app-info.txt
grep -Fqx 'ESP-IDF: v6.0.2' /work/app-info.txt
$PY - <<'PY'
from pathlib import Path
image = Path('/pilot-release/application.bin').read_bytes()
expected = {SIGNED_HARDWARE_IDENTITY_MARKERS[expected_profile]!r}
markers = {identity_markers!r}
if image.count(expected) != 1 or any(marker != expected and marker in image for marker in markers):
    raise SystemExit(1)
PY
$PY /opt/esp/idf/components/partition_table/gen_esp32part.py /pilot-release/pilot-partitions.csv /work/expected-partition-table.bin >/dev/null 2>&1
cmp /work/expected-partition-table.bin /pilot-release/partition_table.bin >/dev/null
"""
    return [
        "docker",
        "run",
        *_docker_security_arguments(),
        "--tmpfs",
        "/work:rw,noexec,nosuid,nodev,size=1m",
        "--mount",
        firmware_mount,
        "--entrypoint",
        "sh",
        PINNED_IDF_IMAGE,
        "-c",
        verification_script,
    ]


def _run_pilot_artifact_verifier(
    firmware_directory: Path, expected_version: str, expected_profile: str
) -> None:
    command = _pilot_artifact_verifier_command(
        firmware_directory, expected_version, expected_profile
    )
    _run_secret_processing_command(
        command,
        "signed pilot artifact verification failed; reject this build",
    )


def _run_nvs_generator(work_directory: Path) -> None:
    mount = f"type=bind,source={work_directory},target=/work"
    command = [
        "docker",
        "run",
        *_docker_security_arguments(),
        "--tmpfs",
        "/tmp:rw,noexec,nosuid,nodev,size=16m",
        "--mount",
        mount,
        "--workdir",
        "/work",
        "--entrypoint",
        IDF_PYTHON,
        PINNED_IDF_IMAGE,
        NVS_GENERATOR,
        "encrypt",
        "--version",
        "2",
        "--keygen",
        "--keyfile",
        "nvs_keys.bin",
        "/work/input.csv",
        "/work/device-nvs.bin",
        hex(NVS_PARTITION_SIZE),
    ]
    _run_secret_processing_command(
        command,
        "pinned encrypted-NVS generator failed; confirm the exact ESP-IDF image is installed",
    )


def _run_nvs_schema_inspector(work_directory: Path) -> list[dict[str, Any]]:
    mount = f"type=bind,source={work_directory},target=/work"
    inspection_script = """set -eu
PY=/opt/esp/python_env/idf6.0_py3.12_env/bin/python
$PY /opt/esp/idf/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py decrypt /work/device-nvs.bin /work/keys/nvs_keys.bin /work/decrypted-nvs.bin >/dev/null 2>&1
$PY /opt/esp/idf/components/nvs_flash/nvs_partition_tool/nvs_tool.py /work/decrypted-nvs.bin --dump minimal --format json --color never
"""
    command = [
        "docker",
        "run",
        *_docker_security_arguments(),
        "--tmpfs",
        "/tmp:rw,noexec,nosuid,nodev,size=16m",
        "--mount",
        mount,
        "--entrypoint",
        "sh",
        PINNED_IDF_IMAGE,
        "-c",
        inspection_script,
    ]
    try:
        result = _run_secret_processing_command(
            command, "encrypted NVS schema verification failed"
        )
        try:
            entries = json.loads(result.stdout)
        except (UnicodeDecodeError, json.JSONDecodeError):
            raise ProvisioningError(
                "encrypted NVS schema verification failed"
            ) from None
        if not isinstance(entries, list) or not all(
            isinstance(entry, dict) for entry in entries
        ):
            raise ProvisioningError("encrypted NVS schema verification failed")
        return entries
    finally:
        _best_effort_wipe(work_directory / "decrypted-nvs.bin")


def _verify_nvs_images(
    image_path: Path,
    key_path: Path,
    request: ProvisioningInput,
    inspector: Callable[[Path], list[dict[str, Any]]] | None,
    work_directory: Path,
) -> tuple[bytes, bytes]:
    try:
        image = image_path.read_bytes()
        key_image = key_path.read_bytes()
    except OSError:
        raise ProvisioningError(
            "encrypted NVS generator did not produce both required images"
        ) from None
    if len(image) != NVS_PARTITION_SIZE:
        raise ProvisioningError("encrypted NVS image has an invalid partition size")
    if len(key_image) != NVS_KEYS_PARTITION_SIZE:
        raise ProvisioningError("NVS key image has an invalid partition size")
    sensitive_values = [request.device_uuid, request.device_credential]
    for network in request.wifi_networks:
        sensitive_values.extend((network.ssid, network.password))
    if any(
        len(value.encode("utf-8")) >= 8 and value.encode("utf-8") in image
        for value in sensitive_values
    ):
        raise ProvisioningError("encrypted NVS image contains exposed input data")

    entries = (inspector or _run_nvs_schema_inspector)(work_directory)
    expected_entries: list[dict[str, Any]] = []
    for index, network in enumerate(request.wifi_networks):
        suffix = "" if index == 0 else str(index)
        expected_entries.extend(
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
    expected_entries.extend(
        [
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
        ]
    )
    if entries != expected_entries:
        raise ProvisioningError("encrypted NVS output failed exact schema verification")
    return image, key_image


def _copy_approved_file(source: Path, destination: Path, expected_digest: str) -> int:
    try:
        before = source.lstat()
    except OSError:
        raise ProvisioningError("pilot firmware artifact became unavailable") from None
    if not stat.S_ISREG(before.st_mode) or stat.S_ISLNK(before.st_mode):
        raise ProvisioningError("pilot firmware artifact became unsafe")

    source_flags = os.O_RDONLY
    if hasattr(os, "O_NOFOLLOW"):
        source_flags |= os.O_NOFOLLOW
    try:
        source_descriptor = os.open(source, source_flags)
    except OSError:
        raise ProvisioningError("pilot firmware artifact could not be copied") from None
    try:
        destination_descriptor = os.open(
            destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600
        )
    except OSError:
        os.close(source_descriptor)
        raise ProvisioningError("pilot firmware artifact could not be copied") from None

    digest = hashlib.sha256()
    size = 0
    try:
        with (
            os.fdopen(source_descriptor, "rb", closefd=True) as input_file,
            os.fdopen(destination_descriptor, "wb", closefd=True) as output_file,
        ):
            opened = os.fstat(input_file.fileno())
            if (opened.st_dev, opened.st_ino) != (before.st_dev, before.st_ino):
                raise ProvisioningError("pilot firmware artifact changed while opened")
            while chunk := input_file.read(1024 * 1024):
                output_file.write(chunk)
                digest.update(chunk)
                size += len(chunk)
            output_file.flush()
            os.fsync(output_file.fileno())
    except Exception:
        destination.unlink(missing_ok=True)
        raise
    if digest.hexdigest() != expected_digest:
        destination.unlink(missing_ok=True)
        raise ProvisioningError("pilot firmware artifact changed after verification")
    destination.chmod(0o600)
    return size


def _esptool_prefix(request: ProvisioningInput) -> list[str]:
    return [
        "esptool",
        "--chip",
        CHIP,
        "--port",
        request.serial_port,
        "--baud",
        "460800",
        "--before",
        "default-reset",
        "--after",
        "no-reset",
    ]


def _shell_command(arguments: list[str]) -> str:
    return " ".join(shlex.quote(argument) for argument in arguments)


def _render_private_preflash_verifier(
    private_manifest_name: str,
    checksums: dict[str, dict[str, Any]],
    artifact_verifier_command: list[str],
) -> str:
    checksums_json = json.dumps(checksums, separators=(",", ":"))
    command_json = json.dumps(artifact_verifier_command, separators=(",", ":"))
    return f'''#!/usr/bin/env python3
"""Private, fail-closed verifier generated for one Provisions CoreS3 bundle."""

import hashlib
import json
import stat
import subprocess
import sys
from pathlib import Path

BUNDLE_ROOT = Path(__file__).resolve().parent
PRIVATE_MANIFEST = BUNDLE_ROOT / {private_manifest_name!r}
EXPECTED = json.loads({checksums_json!r})
ARTIFACT_VERIFIER_COMMAND = json.loads({command_json!r})


def fail() -> None:
    print("PRE-FLASH VERIFICATION FAILED; DO NOT ERASE OR FLASH", file=sys.stderr)
    raise SystemExit(2)


try:
    manifest = json.loads(PRIVATE_MANIFEST.read_text(encoding="utf-8"))
    if manifest.get("files") != EXPECTED:
        fail()
    approved_fingerprint = manifest.get("external_approval_gate", {{}}).get(
        "signing_public_key_sha256"
    )
    if approved_fingerprint != EXPECTED["signing_public_key"]["sha256"]:
        fail()
    for entry in EXPECTED.values():
        relative_path = Path(entry["path"])
        if relative_path.is_absolute() or ".." in relative_path.parts:
            fail()
        path = BUNDLE_ROOT / relative_path
        metadata = path.lstat()
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
            fail()
        digest = hashlib.sha256()
        size = 0
        with path.open("rb") as input_file:
            while chunk := input_file.read(1024 * 1024):
                digest.update(chunk)
                size += len(chunk)
        if size != entry["size"] or digest.hexdigest() != entry["sha256"]:
            fail()
    result = subprocess.run(
        ARTIFACT_VERIFIER_COMMAND,
        stdin=subprocess.DEVNULL,
        capture_output=True,
        check=False,
        timeout=90,
    )
    if result.returncode != 0:
        fail()
except (
    AttributeError,
    OSError,
    KeyError,
    TypeError,
    ValueError,
    subprocess.SubprocessError,
):
    fail()

print("PRE-FLASH VERIFICATION PASSED")
print(f"Signing public-key SHA-256: {{approved_fingerprint}}")
print("Compare that fingerprint with the independently approved release record.")
'''


def generate_bundle(
    request: ProvisioningInput,
    output_directory: Path,
    *,
    generator: Callable[[Path], None] | None = None,
    schema_inspector: Callable[[Path], list[dict[str, Any]]] | None = None,
    firmware_verifier: Callable[[Path, str, str], None] | None = None,
    now: Callable[[], datetime] | None = None,
) -> dict[str, Any]:
    _validate_firmware_contract(request.hardware_profile)
    firmware_metadata, public_key_metadata = _validate_firmware_artifacts(
        request.firmware, request.hardware_profile
    )
    if not output_directory.is_absolute():
        raise ProvisioningError("output directory must be absolute")
    resolved_output = output_directory.resolve(strict=False)
    if _is_within(resolved_output, REPO_ROOT):
        raise ProvisioningError("output directory must be outside the repository")
    if any(
        character in str(resolved_output) or character in str(output_directory)
        for character in (",", "\r", "\n")
    ):
        raise ProvisioningError("output directory contains unsupported characters")
    if output_directory.exists() or output_directory.is_symlink():
        raise ProvisioningError("output directory already exists")
    if not output_directory.parent.is_dir():
        raise ProvisioningError("output parent directory does not exist")
    if _is_within(
        resolved_output, request.firmware.artifact_directory.resolve(strict=False)
    ):
        raise ProvisioningError("output directory must be outside the artifact package")
    try:
        parent_metadata = output_directory.parent.stat()
    except OSError:
        raise ProvisioningError("output parent directory is unavailable") from None
    if (
        parent_metadata.st_uid != os.getuid()
        or stat.S_IMODE(parent_metadata.st_mode) & 0o022
    ):
        raise ProvisioningError(
            "output parent directory must be operator-owned and not group/world writable"
        )

    try:
        output_directory.mkdir(mode=0o700)
        output_directory.chmod(0o700)
    except OSError:
        raise ProvisioningError(
            "output directory could not be created securely"
        ) from None

    try:
        firmware_directory = output_directory / "firmware"
        firmware_directory.mkdir(mode=0o700)
        firmware_directory.chmod(0o700)
        bundled_firmware: dict[str, Path] = {}
        for artifact in FIRMWARE_ARTIFACTS:
            metadata = firmware_metadata[artifact.key]
            destination = firmware_directory / f"{artifact.key}.bin"
            copied_size = _copy_approved_file(
                metadata["source"], destination, metadata["sha256"]
            )
            if copied_size != metadata["size"]:
                raise ProvisioningError("pilot firmware artifact size changed")
            bundled_firmware[artifact.key] = destination
        bundled_public_key = firmware_directory / "signing-public-key.pem"
        copied_public_key_size = _copy_approved_file(
            public_key_metadata["source"],
            bundled_public_key,
            public_key_metadata["sha256"],
        )
        if copied_public_key_size != public_key_metadata["size"]:
            raise ProvisioningError("signing public key size changed")
        partition_contract_digest, partition_contract_size = _hash_file(
            PILOT_PARTITION_TABLE
        )
        bundled_partition_contract = firmware_directory / "pilot-partitions.csv"
        copied_partition_contract_size = _copy_approved_file(
            PILOT_PARTITION_TABLE,
            bundled_partition_contract,
            partition_contract_digest,
        )
        if copied_partition_contract_size != partition_contract_size:
            raise ProvisioningError("pilot partition contract size changed")
        (firmware_verifier or _run_pilot_artifact_verifier)(
            firmware_directory, request.firmware.version, request.hardware_profile
        )

        with tempfile.TemporaryDirectory(
            prefix=".work-", dir=output_directory
        ) as temporary:
            work_directory = Path(temporary)
            work_directory.chmod(0o700)
            csv_path = work_directory / "input.csv"
            generated_image_path = work_directory / "device-nvs.bin"
            generated_key_path = work_directory / "keys/nvs_keys.bin"
            _write_nvs_csv(csv_path, request)
            try:
                (generator or _run_nvs_generator)(work_directory)
            finally:
                _best_effort_wipe(csv_path)
            image, key_image = _verify_nvs_images(
                generated_image_path,
                generated_key_path,
                request,
                schema_inspector,
                work_directory,
            )
            generated_image_path.chmod(0o600)
            generated_key_path.chmod(0o600)

            image_name = f"provisions-core-s3-{request.device_uuid}-encrypted-nvs.bin"
            key_name = f"provisions-core-s3-{request.device_uuid}-nvs-keys.bin"
            manifest_name = f"provisions-core-s3-{request.device_uuid}-manifest.json"
            private_manifest_name = (
                f"provisions-core-s3-{request.device_uuid}-private-checksums.json"
            )
            verifier_name = (
                f"provisions-core-s3-{request.device_uuid}-verify-before-flash.py"
            )
            command_name = f"provisions-core-s3-{request.device_uuid}-flash.txt"
            final_image_path = output_directory / image_name
            final_key_path = output_directory / key_name
            final_manifest_path = output_directory / manifest_name
            final_private_manifest_path = output_directory / private_manifest_name
            final_verifier_path = output_directory / verifier_name
            final_command_path = output_directory / command_name

            read_mac_command = _shell_command(_esptool_prefix(request) + ["read-mac"])
            efuse_summary_command = _shell_command(
                [
                    "espefuse",
                    "--chip",
                    CHIP,
                    "--port",
                    request.serial_port,
                    "summary",
                ]
            )
            erase_command = _shell_command(_esptool_prefix(request) + ["erase-flash"])
            flash_pairs: list[str] = []
            flash_layout = [
                (0x0, bundled_firmware["bootloader"]),
                (0x8000, bundled_firmware["partition_table"]),
                (NVS_PARTITION_OFFSET, final_image_path),
                (0xD000, bundled_firmware["ota_data"]),
                (NVS_KEYS_PARTITION_OFFSET, final_key_path),
                (0x20000, bundled_firmware["application"]),
                (0x800000, bundled_firmware["assets"]),
            ]
            for offset, path in flash_layout:
                flash_pairs.extend((hex(offset), str(path)))
            flash_command = _shell_command(
                _esptool_prefix(request)
                + [
                    "write-flash",
                    "--flash-mode",
                    "dio",
                    "--flash-freq",
                    "80m",
                    "--flash-size",
                    "16MB",
                    *flash_pairs,
                ]
            )
            created_at = (now or (lambda: datetime.now(timezone.utc)))()
            if created_at.tzinfo is None:
                raise ProvisioningError("evidence timestamp must be timezone-aware")
            created_at_text = (
                created_at.astimezone(timezone.utc)
                .isoformat(timespec="seconds")
                .replace("+00:00", "Z")
            )
            checksums: dict[str, dict[str, Any]] = {}
            for artifact in FIRMWARE_ARTIFACTS:
                metadata = firmware_metadata[artifact.key]
                checksums[artifact.key] = {
                    "path": str(
                        bundled_firmware[artifact.key].relative_to(output_directory)
                    ),
                    "size": metadata["size"],
                    "sha256": metadata["sha256"],
                    "flash_offset": hex(artifact.offset),
                }
            checksums.update(
                {
                    "encrypted_nvs": {
                        "path": image_name,
                        "size": len(image),
                        "sha256": hashlib.sha256(image).hexdigest(),
                        "flash_offset": hex(NVS_PARTITION_OFFSET),
                    },
                    "nvs_keys": {
                        "path": key_name,
                        "size": len(key_image),
                        "sha256": hashlib.sha256(key_image).hexdigest(),
                        "flash_offset": hex(NVS_KEYS_PARTITION_OFFSET),
                    },
                    "signing_public_key": {
                        "path": str(bundled_public_key.relative_to(output_directory)),
                        "size": public_key_metadata["size"],
                        "sha256": public_key_metadata["sha256"],
                        "flash_offset": None,
                    },
                    "pilot_partition_contract": {
                        "path": str(
                            bundled_partition_contract.relative_to(output_directory)
                        ),
                        "size": partition_contract_size,
                        "sha256": partition_contract_digest,
                        "flash_offset": None,
                    },
                }
            )
            private_checksum_manifest: dict[str, Any] = {
                "schema_version": 1,
                "sensitive": True,
                "created_at": created_at_text,
                "device_uuid": request.device_uuid,
                "hardware_profile": request.hardware_profile,
                "firmware_version": request.firmware.version,
                "external_approval_gate": {
                    "signing_public_key_sha256": public_key_metadata["sha256"],
                    "must_match_independently_approved_release_record": True,
                    "trust_root_independence_provided_by_bundle": False,
                },
                "files": checksums,
            }
            artifact_verifier_command = _pilot_artifact_verifier_command(
                firmware_directory,
                request.firmware.version,
                request.hardware_profile,
            )
            verifier_text = _render_private_preflash_verifier(
                private_manifest_name, checksums, artifact_verifier_command
            )
            preflash_verifier_command = _shell_command(
                ["python3", str(final_verifier_path)]
            )
            manifest: dict[str, Any] = {
                "schema_version": 2,
                "artifact": "provisions-core-s3-secure-factory-bundle",
                "created_at": created_at_text,
                "device": {
                    "profile": request.hardware_profile,
                    "uuid": request.device_uuid,
                    "hardware_serial": request.hardware_serial,
                },
                "credential": {
                    "format": "pvd1",
                    "key_id": request.credential_key_id,
                    "secret": "<redacted>",
                },
                "wifi": {
                    "network_count": len(request.wifi_networks),
                    "networks": [
                        {
                            "role": network.role,
                            "band_assertion": "2.4GHz",
                            "ssid": "<redacted>",
                            "password": "<redacted>",
                            "ssid_utf8_bytes": len(network.ssid.encode("utf-8")),
                        }
                        for network in request.wifi_networks
                    ],
                },
                "firmware": {
                    "profile": request.hardware_profile,
                    "version": request.firmware.version,
                    "source_artifact_directory": "<redacted>",
                    "approved_artifacts_verified": True,
                    "signing": {
                        "required_mode": "external_offline",
                        "public_key_bundle_path": str(
                            bundled_public_key.relative_to(output_directory)
                        ),
                        "public_key_sha256": public_key_metadata["sha256"],
                        "bootloader_signature_verified": True,
                        "application_signature_verified": True,
                        "private_key_reference_in_build_metadata": False,
                        "signing_custody_verified_by_tool": False,
                    },
                    "artifacts": {
                        artifact.key: {
                            "source_relative_path": firmware_metadata[artifact.key][
                                "relative_path"
                            ],
                            "bundle_path": str(
                                bundled_firmware[artifact.key].relative_to(
                                    output_directory
                                )
                            ),
                            "offset": hex(artifact.offset),
                            "size": firmware_metadata[artifact.key]["size"],
                            "sha256": firmware_metadata[artifact.key]["sha256"],
                            "signed_image": artifact.signed,
                        }
                        for artifact in FIRMWARE_ARTIFACTS
                    },
                },
                "nvs": {
                    "partition_offset": hex(NVS_PARTITION_OFFSET),
                    "partition_size": hex(NVS_PARTITION_SIZE),
                    "image": image_name,
                    "image_sha256": hashlib.sha256(image).hexdigest(),
                    "encryption": "XTS-AES",
                    "namespaces": {
                        "wifi": [
                            key
                            for index in range(len(request.wifi_networks))
                            for key in (
                                f"ssid{'' if index == 0 else index}",
                                f"password{'' if index == 0 else index}",
                            )
                        ],
                        "board": ["uuid"],
                        "provisions": ["device_token"],
                    },
                    "at_rest_protection": (
                        "encrypted_data_with_key_partition_protected_by_flash_encryption_after_first_boot"
                    ),
                },
                "nvs_keys": {
                    "partition_offset": hex(NVS_KEYS_PARTITION_OFFSET),
                    "partition_size": hex(NVS_KEYS_PARTITION_SIZE),
                    "image": key_name,
                    "sensitive": True,
                    "digest_intentionally_omitted": True,
                },
                "private_integrity_bundle": {
                    "checksum_manifest": private_manifest_name,
                    "preflash_verifier": verifier_name,
                    "contains_nvs_key_digest": True,
                    "sensitive": True,
                },
                "tooling": {
                    "idf_image": PINNED_IDF_IMAGE,
                    "nvs_generator": NVS_GENERATOR,
                },
                "operator_gate": {
                    "state": "prepared_not_security_ceremony_complete",
                    "expected_wifi_sta_mac": request.hardware_serial,
                    "read_mac_command": read_mac_command,
                    "efuse_summary_command": efuse_summary_command,
                    "preflash_verifier_command": preflash_verifier_command,
                    "erase_command": erase_command,
                    "flash_command": flash_command,
                    "flash_performed": False,
                    "efuse_modified": False,
                    "first_boot_authorized": False,
                    "stop_conditions": [
                        "Wi-Fi STA MAC differs from the expected hardware serial",
                        "Secure Boot or flash encryption is already enabled",
                        "security key or digest eFuse blocks are already occupied",
                        "UART download restrictions prevent the approved factory flow",
                        "any artifact, signature, partition, version, or hash check fails",
                    ],
                },
            }
            command_text = (
                "PROVISIONS CORES3 SECURE FACTORY COMMANDS — NOTHING HAS BEEN EXECUTED\n\n"
                f"Expected Wi-Fi STA MAC: {request.hardware_serial}\n"
                "1. Keep the device isolated from live yacht data and in ROM download mode.\n"
                "2. Run the read-only MAC check; STOP unless it matches exactly:\n"
                f"{read_mac_command}\n\n"
                "3. Run the read-only eFuse summary:\n"
                f"{efuse_summary_command}\n\n"
                "STOP if Secure Boot or flash encryption is already enabled, if any security\n"
                "key/digest block is occupied, or if UART download restrictions are set.\n\n"
                "4. Immediately before any destructive action, run the private verifier:\n"
                f"{preflash_verifier_command}\n\n"
                "STOP unless it prints PRE-FLASH VERIFICATION PASSED and its public-key\n"
                "fingerprint exactly matches the independently approved release record.\n"
                "The bundle is not an independent trust root.\n\n"
                "5. Only an authorized technician may run this destructive erase:\n"
                f"{erase_command}\n\n"
                "6. Without allowing any reset, power cycle, or first boot, write the complete\n"
                "approved artifact set in this single command:\n"
                f"{flash_command}\n\n"
                "7. STOP. --after no-reset is intentional. Keep the device in download mode\n"
                "or hold reset. First boot/eFuse enablement requires a separate approved\n"
                "security ceremony and sacrificial-device recovery validation.\n"
            )
            manifest_path = work_directory / manifest_name
            private_manifest_path = work_directory / private_manifest_name
            verifier_path = work_directory / verifier_name
            command_path = work_directory / command_name
            _write_private_text(
                manifest_path, json.dumps(manifest, indent=2, sort_keys=True) + "\n"
            )
            _write_private_text(
                private_manifest_path,
                json.dumps(private_checksum_manifest, indent=2, sort_keys=True) + "\n",
            )
            _write_private_text(verifier_path, verifier_text)
            _write_private_text(command_path, command_text)

            os.replace(generated_image_path, final_image_path)
            os.replace(generated_key_path, final_key_path)
            os.replace(manifest_path, final_manifest_path)
            os.replace(private_manifest_path, final_private_manifest_path)
            os.replace(verifier_path, final_verifier_path)
            os.replace(command_path, final_command_path)
            for artifact in (
                final_image_path,
                final_key_path,
                final_manifest_path,
                final_private_manifest_path,
                final_verifier_path,
                final_command_path,
            ):
                artifact.chmod(0o600)
            return manifest
    except Exception:
        shutil.rmtree(output_directory, ignore_errors=True)
        raise


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Prepare an offline, per-device Provisions CoreS3 secure factory bundle. "
            "The tool verifies and emits commands but never touches hardware."
        )
    )
    parser.add_argument(
        "--input", required=True, type=Path, help="absolute path to 0600 JSON"
    )
    parser.add_argument(
        "--output-dir",
        required=True,
        type=Path,
        help="new absolute directory outside this repo",
    )
    return parser


def main(arguments: list[str] | None = None) -> int:
    parser = _build_parser()
    options = parser.parse_args(arguments)
    try:
        request = load_private_input(options.input)
        manifest = generate_bundle(request, options.output_dir)
    except ProvisioningError as error:
        print(f"Provisioning failed: {error}", file=sys.stderr)
        return 2
    except Exception:  # noqa: BLE001 - never expose secret-bearing runtime state
        print(
            "Provisioning failed: unexpected internal error; no bundle retained",
            file=sys.stderr,
        )
        return 3
    print("Secure provisioning bundle prepared; no hardware or eFuses were changed.")
    print(f"Device UUID: {manifest['device']['uuid']}")
    print(f"Hardware serial: {manifest['device']['hardware_serial']}")
    print(f"Encrypted NVS SHA-256: {manifest['nvs']['image_sha256']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
