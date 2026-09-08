"""Real asynchronous worker, scheduler, face and codec; only storage is injected."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
BOARD = HERE.parent
MAIN = BOARD.parents[2]


class ServiceScheduleWorkerTests(unittest.TestCase):
    def test_actual_threaded_worker(self):
        compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler, "host C++17 compiler is required")
        with tempfile.TemporaryDirectory(prefix="orbit-schedule-worker-") as temporary:
            directory = Path(temporary)
            (directory / "sdkconfig.h").write_text("#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1\n")
            executable = directory / "worker_test"
            command = [
                compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
                "-I", str(directory), "-I", str(BOARD), "-I", str(MAIN),
                str(BOARD / "service_schedule.cc"),
                str(BOARD / "service_schedule_face.cc"),
                str(BOARD / "orbit_dial.cc"),
                str(BOARD / "service_schedule_storage.cc"),
                str(BOARD / "service_schedule_worker.cc"),
                str(HERE / "service_schedule_worker_test.cc"),
                "-o", str(executable),
            ]
            built = subprocess.run(command, capture_output=True, text=True, timeout=90)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            environment = os.environ.copy()
            leaks = 0 if sys.platform == "darwin" else 1
            environment["ASAN_OPTIONS"] = f"detect_leaks={leaks}:halt_on_error=1"
            environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
            run = subprocess.run([str(executable)], capture_output=True, text=True,
                                 env=environment, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("16 threaded worker scenarios passed", run.stdout)
            print(run.stdout, end="")


if __name__ == "__main__":
    unittest.main()
