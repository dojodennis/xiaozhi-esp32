"""Actual compact codec + FaceModel + NVS adapter, with only platform NVS fault-injected."""

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
NVS_STUB = """#pragma once
#include <cstddef>
using nvs_handle_t = unsigned;
constexpr int ESP_OK=0, NVS_READONLY=0, NVS_READWRITE=1, ESP_ERR_NVS_NOT_FOUND=9;
constexpr int ESP_ERR_NVS_NOT_ENOUGH_SPACE=10;
int nvs_open(const char*, int, nvs_handle_t*);
void nvs_close(nvs_handle_t);
int nvs_get_blob(nvs_handle_t, const char*, void*, size_t*);
int nvs_set_blob(nvs_handle_t, const char*, const void*, size_t);
int nvs_commit(nvs_handle_t);
"""


class ServiceScheduleStorageTests(unittest.TestCase):
    def test_actual_storage_and_recovery(self):
        compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler, "host C++17 compiler is required")
        with tempfile.TemporaryDirectory(prefix="orbit-schedule-storage-") as temporary:
            directory = Path(temporary)
            (directory / "nvs.h").write_text(NVS_STUB)
            (directory / "sdkconfig.h").write_text("#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1\n")
            executable = directory / "storage_test"
            command = [
                compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
                "-I", str(directory), "-I", str(BOARD), "-I", str(MAIN),
                str(BOARD / "service_schedule.cc"),
                str(BOARD / "service_schedule_face.cc"),
                str(BOARD / "orbit_dial.cc"),
                str(BOARD / "service_schedule_storage.cc"),
                str(BOARD / "service_schedule_nvs_store.cc"),
                str(HERE / "service_schedule_storage_test.cc"),
                "-o", str(executable),
            ]
            built = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            environment = os.environ.copy()
            # Apple's ASan does not implement LeakSanitizer; keep address and
            # undefined-behavior checking enabled on both host platforms.
            leaks = 0 if sys.platform == "darwin" else 1
            environment["ASAN_OPTIONS"] = f"detect_leaks={leaks}:halt_on_error=1"
            environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
            run = subprocess.run([str(executable)], capture_output=True, text=True, env=environment)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("15 storage scenarios passed", run.stdout)
            print(run.stdout, end="")


if __name__ == "__main__":
    unittest.main()
