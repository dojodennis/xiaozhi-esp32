"""Actual portable alarm sequencer; hooks model admission and physical drain separately."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
CASES = """idle_single_admission complete_drain_and_gap cancel_waits_for_tail
cancelled_clip_stays_stopped foreign_owner_at_start foreign_owner_during_gap
failed_admission_explicit_retry error_wins_over_drain error_counter_wrap
playback_and_tail_timeout stale_time_after_cancel pulse_pattern_50ms
resumed_poll_drops_stale_high missing_hooks""".split()


class AlarmOutputTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="orbit-alarm-output-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.executable = Path(cls.directory.name) / "alarm_output"
        compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            raise RuntimeError("A host C++17 compiler is required")
        command = [
            compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
            "-I", str(HERE.parent),
            str(HERE.parent / "service_schedule_alarm_output.cc"),
            str(HERE / "service_schedule_alarm_output_test.cc"),
            "-o", str(cls.executable),
        ]
        compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
        if compiled.returncode:
            raise RuntimeError(compiled.stdout + compiled.stderr)


def make_test(case):
    def test(self):
        environment = os.environ.copy()
        leaks = 0 if sys.platform == "darwin" else 1
        environment["ASAN_OPTIONS"] = f"detect_leaks={leaks}:halt_on_error=1"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        result = subprocess.run(
            [str(self.executable), case], capture_output=True, text=True,
            env=environment, timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
    return test


for case in CASES:
    setattr(AlarmOutputTests, "test_" + case, make_test(case))

if __name__ == "__main__":
    unittest.main()
