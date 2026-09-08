"""Compile and exercise the actual portable scheduler, using the shared wire fixture."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


def cpp_string(value):
    return json.dumps(value, ensure_ascii=False)


def fixture_header():
    data = json.loads((HERE / "orbit_service_schedule_v1.json").read_text())
    result = ['#include "service_schedule.h"', "using namespace orbit::service_schedule;"]
    for index, fixture in enumerate(data["snapshots"]):
        source = fixture["wire"]["service_schedule"]
        result.append(f"inline Snapshot Fixture{index}() {{ Snapshot s;")
        for key in ("assignment_id", "device_id"):
            result.append(f"s.scope.{key} = {cpp_string(source[key])};")
        for key in ("service_occurrence_id", "timezone"):
            result.append(f"s.{key} = {cpp_string(source[key])};")
        for key in ("version", "service_revision", "snapshot_revision", "service_at_ms", "server_now_ms"):
            result.append(f"s.{key} = {source[key]}LL;")
        for cue in source["cues"]:
            kind = "ServiceOffset" if cue["kind"] == "service_offset" else "Fixed"
            result.append("s.cues.push_back({%s, %s, %s, CueKind::%s, %sLL, %sLL});" % (
                cpp_string(cue["id"]), cue["revision"], cpp_string(cue["label"]), kind,
                cue["deadline_ms"], cue.get("offset_ms", 0)))
        for timer in source["timers"]:
            result.append("s.timers.push_back({%s, %s, %s, %sLL});" % (
                cpp_string(timer["id"]), timer["revision"], cpp_string(timer["label"]), timer["deadline_ms"]))
        result.append("return s; }")
    return "\n".join(result)


class ServiceScheduleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        build = Path(cls.directory.name)
        (build / "service_schedule_fixture.h").write_text(fixture_header())
        cls.executable = build / "service_schedule_test"
        compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            raise RuntimeError("A host C++17 compiler is required")
        subprocess.run([
            compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
            "-I", str(HERE.parent), "-I", str(build), str(HERE.parent / "service_schedule.cc"),
            str(HERE / "service_schedule_test.cc"), "-o", str(cls.executable),
        ], check=True, capture_output=True, text=True)


CASES = (
    "golden_scenario service_edit exact_acknowledgement occurrence_switch replay_no_rearm "
    "conflicting_snapshot conflicting_items lower_item_revision service_revision atomic_malformed "
    "scope_mismatch capacity retired_cannot_return retired_capacity restore_waits_for_fresh_clock "
    "restore_retired_history corrupt_restore monotonic_rollback server_clock_rollback "
    "delivery_delay_never_rewinds invalid_clock_and_overflow long_offline valid_utf8_and_limits malformed_fields"
).split()


def make_test(case):
    def test(self):
        result = subprocess.run([str(self.executable), case], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
    return test


for name in CASES:
    setattr(ServiceScheduleTests, "test_" + name, make_test(name))

if __name__ == "__main__":
    unittest.main()
