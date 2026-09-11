import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class ProvisionsCaptureDiagnosticsTests(unittest.TestCase):
    def test_recorder_logs_one_info_line_per_capture_without_changing_gain(self):
        recorder = (ROOT / "main/provisions_voice_recorder.cc").read_text(encoding="utf-8")
        board = (ROOT / "main/boards/m5stack/stopwatch/m5stack_stopwatch.cc").read_text(encoding="utf-8")
        save = recorder.split("void VoiceRecorder::Save(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("DescribeCapture(work.press, ok, work.samples, work.levels, bytes", save)
        self.assertIn('ESP_LOGI("VoiceRecorder", "%s", line.c_str())', save)
        self.assertIn("NoteTalkPressDown(esp_timer_get_time())", board)
        self.assertNotIn("SetInputGain", recorder)
        self.assertNotIn("set_in_gain", recorder)

    @unittest.skipUnless(shutil.which("c++"), "host C++ compiler is unavailable")
    def test_levels_and_description(self):
        test_source = textwrap.dedent(
            r'''
            #include "provisions_voice_recording.h"
            #include <cassert>
            #include <string>
            #include <vector>
            using namespace provisions;

            int main() {
                std::vector<int16_t> first(VoiceRecording::kMaxSamples), second(VoiceRecording::kMaxSamples);
                VoiceRecording recording(first.data(), second.data(), first.size());
                VoiceContext context; context.conversation_id[0] = 1;
                assert(recording.Begin(7, context, 0));
                int16_t silence[160] = {};
                int16_t voice[160];
                for (int i = 0; i < 160; ++i) voice[i] = static_cast<int16_t>(i % 2 ? -1000 : 1000);
                // 40 ms of zeros from the codec, then speech.
                for (int i = 0; i < 4; ++i) assert(recording.Append(7, silence, 160, 1));
                assert(recording.Append(7, voice, 160, 1));
                int16_t extreme[2] = {-32768, 5};
                assert(recording.Append(7, extreme, 2, 1));
                recording.Release(7);
                VoiceRecording::Work work;
                assert(recording.Take(work));
                assert(work.samples == 802);
                assert(work.levels.first_nonzero == 640);
                assert(work.levels.peak == 32768 - 1 || work.levels.peak == 32768);
                const auto line = DescribeCapture(7, true, work.samples, work.levels, 321, 25);
                assert(line.find("press=7 saved=1") != std::string::npos);
                assert(line.find("duration_ms=50 samples=802 opus_bytes=321 rate_hz=16000") != std::string::npos);
                assert(line.find("press_to_first_chunk_ms=25 press_to_first_nonzero_ms=65 leading_zero_ms=40") != std::string::npos);
                assert(line.find("rms=") != std::string::npos);
                recording.Finish(work);

                // A capture that never left zero reports it plainly.
                assert(recording.Begin(8, context, 0));
                for (int i = 0; i < 3; ++i) assert(recording.Append(8, silence, 160, 1));
                recording.Release(8);
                assert(recording.Take(work));
                assert(work.levels.peak == 0 && work.levels.first_nonzero == SIZE_MAX);
                const auto quiet = DescribeCapture(8, true, work.samples, work.levels, 9, -1);
                assert(quiet.find("peak=0 rms=0 press_to_first_chunk_ms=-1 press_to_first_nonzero_ms=-1 leading_zero_ms=-1") != std::string::npos);
                recording.Finish(work);
                // Levels reset per capture.
                assert(recording.Begin(9, context, 0));
                assert(recording.Append(9, voice, 160, 1));
                recording.Release(9);
                assert(recording.Take(work) && work.levels.first_nonzero == 0 && work.levels.peak == 1000);
                return 0;
            }
            '''
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            source = temporary / "capture_diagnostics_test.cc"
            executable = temporary / "capture_diagnostics_test"
            source.write_text(test_source, encoding="utf-8")
            sanitize = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            result = subprocess.run(
                [shutil.which("c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread", *sanitize,
                 "-I", str(ROOT / "main"), str(source), str(ROOT / "main/provisions_voice_recording.cc"),
                 "-o", str(executable)],
                capture_output=True, text=True, cwd=ROOT,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            run = subprocess.run(
                [str(executable)], capture_output=True, text=True, cwd=ROOT, timeout=15,
                env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0" if sys.platform == "darwin" else "detect_leaks=1"},
            )
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
