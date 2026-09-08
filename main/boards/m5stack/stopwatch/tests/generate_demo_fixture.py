"""Emit C++ wire strings from the one canonical synthetic fixture, without deadlines logic."""
import json
from pathlib import Path
import sys


def generate(output):
    fixture = json.loads(Path(__file__).with_name("orbit_service_schedule_v1.json").read_text())
    lines = ["#pragma once", "namespace orbit::service_schedule::demo_fixture {"]
    for name, value in {
        "kAssignment": fixture["scope"]["assignment_id"],
        "kDevice": fixture["scope"]["device_id"],
        "kOccurrence": fixture["scope"]["service_occurrence_id"],
        "kSession": fixture["websocket_session_id"],
    }.items():
        lines.append(f"inline constexpr const char* {name} = {json.dumps(value)};")
    lines.append("inline constexpr const char* kFrames[] = {")
    for snapshot in fixture["snapshots"]:
        wire = json.dumps(snapshot["wire"], separators=(",", ":"), ensure_ascii=False)
        lines.append(json.dumps(wire, ensure_ascii=False) + ",")
    lines += ["};", "}"]
    Path(output).write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    generate(sys.argv[1])
