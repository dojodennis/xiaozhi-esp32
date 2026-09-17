import hashlib
import importlib.util
import math
import struct
import zlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/m5stack/stopwatch"
SIZE = 466
ARTIFACTS = Path("/opt/cursor/artifacts")


def png_from_rgb(pixels):
    def chunk(kind, payload):
        return (struct.pack("!I", len(payload)) + kind + payload
                + struct.pack("!I", zlib.crc32(kind + payload) & 0xFFFFFFFF))

    rows = b"".join(b"\0" + pixels[y * SIZE * 3:(y + 1) * SIZE * 3] for y in range(SIZE))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack("!2I5B", SIZE, SIZE, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


GLYPHS = {
    " ": ["00000"] * 7,
    "2": ["01110", "10001", "00010", "00100", "01000", "10000", "11111"],
    "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    "B": ["11110", "10001", "10001", "11110", "10001", "10001", "11110"],
    "D": ["11110", "10001", "10001", "10001", "10001", "10001", "11110"],
    "E": ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    "S": ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    "U": ["10001", "10001", "10001", "10001", "10001", "10001", "01110"],
    "Y": ["10001", "10001", "10001", "01110", "00100", "00100", "00100"],
    "T": ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
}


def blit_caption(pixels, caption):
    scale = 5
    gap = 4
    lines = caption.split("\n")
    line_h = 7 * scale + 12
    y0 = SIZE // 2 - (len(lines) * line_h) // 2 + 4
    for line_index, line in enumerate(lines):
        width_px = len(line) * (5 * scale + gap) - gap
        x0 = (SIZE - width_px) // 2
        y = y0 + line_index * line_h
        for char_index, char in enumerate(line):
            rows = GLYPHS.get(char, ["11111", "10001", "10001", "10001", "10001", "10001", "11111"])
            for row, bits in enumerate(rows):
                for col, bit in enumerate(bits):
                    if bit != "1":
                        continue
                    for dy in range(scale):
                        for dx in range(scale):
                            x = x0 + char_index * (5 * scale + gap) + col * scale + dx
                            yy = y + row * scale + dy
                            if 0 <= x < SIZE and 0 <= yy < SIZE:
                                i = (yy * SIZE + x) * 3
                                pixels[i] = pixels[i + 1] = pixels[i + 2] = 255


def raster_ring(color, radius, width, opacity, dash_count, dash_span, rotation, caption=""):
    pixels = bytearray(SIZE * SIZE * 3)
    cx = cy = (SIZE - 1) / 2.0
    inner = radius - width / 2.0
    outer = radius + width / 2.0
    red, green, blue = (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF
    alpha = opacity / 255.0
    for y in range(SIZE):
        for x in range(SIZE):
            dx, dy = x - cx, y - cy
            dist = math.hypot(dx, dy)
            if dist < inner - 1.2 or dist > outer + 1.2:
                continue
            edge = 1.0
            if dist < inner:
                edge = max(0.0, 1.0 - (inner - dist))
            elif dist > outer:
                edge = max(0.0, 1.0 - (dist - outer))
            if edge <= 0:
                continue
            angle = (math.degrees(math.atan2(dx, -dy)) + 360.0) % 360.0
            covered = dash_count <= 1
            if not covered:
                step = 360.0 / dash_count
                local = (angle - rotation) % 360.0
                covered = (local % step) <= dash_span
            if not covered:
                continue
            gain = alpha * edge
            i = (y * SIZE + x) * 3
            pixels[i] = int(red * gain)
            pixels[i + 1] = int(green * gain)
            pixels[i + 2] = int(blue * gain)
    if caption:
        blit_caption(pixels, caption)
    return png_from_rgb(bytes(pixels))


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
using namespace OrbitCrest;
int main() {
    Frame idle;
    assert(idle.band_opacity == 255 && idle.star_opacity == 255);
    assert(idle.opacity == 0 && idle.dash_count == 1);

    auto ready = Rings(State::Idle, 0, 0, false);
    assert(IsTeal(ready.color));
    assert(ready.dash_count == 1 && ready.dash_span_deg == 360);
    assert(ready.width <= 8 && ready.opacity > 160);
    assert(ready.band_opacity == 0 && ready.star_opacity == 0);
    assert(ready.radius + ready.width / 2 <= kSafeRadius);

    for (State state : {State::Listening, State::Speaking, State::Thinking}) {
        for (bool reduced : {false, true}) {
            for (uint32_t t = 0; t < 10000; t += 7) {
                for (float level : {0.0F, 0.1F, 0.5F, 1.0F}) {
                    Frame frame = Rings(state, t, level, reduced);
                    assert(frame.radius > 0 && frame.radius + frame.width / 2 <= kSafeRadius);
                    assert(frame.width >= 6);
                    assert(IsPttYellow(frame.color));
                    assert(!IsTeal(frame.color));
                    assert(frame.band_opacity == 0);
                    for (uint32_t age : {0U, 90U, 180U, 359U, 360U}) {
                        Frame transition = Transition(idle, frame, age, reduced);
                        bool visible = transition.band_opacity || transition.star_opacity ||
                                       transition.opacity > 0;
                        assert(visible);
                    }
                    for (int index = 0; index < kDashSlots; ++index) {
                        DashSlot slot = Slot(frame, index);
                        if (frame.dash_count <= 1)
                            assert(slot.visible == (index == 0));
                        else
                            assert(slot.visible == (index < frame.dash_count));
                    }
                }
            }
        }
    }

    auto listening = Rings(State::Listening, 400, 1, false);
    auto thinking = Rings(State::Thinking, 400, 1, false);
    auto speaking = Rings(State::Speaking, 400, 1, false);
    assert(listening.dash_count == 1 && listening.dash_span_deg == 360);
    assert(thinking.dash_count == kDashSlots && thinking.dash_span_deg < 30);
    assert(speaking.dash_count == kDashSlots && speaking.dash_span_deg < thinking.dash_span_deg);
    assert(speaking.rotation_deg == 0);
    assert(thinking.rotation_deg != Rings(State::Thinking, 800, 1, false).rotation_deg);
    assert(listening.width > thinking.width && thinking.width >= speaking.width);
    auto quiet_listening = Rings(State::Listening, 400, 0, false);
    auto quiet_speaking = Rings(State::Speaking, 400, 0, false);
    assert(listening.opacity > quiet_listening.opacity);
    assert(speaking.opacity > quiet_speaking.opacity);

    for (uint32_t t : {0U, 330U, 899U, 900U, 1799U, 1800U, 4999U}) {
        auto current = Rings(State::Speaking, t, 0.5F, false);
        auto next = Rings(State::Speaking, t + 33, 0.5F, false);
        assert(current.radius == next.radius);
        assert(current.dash_count == next.dash_count);
        assert(std::abs(static_cast<int>(current.opacity) - static_cast<int>(next.opacity)) <= 10);
    }

    auto start = Transition(idle, listening, 0, false);
    assert(start.star_opacity == 255 && start.band_opacity == 255);
    auto halfway = Transition(idle, listening, 180, false);
    assert(halfway.star_opacity < 255 && halfway.star_opacity > 0);
    assert(halfway.band_opacity > 0 && halfway.band_opacity < 255);
    assert(halfway.color != idle.color && halfway.color != listening.color);
    auto back = Transition(listening, idle, 180, false);
    assert(back.star_opacity < 255 && back.star_opacity > 0);
    auto interrupted = Transition(halfway, idle, 0, false);
    assert(interrupted.band_opacity == halfway.band_opacity);
    assert(interrupted.star_opacity == halfway.star_opacity);
    assert(Transition(idle, listening, kTransitionMs, false).color == listening.color);
    assert(Rings(State::Listening, 0, 0, true).radius ==
           Rings(State::Listening, 999, 1, true).radius);
    assert(Rings(State::Thinking, 0, 0, true).rotation_deg == 0);
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
    assert(Caption(State::Thinking)[0] == '\0');
    assert(Caption(State::Listening)[0] == '\0');
    for (auto input : {"Saved", "HTTP 403", "token expired", "a long technical paragraph", ""})
        assert(ResultCaption(input)[0] == '\0');
    assert(ResultCaption(nullptr)[0] == '\0');
    auto result = Rings(State::Result, 0, 0, false);
    assert(IsPttYellow(result.color) && result.opacity < 100);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "test.cc"
            executable = Path(directory) / "test"
            source.write_text(program)
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I", str(BOARD), str(source), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_moodboard_ring_frames_render(self):
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler)
        program = r'''
#include "crest_motion.h"
#include <cstdio>
using namespace OrbitCrest;
int main() {
    struct Row { const char* name; State state; uint32_t t; float level; };
    Row rows[] = {
        {"ready", State::Idle, 0, 0},
        {"listening", State::Listening, 400, 0.6F},
        {"working", State::Thinking, 700, 0},
        {"speaking", State::Speaking, 900, 0.4F},
        {"result", State::Result, 0, 0},
    };
    for (const auto& row : rows) {
        Frame frame = Rings(row.state, row.t, row.level, false);
        std::printf("%s %u %d %d %u %u %u %d\n", row.name, frame.color, frame.radius,
                    frame.width, frame.opacity, frame.dash_count, frame.dash_span_deg,
                    frame.rotation_deg);
    }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "dump.cc"
            executable = Path(directory) / "dump"
            source.write_text(program)
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I", str(BOARD), str(source), "-o", str(executable)], check=True)
            dumped = subprocess.run([str(executable)], check=True, capture_output=True, text=True)
        frames = {}
        for line in dumped.stdout.strip().splitlines():
            name, color, radius, width, opacity, dashes, span, rotation = line.split()
            frames[name] = {
                "color": int(color),
                "radius": int(radius),
                "width": int(width),
                "opacity": int(opacity),
                "dash_count": int(dashes),
                "dash_span": int(span),
                "rotation": int(rotation),
            }
        self.assertEqual(set(frames), {"ready", "listening", "working", "speaking", "result"})
        self.assertLess(frames["ready"]["width"], frames["listening"]["width"])
        self.assertEqual(frames["listening"]["dash_count"], 1)
        self.assertGreater(frames["working"]["dash_count"], 1)
        self.assertGreater(frames["speaking"]["dash_count"], 1)
        self.assertLess(frames["speaking"]["dash_span"], frames["working"]["dash_span"])
        captions = {"result": "2 SEA BASS\nUSE BY TUESDAY"}
        ARTIFACTS.mkdir(parents=True, exist_ok=True)
        for name, frame in frames.items():
            png = raster_ring(frame["color"], frame["radius"], frame["width"],
                              frame["opacity"], frame["dash_count"], frame["dash_span"],
                              frame["rotation"], captions.get(name, ""))
            (ARTIFACTS / f"orbit_ring_{name}.png").write_bytes(png)

    def test_asset_regeneration_preserves_geometry(self):
        try:
            import cairosvg  # noqa: F401
        except ImportError:
            self.skipTest("cairosvg is optional for crest asset regeneration")
        spec = importlib.util.spec_from_file_location("crest_generator", ROOT / "scripts/generate_orbit_crest.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        layers = module.masks()  # checks exact recombination and disjoint masks
        self.assertEqual(len(layers), 2)
        for layer in layers:
            self.assertEqual(layer.size, (384, 384))


if __name__ == "__main__":
    unittest.main()
