"""V2 actual decoder/model/face/compact codec, with synthetic shared wire fixtures.

The v1 OSS1 golden was captured from committed firmware 69afde7, applying the
unchanged first v1 wire fixture at monotonic zero. It must never be regenerated
from candidate source to make a changed encoder pass. No live authentication proof.
"""

import copy
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from test_service_schedule_wire import cjson_source, encoded

HERE = Path(__file__).resolve().parent
BOARD = HERE.parent
FIXTURE = json.loads((HERE / "orbit_service_schedule_v2.json").read_text())
ABSENT = FIXTURE["snapshots"][1]["wire"]
CLOCK = FIXTURE["clock"]["response"]


def fixture_header():
    def quoted(value):
        return (
            'R"fixture('
            + json.dumps(value, ensure_ascii=False, separators=(",", ":"))
            + ')fixture"'
        )

    legacy = json.loads((HERE / "orbit_service_schedule_v1.json").read_text())[
        "snapshots"
    ][0]["wire"]
    old_bytes = bytes.fromhex((HERE / "orbit_service_schedule_v1_oss1.hex").read_text())
    return "\n".join(
        [
            "#include <cstdint>",
            f'constexpr char kSession[] = "{FIXTURE["websocket_session_id"]}";',
            "constexpr const char* kFrames[] = {"
            + ",".join(quoted(x["wire"]) for x in FIXTURE["snapshots"])
            + "};",
            "constexpr const char* kOccurrences[] = {"
            + ",".join(
                json.dumps(x["scope"]["service_occurrence_id"] or "")
                for x in FIXTURE["snapshots"]
            )
            + "};",
            "constexpr const char* kClockRequest = "
            + quoted(FIXTURE["clock"]["request"])
            + ";",
            "constexpr const char* kClockResponse = " + quoted(CLOCK) + ";",
            "constexpr const char* kV1Frame = " + quoted(legacy) + ";",
            "constexpr uint8_t kV1Bytes[] = {"
            + ",".join(str(x) for x in old_bytes)
            + "};",
        ]
    )


