"""Capture the exact board RenderHardwareBench with local LVGL/fonts. No hardware/network.

The generated class contains source-extracted board methods/fields/constants, not
an independently reproduced layout. Only the display lock, inherited (hidden)
normal-screen pointers and unused platform monotonic API are host stubs.
"""

import argparse
import hashlib
import itertools
import math
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from run_service_schedule_demo import managed_components, png_from_ppm

HERE = Path(__file__).resolve().parent
BOARD = HERE.parent
METHODS = (
    "SetVisible", "EffectiveServerNowMs", "ShouldShowOrbitLocked", "SetReplyLayoutLocked",
    "RefreshOrbitLocked", "CreateOrbitUiLocked", "RenderHardwareBench",
)


def block(source, marker):
    """Extract one complete C++ block, respecting strings and comments."""
    start = source.index(marker)
    opened = source.index("{", start)
    depth = 0
    state = None
    index = opened
    while index < len(source):
        char = source[index]
        following = source[index:index + 2]
        if state in ('"', "'"):
            if char == "\\":
                index += 2
                continue
            if char == state:
                state = None
        elif state == "//":
            if char == "\n":
                state = None
        elif state == "/*":
            if following == "*/":
                state = None
                index += 2
                continue
        elif following in ("//", "/*"):
            state = following
            index += 2
            continue
        elif char in ('"', "'"):
            state = char
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if not depth:
                return source[start:index + 1]
        index += 1
    raise ValueError(f"Incomplete block: {marker}")


