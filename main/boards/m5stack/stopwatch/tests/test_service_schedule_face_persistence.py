"""Exercise actual face/scheduler recovery; no simulated board or storage claims."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


class FacePersistenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.executable = Path(cls.directory.name) / "face_persistence"
        (Path(cls.directory.name) / "sdkconfig.h").write_text(
            "#pragma once\n#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1\n"
        )
        compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            raise RuntimeError("A host C++17 compiler is required")
        result = subprocess.run([
            compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
            "-I", str(HERE.parent), "-I", str(HERE.parents[3]),
            "-I", cls.directory.name,
            str(HERE.parent / "service_schedule.cc"),
            str(HERE.parent / "service_schedule_face.cc"),
            str(HERE.parent / "orbit_dial.cc"),
            str(HERE / "service_schedule_face_persistence_test.cc"),
            "-o", str(cls.executable),
        ], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)


CASES = (
    "running_waits_for_fresh_time one_of_six_ack_survives_restart all_acked_stay_silent "
    "old_ack_does_not_silence_new_revision retired_ack_survives_omission "
    "cue_ack_survives_service_and_occurrence_change corrupt_pending_rejected_atomically "
    "corrupt_cue_pending_rejected existing_model_cannot_be_replaced"
).split()


def make_test(case):
    def test(self):
        result = subprocess.run([str(self.executable), case], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
    return test


for case in CASES:
    setattr(FacePersistenceTests, "test_" + case, make_test(case))

if __name__ == "__main__":
    unittest.main()
