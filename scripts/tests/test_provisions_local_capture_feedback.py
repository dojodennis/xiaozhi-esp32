"""Host checks for the local capture acknowledgement timing gate."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class LocalCaptureFeedbackTests(unittest.TestCase):
    def test_old_expiry_cannot_end_a_new_haptic_pulse(self):
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler)
        source = r'''
#include <cassert>
#include "provisions_local_capture_feedback.h"
int main() {
    provisions::LocalCapturePulse pulse;
    const auto first = pulse.Arm(1'000'000, 90);
    const auto second = pulse.Arm(1'050'000, 90);
    assert(first != second);
    assert(!pulse.IsExpired(first, first));
    assert(!pulse.IsExpired(second, second - 1));
    assert(pulse.IsExpired(second, second));
    assert(pulse.Clear(second));
    assert(pulse.deadline() == 0);
    assert(!pulse.Clear(first));
}
'''
        with tempfile.TemporaryDirectory(prefix="orbit-capture-pulse-") as folder:
            path = Path(folder)
            (path / "test.cc").write_text(source)
            result = subprocess.run(
                [compiler, "-std=c++17", "-pthread", "-I", str(ROOT / "main"),
                 str(path / "test.cc"), "-o", str(path / "test")],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            run = subprocess.run([str(path / "test")], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == "__main__":
    unittest.main()