class ServiceScheduleV2Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="orbit-v2-")
        cls.addClassCleanup(cls.temporary.cleanup)
        build = Path(cls.temporary.name)
        (build / "service_schedule_v2_fixture.h").write_text(fixture_header())
        (build / "sdkconfig.h").write_text(
            "#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1\n"
        )
        source = cjson_source()
        cls.executable = build / "v2"
        cc = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc")
        cxx = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
        sanitizer = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        commands = [
            [
                cc,
                "-std=c11",
                *sanitizer,
                "-c",
                str(source / "cJSON.c"),
                "-o",
                str(build / "cjson.o"),
            ],
            [
                cxx,
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pedantic",
                *sanitizer,
                "-I",
                str(build),
                "-I",
                str(BOARD),
                "-I",
                str(BOARD.parents[2]),
                "-I",
                str(source),
                *[
                    str(BOARD / (x + ".cc"))
                    for x in (
                        "service_schedule",
                        "service_schedule_face",
                        "service_schedule_storage",
                        "service_schedule_wire",
                        "orbit_dial",
                    )
                ],
                str(HERE / "service_schedule_v2_test.cc"),
                str(build / "cjson.o"),
                "-o",
                str(cls.executable),
            ],
        ]
        for command in commands:
            result = subprocess.run(command, capture_output=True, text=True, timeout=90)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
        cls.environment = os.environ.copy()
        cls.environment["ASAN_OPTIONS"] = (
            f"detect_leaks={0 if sys.platform == 'darwin' else 1}:halt_on_error=1"
        )
        cls.environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"

    def invoke(self, mode=None, frame=None, expected=0):
        result = subprocess.run(
            [str(self.executable), *([mode] if mode else [])],
            input=(frame if isinstance(frame, bytes) else encoded(frame))
            if mode
            else None,
            capture_output=True,
            env=self.environment,
            timeout=30,
        )
        self.assertEqual(
            result.returncode,
            0,
            (result.stdout + result.stderr).decode(errors="replace"),
        )
        if mode:
            self.assertEqual(int(result.stdout), expected)
        else:
            self.assertIn(b"11 v2 model/codec/clock scenarios passed", result.stdout)
            print(result.stdout.decode(), end="")

    def test_actual_model_codec_clock_scenarios(self):
        self.invoke()

    def test_exact_v2_absence(self):
        self.invoke("absent", ABSENT)
        self.invoke("present", FIXTURE["snapshots"][0]["wire"])
        for key, values in {
            "service_occurrence_id": [
                "",
                "00000000-0000-0000-0000-000000000000",
                False,
            ],
            "service_revision": [1, -1, None],
            "service_at_ms": [1, None],
            "timezone": [None, "Europe/Brussels"],
            "cues": [FIXTURE["snapshots"][0]["wire"]["service_schedule"]["cues"]],
        }.items():
            for value in values:
                frame = copy.deepcopy(ABSENT)
                frame["service_schedule"][key] = value
                self.invoke("absent", frame, 1)
        frame = copy.deepcopy(ABSENT)
        frame["service_schedule"]["server_now_ms"] = 1788883300000
        self.invoke("absent", frame, 1)
        self.invoke("present", ABSENT, 5)
        self.invoke("absent", FIXTURE["snapshots"][0]["wire"], 5)

    def test_clock_context(self):
        self.invoke("clock", CLOCK)
        self.invoke("wrong_session", CLOCK, 3)
        self.invoke("wrong_scope", CLOCK, 4)
        self.invoke("clock", FIXTURE["clock"]["request"], 1)
        self.invoke("clock", ABSENT, 1)

    def test_clock_closed_keys(self):
        for container in (None, "service_schedule_clock"):
            mapping = CLOCK if container is None else CLOCK[container]
            for key in [*mapping, "extra"]:
                frame = copy.deepcopy(CLOCK)
                target = frame if container is None else frame[container]
                if key == "extra":
                    target[key] = 1
                else:
                    del target[key]
                self.invoke("clock", frame, 1)
            for key, value in mapping.items():
                token = encoded({key: value})[1:-1]
                self.invoke(
                    "clock", encoded(CLOCK).replace(token, token + b"," + token, 1), 1
                )

    def test_clock_integer_forms_and_limits(self):
        for raw in (b"1.0", b"1e0", b"01", b"+1", b"-1", b"true", b"null", b'"1"'):
            self.invoke(
                "clock", encoded(CLOCK).replace(b'"version":1', b'"version":' + raw), 1
            )
        for key, values in {
            "snapshot_revision": [0, 9007199254740992],
            "server_now_ms": [0, -1, 253402300800000],
        }.items():
            for value in values:
                frame = copy.deepcopy(CLOCK)
                frame["service_schedule_clock"][key] = value
                self.invoke("clock", frame, 1)
        frame = copy.deepcopy(CLOCK)
        frame["service_schedule_clock"]["snapshot_revision"] = 9007199254740991
        frame["service_schedule_clock"]["server_now_ms"] = 253402300799999
        self.invoke("clock", frame)

    def test_clock_raw_limits_and_nul(self):
        raw = encoded(CLOCK)
        self.invoke("clock", raw + b" " * (32768 - len(raw)))
        self.invoke("clock", raw + b" " * (32769 - len(raw)), 1)
        for suffix in (b"{}", b"null", b"\x00"):
            self.invoke("clock", raw + suffix, 1)
        self.invoke("clock", raw.replace(b'"version"', b'"version\\u0000hidden"'), 1)
        self.invoke(
            "clock", raw.replace(b'"version":1', b'"version":1,"ver\\u0073ion":1'), 1
        )
        self.invoke("clock", b'{"a":' + b"[" * 17 + b"0" + b"]" * 17 + b"}", 1)
        for key in ("request_id", "assignment_id", "device_id"):
            for value in (
                "",
                "00000000-0000-0000-0000-000000000000",
                "AAAAAAAA-0000-4000-8000-000000000001",
            ):
                frame = copy.deepcopy(CLOCK)
                frame["service_schedule_clock"][key] = value
                self.invoke("clock", frame, 1)


if __name__ == "__main__":
    unittest.main()
