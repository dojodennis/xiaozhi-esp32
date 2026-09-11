import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD_DIR = ROOT / "main/boards/m5stack/stopwatch"


class ProvisionsButtonChordTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "host C++ compiler is unavailable")
    def test_chord_never_fires_single_button_actions(self):
        test_source = textwrap.dedent(
            r'''
            #include "button_chord.h"
            #include <cassert>

            using ProvisionsStopWatch::ButtonChord;
            using Edge = ButtonChord::Edge;
            constexpr int64_t W = ButtonChord::kWindowUs;

            int main() {
                {
                    // A plain Talk hold starts after the window and stops on release.
                    ButtonChord c;
                    assert(c.TalkDown(0) == Edge::kArmTalk);
                    assert(c.TalkWindowElapsed());
                    assert(!c.TalkWindowElapsed());  // Exactly one start.
                    assert(c.TalkUp());
                    assert(!c.SwallowBlueGesture());
                }
                {
                    // A tap shorter than the window never opens the microphone.
                    ButtonChord c;
                    assert(c.TalkDown(0) == Edge::kArmTalk);
                    assert(!c.TalkUp());
                    assert(!c.TalkWindowElapsed());
                }
                {
                    // Talk then blue inside the window: chord, no Talk, blue swallowed.
                    ButtonChord c;
                    assert(c.TalkDown(0) == Edge::kArmTalk);
                    assert(c.BlueDown(W) == Edge::kChord);
                    assert(!c.TalkWindowElapsed());
                    assert(c.SwallowBlueGesture());
                    c.BlueUp();
                    assert(c.SwallowBlueGesture());  // The blue click fires after release.
                    assert(!c.TalkUp());
                    // Next blue press on its own acts normally again.
                    assert(c.BlueDown(10 * W) == Edge::kNone);
                    assert(!c.SwallowBlueGesture());
                    c.BlueUp();
                }
                {
                    // Blue then Talk inside the window: chord either way round.
                    ButtonChord c;
                    assert(c.BlueDown(0) == Edge::kNone);
                    assert(c.TalkDown(W) == Edge::kChord);
                    assert(!c.TalkWindowElapsed());
                    assert(c.SwallowBlueGesture());
                    assert(!c.TalkUp());
                    c.BlueUp();
                    // A new Talk hold afterwards is ordinary.
                    assert(c.TalkDown(100 * W) == Edge::kArmTalk);
                    assert(c.TalkWindowElapsed());
                    assert(c.TalkUp());
                }
                {
                    // Outside the window both actions stay ordinary: a Talk hold
                    // that already started is not cancelled by a later blue press.
                    ButtonChord c;
                    assert(c.TalkDown(0) == Edge::kArmTalk);
                    assert(c.TalkWindowElapsed());
                    assert(c.BlueDown(W + 1) == Edge::kNone);
                    assert(!c.SwallowBlueGesture());
                    c.BlueUp();
                    assert(c.TalkUp());
                    assert(c.BlueDown(0) == Edge::kNone);
                    assert(c.TalkDown(W + 1) == Edge::kArmTalk);
                    assert(!c.SwallowBlueGesture());
                    c.BlueUp();
                    assert(c.TalkWindowElapsed());
                    assert(c.TalkUp());
                }
                {
                    // Re-pressing one button while the other is still held from a
                    // chord is not a second chord and not a single action.
                    ButtonChord c;
                    assert(c.TalkDown(0) == Edge::kArmTalk);
                    assert(c.BlueDown(10) == Edge::kChord);
                    assert(!c.TalkUp());
                    assert(c.TalkDown(20) == Edge::kNone);
                    assert(!c.TalkWindowElapsed());
                    assert(!c.TalkUp());
                    c.BlueUp();
                    assert(c.TalkDown(100 * W) == Edge::kArmTalk);
                }
                return 0;
            }
            '''
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            source = temporary / "button_chord_test.cc"
            executable = temporary / "button_chord_test"
            source.write_text(test_source, encoding="utf-8")
            sanitize = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            result = subprocess.run(
                [shutil.which("c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror", *sanitize,
                 "-I", str(BOARD_DIR), str(source), "-o", str(executable)],
                capture_output=True, text=True, cwd=ROOT,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            run = subprocess.run(
                [str(executable)], capture_output=True, text=True, cwd=ROOT, timeout=15,
                env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0" if sys.platform == "darwin" else "detect_leaks=1"},
            )
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_board_wires_chord_to_timer_face_and_documents_gestures(self):
        board = (BOARD_DIR / "m5stack_stopwatch.cc").read_text(encoding="utf-8")
        readme = (BOARD_DIR / "README.md").read_text(encoding="utf-8")
        self.assertIn('#include "button_chord.h"', board)
        self.assertIn("button2_.OnPressDown", board)
        self.assertIn("button2_.OnPressUp", board)
        self.assertIn("ButtonChord::kWindowUs", board)
        self.assertIn("ToggleTimerFace()", board)
        self.assertIn("kTimerFaceIdleUs = 30LL * 1000 * 1000", board)
        # Every blue gesture checks the chord before silencing, dictation, retry or volume.
        buttons = board.split("void InitializeButtons()", 1)[1].split("button1_.OnPressDown", 1)[1]
        buttons = buttons.split("#else\n        // Button1", 1)[0]
        for gesture in ("button2_.OnClick", "button2_.OnDoubleClick", "button2_.OnLongPress"):
            body = buttons.split(gesture, 1)[1].split("});", 1)[0]
            self.assertIn("BlueGestureInChord()", body, gesture)
        self.assertIn("Two-button chord", readme)
        self.assertIn("| Talk + blue together", readme)


if __name__ == "__main__":
    unittest.main()