def extract(destination):
    source_path = BOARD / "m5stack_stopwatch.cc"
    source = source_path.read_text()
    constants = []
    for name in ("kOrbitArcDiameter", "kOrbitLabelWidth", "kOrbitAlarmMaximumNames",
                 "kColorCream", "kColorAmber", "kColorRed", "kColorGold"):
        match = re.search(rf"^constexpr [^\n]*\b{name}\b[^;]*;", source, re.MULTILINE)
        if not match:
            raise ValueError(f"Missing board constant {name}")
        constants.append(match.group())
    constants.append(block(source, "constexpr std::array<uint32_t, ProvisionsStopwatchOrbit::kMaximumSlots> kOrbitColors") + ";")
    enum = block(source, "    enum class VisualState") + ";"
    fields_start = source.index("    struct OrbitSlotObjects")
    fields_end = source.index("\n#endif", source.index("    bool bench_clock_trusted_", fields_start))
    fields = source[fields_start:fields_end] + "\n#endif"
    extracted = []
    for name in METHODS:
        pattern = re.search(rf"^    (?:static )?[^\n]*\b{name}\(", source, re.MULTILINE)
        if not pattern:
            raise ValueError(f"Missing board method {name}")
        extracted.append(block(source, pattern.group()))
    preamble = '''// Generated from the actual board source; do not edit.
#pragma once
#include "sdkconfig.h"
#include "lvgl.h"
#include "orbit_dial.h"
#include "utf8_ellipsis.h"
#include "service_schedule_hardware_bench.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <limits>
#include <string>
#include <vector>
using esp_timer_handle_t = void*;
inline int64_t esp_timer_get_time() { return 0; }
struct DisplayLockGuard { explicit DisplayLockGuard(void*) {} };
LV_FONT_DECLARE(font_noto_sans_basic_16_4);
LV_FONT_DECLARE(font_noto_sans_basic_30_4);
'''
    generated = (preamble + "\n".join(constants) + "\nclass ExactBoardRenderer {\npublic:\n"
                 + enum + "\n" + fields + "\n"
                 + "lv_obj_t* top_bar_ = nullptr; lv_obj_t* status_bar_ = nullptr;\n"
                 + "\n".join(extracted) + "\n};\n")
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "exact_board_renderer.h").write_text(generated)
    manifest = {
        "board_source": str(source_path), "board_sha256": hashlib.sha256(source.encode()).hexdigest(),
        "generated_sha256": hashlib.sha256(generated.encode()).hexdigest(),
        "methods": {name: hashlib.sha256(text.encode()).hexdigest() for name, text in zip(METHODS, extracted)},
        "host_stubs": ["single-thread no-op display lock", "hidden inherited normal-screen pointers",
                       "unused esp_timer_get_time after hardware bench early return"],
        "evidence": "Host LVGL renderer only; no physical visibility/audio/haptic proof.",
    }
    (destination / "renderer-source.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def validate_frames(frames, destination):
    if len(frames) != 11:
        raise ValueError("Expected all eleven board scenarios")
    visible_labels = 0
    for frame in frames:
        labels = [label for label in frame["visible_labels"] if label["text"]]
        for label in labels:
            x, y, right, bottom = label["bounds"]
            if max(math.hypot(a - 232.5, b - 232.5)
                   for a in (x, right) for b in (y, bottom)) > 233:
                raise ValueError(f"Label extends beyond round screen: {frame['png']} {label}")
        for first, second in itertools.combinations(labels, 2):
            x, y, right, bottom = first["bounds"]
            a, b, c, d = second["bounds"]
            if max(x, a) <= min(right, c) and max(y, b) <= min(bottom, d):
                raise ValueError(f"Overlapping labels: {frame['png']} {first} {second}")
        visible_labels += len(labels)
    if [(frame["due"], frame["pending"]) for frame in frames[2:8]] != [
            (0, 0), (6, 0), (5, 1), (5, 1), (5, 1), (5, 1)]:
        raise ValueError("Captured alarm/ACK scenarios differ from expected actual worker states")
    if "NEEDS SYNC" not in frames[6]["status"] or "NEEDS SYNC" not in frames[8]["status"]:
        raise ValueError("Restored states must not display fresh clock trust")
    proof = {
        "frames": len(frames), "visible_label_rectangles": visible_labels,
        "label_overlaps": 0, "labels_outside_round_boundary": 0,
        "full_screen_invalidation_per_capture": True,
        "two_full_redraws_pixel_identical": True,
        "scope": "Exact extracted timer/alarm renderer with host LVGL and production fonts; no hardware proof.",
    }
    (destination / "geometry-checks.json").write_text(json.dumps(proof, indent=2) + "\n")
    return proof


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, required=True)
    options = parser.parse_args()
    destination = options.capture.resolve()
    build = Path(tempfile.gettempdir()) / "orbit-hardware-bench-render-build"
    manifest = extract(build)
    cmake = os.environ.get("ORBIT_CMAKE") or shutil.which("cmake")
    if not cmake:
        raise RuntimeError("Set ORBIT_CMAKE to the existing CMake executable")
    commands = [
        [cmake, "-S", str(HERE / "hardware_bench_render"), "-B", str(build),
         f"-DORBIT_MANAGED_COMPONENTS={managed_components()}", "-DCMAKE_BUILD_TYPE=Debug"],
        [cmake, "--build", str(build), "--parallel", str(min(os.cpu_count() or 2, 8))],
    ]
    for command in commands:
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
    destination.mkdir(parents=True, exist_ok=True)
    result = subprocess.run([str(build / "hardware_bench_render"), str(destination)],
                            capture_output=True, text=True, timeout=60)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    frames = [json.loads(line) for line in result.stdout.splitlines()]
    for frame in frames:
        ppm = destination / frame.pop("ppm")
        name = ppm.with_suffix(".png")
        png = png_from_ppm(ppm)
        name.write_bytes(png)
        frame["png"] = name.name
        frame["png_sha256"] = hashlib.sha256(png).hexdigest()
    proof = validate_frames(frames, destination)
    (destination / "frames.json").write_text(json.dumps(frames, indent=2) + "\n")
    (destination / "renderer-source.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"capture": str(destination), "checks": proof, **manifest}, indent=2))


if __name__ == "__main__":
    main()
