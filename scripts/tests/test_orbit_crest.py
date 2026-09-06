import hashlib
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/m5stack/stopwatch"


class OrbitCrestTests(unittest.TestCase):
    def test_original_svg_payload_is_unchanged(self):
        payload = (BOARD / "provisionscrestexact.svg").read_bytes().removesuffix(b"\n")
        self.assertEqual(hashlib.sha256(payload).hexdigest(),
                         "92c8326411d6b07a87b9b18aadb6cd1f137886e2ec88b553970f2e9bd8bb20bd")

    def test_motion_and_audio_contract_compile_and_run(self):
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler)
        program = r'''
#include "crest_motion.h"
#include "crest_audio.h"
#include <cassert>
#include <cstring>
#include <limits>
using namespace OrbitCrest;
int main() {
    Frame idle;
    assert(idle.band_opacity == 255 && idle.star_opacity == 255);
    for (auto alpha : idle.opacity) assert(alpha == 0);
    for (State state : {State::Listening, State::Speaking, State::Thinking}) {
        for (bool reduced : {false, true}) {
            for (uint32_t t = 0; t < 10000; t += 7) {
                for (float level : {0.0F, 0.1F, 0.5F, 1.0F}) {
                    Frame frame = Rings(state, t, level, reduced);
                    for (int radius : frame.radii) assert(radius > 0 && radius + 2 <= kSafeRadius);
                    assert(frame.radii == Frame().radii);
                    assert(frame.radii[0] < frame.radii[1] && frame.radii[1] < frame.radii[2]);
                    for (uint32_t age : {0U, 90U, 180U, 359U, 360U}) {
                        Frame transition = Transition(idle, frame, age, reduced);
                        bool visible = transition.band_opacity || transition.star_opacity;
                        for (auto alpha : transition.opacity) visible |= alpha > 0;
                        assert(visible);
                    }
                }
            }
        }
    }
    auto listening = Rings(State::Listening, 400, 1, false);
    auto speaking = Rings(State::Speaking, 400, 1, false);
    assert(listening.radii == speaking.radii);
    assert(listening.opacity != speaking.opacity);
    auto quiet_listening = Rings(State::Listening, 400, 0, false);
    auto quiet_speaking = Rings(State::Speaking, 400, 0, false);
    for (int index = 0; index < 3; ++index) {
        assert(listening.opacity[index] > quiet_listening.opacity[index]);
        assert(speaking.opacity[index] > quiet_speaking.opacity[index]);
    }
    // Adjacent 30 Hz frames and the old 0.9-second wrap boundary stay
    // continuous. Geometry is fixed and alpha cannot jump visibly.
    for (uint32_t t : {0U, 330U, 899U, 900U, 2199U, 2200U, 4999U}) {
        auto current = Rings(State::Speaking, t, 0.5F, false);
        auto next = Rings(State::Speaking, t + 33, 0.5F, false);
        assert(current.radii == next.radii);
        for (int index = 0; index < 3; ++index)
            assert(std::abs(static_cast<int>(current.opacity[index]) -
                            static_cast<int>(next.opacity[index])) <= 8);
    }
    auto start = Transition(idle, listening, 0, false);
    assert(start.star_opacity == 255 && start.band_opacity == 255);
    auto halfway = Transition(idle, listening, 180, false);
    assert(halfway.star_opacity < 255 && halfway.star_opacity > kActiveStarOpacity);
    assert(halfway.band_opacity > 0 && halfway.band_opacity < 255);
    assert(halfway.color != idle.color && halfway.color != listening.color);
    auto back = Transition(listening, idle, 180, false);
    assert(back.star_opacity < 255 && back.star_opacity > kActiveStarOpacity);
    assert(back.band_opacity > 0 && back.band_opacity < 255);
    auto interrupted = Transition(halfway, idle, 0, false);
    assert(interrupted.band_opacity == halfway.band_opacity);
    assert(interrupted.star_opacity == halfway.star_opacity);
    assert(Transition(idle, listening, kTransitionMs, false).color == listening.color);
    assert(Rings(State::Listening, 0, 0, true).radii == Rings(State::Listening, 999, 1, true).radii);
    assert(AudioLevel(99, 0) == 0 && AudioLevel(32768, 121) == 0);
    assert(AudioLevel(32768, 1) == 1);
    int16_t pcm[] = {-32768, 32767, 0, -1000};
    int16_t original[4]; std::memcpy(original, pcm, sizeof pcm);
    AudioMeter meter;
    meter.Observe(pcm, 4, 42);
    assert(std::memcmp(original, pcm, sizeof pcm) == 0);
    assert(meter.mean_absolute == (32768U + 32767U + 1000U) / 4);
    assert(meter.sampled_ms == 42);
    meter.Observe(nullptr, 0, 43);
    assert(meter.mean_absolute == 0);
    assert(std::strcmp(ResultCaption("No match"), "No match") == 0);
    assert(std::strstr(ResultCaption("Added"), "Not sent"));
    for (auto input : {"Saved", "HTTP 403", "token expired", "a long technical paragraph", ""})
        assert(ResultCaption(input)[0] == '\0');
    assert(ResultCaption(nullptr)[0] == '\0');
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "test.cc"
            executable = Path(directory) / "test"
            source.write_text(program)
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I", str(BOARD), str(source), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_asset_regeneration_preserves_geometry(self):
        spec = importlib.util.spec_from_file_location("crest_generator", ROOT / "scripts/generate_orbit_crest.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        layers = module.masks()  # checks exact recombination and disjoint masks
        self.assertEqual(len(layers), 2)
        for layer in layers:
            self.assertEqual(layer.size, (384, 384))


if __name__ == "__main__":
    unittest.main()
