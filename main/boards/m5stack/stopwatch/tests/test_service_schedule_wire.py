"""Strict raw-frame decoder behavioral tests against the actual cJSON-backed C++ source.

No network, board or auth evidence. Uses an existing managed cJSON source from this
repository or another checkout; CJSON_SOURCE_DIR can name an explicit installation.
"""

import copy
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_service_schedule import fixture_header

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[4]
FIXTURE = json.loads((HERE / "orbit_service_schedule_v1.json").read_text())
BASE = FIXTURE["snapshots"][0]["wire"]


def encoded(frame):
    return json.dumps(frame, ensure_ascii=False, separators=(",", ":")).encode()


def cjson_source():
    candidates = []
    if os.environ.get("CJSON_SOURCE_DIR"):
        candidates.append(Path(os.environ["CJSON_SOURCE_DIR"]))
    paths = subprocess.run(["git", "worktree", "list", "--porcelain"], cwd=HERE,
                           capture_output=True, text=True, check=True).stdout.splitlines()
    for line in paths:
        if line.startswith("worktree "):
            candidates.append(Path(line[9:]) / "managed_components/espressif__cjson/cJSON")
    for candidate in candidates:
        if (candidate / "cJSON.c").is_file() and (candidate / "cJSON.h").is_file():
            return candidate
    raise RuntimeError("Existing managed cJSON source required; set CJSON_SOURCE_DIR")


class ServiceScheduleWireTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        build = Path(cls.directory.name)
        (build / "service_schedule_fixture.h").write_text(fixture_header())
        cls.executable = build / "service_schedule_wire_test"
        source = cjson_source()
        cxx = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
        cc = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc")
        if not cxx or not cc:
            raise RuntimeError("Host C and C++17 compilers are required")
        sanitizer = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        commands = [
            [cc, "-std=c11", *sanitizer, "-c", str(source / "cJSON.c"), "-o", str(build / "cJSON.o")],
            [cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", *sanitizer,
             "-I", str(HERE.parent), "-I", str(build), "-I", str(source),
             str(HERE.parent / "service_schedule.cc"), str(HERE.parent / "service_schedule_wire.cc"),
             str(HERE / "service_schedule_wire_test.cc"), str(build / "cJSON.o"),
             "-o", str(cls.executable)],
        ]
        for command in commands:
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)

    def decode(self, frame=BASE, mode="normal", result="accepted"):
        raw = frame if isinstance(frame, bytes) else encoded(frame)
        process = subprocess.run([str(self.executable), mode], input=raw, capture_output=True)
        self.assertEqual(process.returncode, 0, (process.stdout + process.stderr).decode(errors="replace"))
        output = process.stdout.decode().split()
        self.assertEqual(output[0], result)
        return output

    def test_shared_fixtures(self):
        for fixture in FIXTURE["snapshots"]:
            wire = fixture["wire"]
            output = self.decode(wire)
            self.assertEqual(list(map(int, output[1:])), [
                wire["service_schedule"]["snapshot_revision"], 2, 3,
                wire["service_schedule"]["service_at_ms"],
                fixture["expected_deadlines"]["00000000-0000-4000-8000-000000000010"],
                fixture["expected_deadlines"]["00000000-0000-4000-8000-000000000020"],
            ])

    def test_whitespace_complete_parse(self):
        self.decode(b" \n\t" + encoded(BASE) + b" \r\n")
        for suffix in (b"{}", b"true", b"garbage", b"\v", b"\x00"):
            with self.subTest(suffix=suffix):
                self.decode(encoded(BASE) + suffix, result="malformed")

    def test_raw_byte_budget(self):
        raw = encoded(BASE)
        self.decode(raw + b" " * (32768 - len(raw)))
        self.decode(raw + b" " * (32769 - len(raw)), result="malformed")

    def test_depth_budget(self):
        self.decode(b'{"nested":' + b"[" * 17 + b"0" + b"]" * 17 + b"}", result="malformed")

    def test_exact_envelope(self):
        for key in BASE:
            frame = copy.deepcopy(BASE)
            del frame[key]
            self.decode(frame, result="malformed")
        for key, value in (("extra", 1), ("type", "timer"), ("state", "timer_snapshot")):
            frame = copy.deepcopy(BASE)
            frame[key] = value
            self.decode(frame, result="malformed")

    def test_duplicate_envelope_keys(self):
        raw = encoded(BASE)
        for key in BASE:
            duplicate = encoded({key: BASE[key]})[1:-1]
            self.decode(raw[:-1] + b"," + duplicate + b"}", result="malformed")

    def test_payload_keys(self):
        for key in BASE["service_schedule"]:
            frame = copy.deepcopy(BASE)
            del frame["service_schedule"][key]
            self.decode(frame, result="malformed")
        frame = copy.deepcopy(BASE)
        frame["service_schedule"]["extra"] = True
        self.decode(frame, result="malformed")

    def test_duplicate_payload_keys(self):
        raw = encoded(BASE)
        for key in ("version", "snapshot_revision", "service_occurrence_id", "timers"):
            token = encoded({key: BASE["service_schedule"][key]})[1:-1]
            self.decode(raw.replace(token, token + b"," + token, 1), result="malformed")

    def test_duplicate_escaped_key(self):
        self.decode(encoded(BASE).replace(b'"version":1', b'"version":1,"ver\\u0073ion":1'),
                    result="malformed")

    def test_duplicate_item_keys(self):
        for token in (b'"label":"Setup"', b'"kind":"service_offset"', b'"label":"Rice"'):
            self.decode(encoded(BASE).replace(token, token + b"," + token, 1), result="malformed")

    def test_item_schema(self):
        for collection in ("cues", "timers"):
            for key in BASE["service_schedule"][collection][0]:
                frame = copy.deepcopy(BASE)
                del frame["service_schedule"][collection][0][key]
                self.decode(frame, result="malformed")
        for index in (0, 1):
            frame = copy.deepcopy(BASE)
            frame["service_schedule"]["cues"][index]["extra"] = 1
            self.decode(frame, result="malformed")
        frame = copy.deepcopy(BASE)
        frame["service_schedule"]["cues"][1]["offset_ms"] = 0
        self.decode(frame, result="malformed")

    def test_integer_lexical_forms(self):
        for value in (b"10.0", b"1e1", b"1E+1", b"+10", b"010", b".10", b"NaN", b"Infinity",
                      b"true", b"false", b'"10"', b"null"):
            self.decode(encoded(BASE).replace(b'"snapshot_revision":10', b'"snapshot_revision":' + value),
                        result="malformed")

    def test_integer_bounds(self):
        for key, values in {
            "version": [0, 2, -1], "snapshot_revision": [0, -1, 9007199254740992],
            "service_revision": [0, 9007199254740992],
            "service_at_ms": [0, 253402300800000], "server_now_ms": [0, -1, 253402300800000],
        }.items():
            for value in values:
                frame = copy.deepcopy(BASE)
                frame["service_schedule"][key] = value
                self.decode(frame, result="malformed")
        frame = copy.deepcopy(BASE)
        frame["service_schedule"]["snapshot_revision"] = 9007199254740991
        self.decode(frame)

    def test_offset_integer_forms_and_relationship(self):
        for value in (b"-3600000.0", b"-36e5", b"-03600000", b"true"):
            self.decode(encoded(BASE).replace(b'"offset_ms":-3600000', b'"offset_ms":' + value),
                        result="malformed")
        for offset in (-604800001, 604800001, -3599999):
            frame = copy.deepcopy(BASE)
            frame["service_schedule"]["cues"][0]["offset_ms"] = offset
            self.decode(frame, result="malformed")

    def test_enrolled_context(self):
        for mode, result in (("wrong_session", "session_mismatch"), ("empty_context", "session_mismatch"),
                             ("wrong_assignment", "scope_mismatch"), ("wrong_device", "scope_mismatch"),
                             ("wrong_occurrence", "occurrence_mismatch")):
            self.decode(mode=mode, result=result)

    def test_missing_validators_fail_closed(self):
        for mode in ("missing_label", "missing_zone"):
            self.decode(mode=mode, result="missing_validators")

    def test_policy_rejection(self):
        for mode in ("reject_label", "reject_zone"):
            self.decode(mode=mode, result="malformed")
        frame = copy.deepcopy(BASE)
        frame["service_schedule"]["cues"][0]["label"] = "Soft\u00adhyphen"
        self.decode(frame, result="malformed")

    def test_uuid_format(self):
        for value in ("00000000-0000-0000-0000-000000000000", "AAAAAAAA-0000-4000-8000-000000000001",
                      "bad", 4, None):
            frame = copy.deepcopy(BASE)
            frame["service_schedule"]["timers"][0]["id"] = value
            self.decode(frame, result="malformed")

    def test_duplicate_item_ids(self):
        for collection in ("cues", "timers"):
            frame = copy.deepcopy(BASE)
            frame["service_schedule"][collection][0]["id"] = frame["service_schedule"]["timers"][1]["id"]
            self.decode(frame, result="malformed")

    def test_empty_and_maximum_arrays(self):
        frame = copy.deepcopy(BASE)
        frame["service_schedule"]["cues"] = []
        frame["service_schedule"]["timers"] = []
        self.decode(frame)
        for i in range(64):
            frame["service_schedule"]["timers"].append({
                "id": f"00000000-0000-4000-8000-{i + 100:012x}", "revision": 1, "label": "Timer",
                "deadline_ms": 1788883260000})
        self.decode(frame)
        frame["service_schedule"]["cues"] = BASE["service_schedule"]["cues"][:1]
        self.decode(frame, result="malformed")

    def test_array_and_item_types(self):
        for value in ({}, "", 1, None, [None]):
            for collection in ("cues", "timers"):
                frame = copy.deepcopy(BASE)
                frame["service_schedule"][collection] = value
                self.decode(frame, result="malformed")

    def test_utf8_scalar_bounds(self):
        frame = copy.deepcopy(BASE)
        frame["service_schedule"]["timers"][0]["label"] = "🍳" * 80
        self.decode(frame)
        frame["service_schedule"]["timers"][0]["label"] += "é"
        self.decode(frame, result="malformed")
        for value in (b"\xc0\x80", b"\xed\xa0\x80", b"\xf4\x90\x80\x80", b"\xf0\x90"):
            self.decode(encoded(BASE).replace(b'"Rice"', b'"' + value + b'"'), result="malformed")

    def test_label_controls_and_blank(self):
        for value in ("", "   ", "\n", "\x7f", "\x80", "\t", "\u009f", " Rice", "Rice ",
                      "\u00a0Rice", "Rice\u00a0"):
            frame = copy.deepcopy(BASE)
            frame["service_schedule"]["timers"][0]["label"] = value
            self.decode(frame, result="malformed")

    def test_escaped_nul(self):
        for token, replacement in ((b'"Rice"', b'"Rice\\u0000Hidden"'),
                                   (b'"version"', b'"version\\u0000hidden"'),
                                   (b'"provisions"', b'"provisions\\u0000hidden"')):
            self.decode(encoded(BASE).replace(token, replacement), result="malformed")
        # Escaped backslash followed by literal u0000 is ordinary printable text.
        self.decode(encoded(BASE).replace(b'"Rice"', b'"Rice\\\\u0000"'))

    def test_zone_membership(self):
        for zone in ("", "A" * 65, "Mars/Kitchen", "../etc", "Europe/Brussels\n"):
            frame = copy.deepcopy(BASE)
            frame["service_schedule"]["timezone"] = zone
            self.decode(frame, result="malformed")

    def test_incomplete_invalid_and_bom(self):
        for raw in (b"", b"{}", b"[]", encoded(BASE)[:-1], b"\xef\xbb\xbf" + encoded(BASE),
                    encoded(BASE).replace(b'"Rice"', b'"\\q"'), encoded(BASE).replace(b'"Rice"', b'"\\uZZZZ"')):
            self.decode(raw, result="malformed")


if __name__ == "__main__":
    unittest.main()
