"""Collect only synthetic, checksummed Orbit framebuffer fixtures over local USB."""

import argparse
import json
from pathlib import Path
import re
import struct
import time


CASES = (
    "idle", "light_theme_idle", "dark_theme_idle", "listening_low", "listening_high",
    "thinking", "speaking_active_a", "speaking_active_b", "speaking_stale_a",
    "speaking_stale_b", "result", "result_expired_idle", "reply_only",
    "unknown_labels_ignored", "error", "sleep", "wake_idle",
    "interrupted_transition", "final_idle",
)


def checksum(payload):
    value = 2166136261
    for byte in payload:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def decode_rle(payload, width, height):
    if (width, height) != (466, 466) or not payload or len(payload) % 4:
        raise ValueError("invalid synthetic frame dimensions or packet length")
    if len(payload) > width * height * 4:
        raise ValueError("oversized frame packet")
    decoded = bytearray()
    for count, pixel in struct.iter_unpack("<HH", payload):
        if count == 0 or len(decoded) + count * 2 > width * height * 2:
            raise ValueError("invalid frame run length")
        decoded.extend(struct.pack("<H", pixel) * count)
    if len(decoded) != width * height * 2:
        raise ValueError("incomplete frame")
    return bytes(decoded)


def validate_metadata(value):
    expected = {"state", "band", "star", "color", "speech_clock_ms", "level",
                "timer_paused", "power_save", "caption", "radii", "opacities"}
    if not isinstance(value, dict) or set(value) != expected:
        raise ValueError("invalid fixture metadata")
    captions = {"", "Thinking", "Found in records", "Reply received", "Please try again"}
    if value["caption"] not in captions:
        raise ValueError("unexpected fixture caption; not recorded")
    for field in ("radii", "opacities"):
        if not isinstance(value[field], list) or len(value[field]) != 3:
            raise ValueError("invalid ring metadata")
        if any(type(item) is not int or not 0 <= item <= 255 for item in value[field]):
            raise ValueError("invalid ring metadata value")
    for field in ("state", "band", "star", "color", "speech_clock_ms"):
        if type(value[field]) is not int or not 0 <= value[field] <= 0xFFFFFFFF:
            raise ValueError("invalid numeric metadata")
    if type(value["level"]) not in (float, int) or not 0 <= value["level"] <= 1:
        raise ValueError("invalid synthetic audio level")
    if any(type(value[key]) is not bool for key in ("timer_paused", "power_save")):
        raise ValueError("invalid timer metadata")
    return value


def assess(frames, metadata):
    checks = []

    def check(name, result):
        checks.append({"check": name, "passed": bool(result)})

    check("all_19_framebuffers_captured", set(frames) == set(CASES))
    idle_names = ("idle", "light_theme_idle", "dark_theme_idle", "result_expired_idle",
                  "unknown_labels_ignored", "wake_idle", "final_idle")
    for name in idle_names:
        item = metadata[name]
        check(name + "_crest_and_idle_timer", item["state"] == 2 and item["band"] == 255
              and item["star"] == 255 and item["opacities"] == [0, 0, 0]
              and item["timer_paused"] and not item["caption"])
        check(name + "_matches_original_idle_pixels", frames[name] == frames["idle"])
    for name in ("listening_low", "listening_high"):
        item = metadata[name]
        check(name + "_gold_tight_rings", item["state"] == 3 and item["color"] == 0xC5A46D
              and item["band"] == item["star"] == 0
              and all(abs(a - b) <= 5 for a, b in zip(item["radii"], (120, 154, 184)))
              and item["opacities"] == [204, 204, 204] and not item["caption"])
    check("synthetic_input_level_is_reactive", metadata["listening_high"]["level"] > 0.8
          and metadata["listening_low"]["level"] < 0.3)
    check("thinking_caption_bounded", metadata["thinking"]["state"] == 4
          and metadata["thinking"]["caption"] == "Thinking")
    for name in ("speaking_active_a", "speaking_active_b", "speaking_stale_a", "speaking_stale_b"):
        item = metadata[name]
        check(name + "_ivory_outward_rings", item["state"] == 5 and item["color"] == 0xE8E0D2
              and item["band"] == item["star"] == 0
              and all(116 <= radius <= 194 for radius in item["radii"]) and not item["caption"])
    check("synthetic_output_advances_speech_clock", metadata["speaking_active_b"]["speech_clock_ms"]
          > metadata["speaking_active_a"]["speech_clock_ms"])
    check("stale_output_stops_speech_clock", metadata["speaking_stale_a"]["speech_clock_ms"]
          == metadata["speaking_stale_b"]["speech_clock_ms"])
    check("known_receipt_is_short", metadata["result"]["state"] == 7
          and metadata["result"]["caption"] == "Found in records")
    check("raw_transcript_not_rendered", metadata["reply_only"]["state"] == 7
          and metadata["reply_only"]["caption"] == "Reply received")
    check("bounded_error_caption", metadata["error"]["state"] == 6
          and metadata["error"]["caption"] == "Please try again")
    check("sleep_is_black_and_timer_paused", not any(frames["sleep"])
          and metadata["sleep"]["power_save"] and metadata["sleep"]["timer_paused"])
    check("interrupted_transition_stays_visible", any(frames["interrupted_transition"])
          and metadata["interrupted_transition"]["star"] > 0)
    boundary = ({x for x in range(466)} | {465 * 466 + x for x in range(466)}
                | {y * 466 for y in range(466)} | {y * 466 + 465 for y in range(466)})
    for name in CASES:
        check(name + "_outer_frame_boundary_black",
              all(frames[name][pixel * 2:pixel * 2 + 2] == b"\0\0" for pixel in boundary))
    return checks


def collect(port, destination, timeout):
    import serial
    from PIL import Image

    destination.mkdir(mode=0o700, parents=False, exist_ok=False)
    frames, metadata = {}, {}
    current = None
    buffer = bytearray()
    armed = False
    completed = False
    device = serial.Serial()
    device.port, device.baudrate, device.timeout = port, 115200, 0.5
    device.dtr = device.rts = False
    deadline = time.monotonic() + timeout
    device.open()
    try:
        with device:
            while time.monotonic() < deadline and not completed:
                buffer.extend(device.read(4096))
                if len(buffer) > 32768 and b"\n" not in buffer:
                    buffer.clear()
                while b"\n" in buffer:
                    raw, _, buffer = buffer.partition(b"\n")
                    # Ignore all ordinary logs, including identity/settings.
                    if not raw.startswith(b"ORBIT_"):
                        continue
                    line = raw.decode("ascii").rstrip("\r")
                    if line.startswith("ORBIT_READY_V1 ") and not armed:
                        device.write(b"ORBIT_SELFTEST_V1 RUN\n")
                        armed = True
                        print("SYNTHETIC_USB_TEST_ARMED", flush=True)
                    elif line.startswith("ORBIT_META_V1 "):
                        _, name, body = line.split(" ", 2)
                        if name not in CASES or name in metadata or len(body) > 2048:
                            raise ValueError("unexpected metadata frame")
                        metadata[name] = validate_metadata(json.loads(body))
                    elif line.startswith("ORBIT_FRAME_V1 "):
                        _, name, width, height, length, digest = line.split()
                        if current or name not in metadata or name in frames:
                            raise ValueError("unexpected frame start")
                        width, height, length = int(width), int(height), int(length)
                        if (width, height) != (466, 466) or not 0 < length <= 466 * 466 * 4:
                            raise ValueError("invalid frame bound")
                        current = (name, width, height, length, int(digest, 16), bytearray())
                    elif line.startswith("ORBIT_DATA_V1 "):
                        _, offset, hex_data = line.split()
                        if not current or int(offset) != len(current[5]):
                            raise ValueError("nonsequential frame data")
                        if not re.fullmatch(r"[0-9a-f]{2,512}", hex_data) or len(hex_data) % 2:
                            raise ValueError("invalid frame data encoding")
                        current[5].extend(bytes.fromhex(hex_data))
                        if len(current[5]) > current[3]:
                            raise ValueError("oversized frame data")
                    elif line.startswith("ORBIT_END_V1 "):
                        name = line.split()[1]
                        if not current or current[0] != name or len(current[5]) != current[3]:
                            raise ValueError("incomplete frame transport")
                        if checksum(current[5]) != current[4]:
                            raise ValueError("frame transport checksum mismatch")
                        frames[name] = decode_rle(current[5], current[1], current[2])
                        picture = Image.frombytes("RGB", (466, 466), frames[name], "raw", "BGR;16")
                        picture.save(destination / (name + ".png"))
                        current = None
                        print("FRAME_CAPTURED=" + name, flush=True)
                    elif line == "ORBIT_DONE_V1 19 synthetic_frames":
                        completed = True
                    elif line.startswith("ORBIT_FAIL_V1 "):
                        raise ValueError("device reported incomplete diagnostic run")
            if not completed or set(frames) != set(CASES):
                raise ValueError("synthetic capture incomplete or timed out")
        checks = assess(frames, metadata)
        report = {
            "scope": "synthetic renderer/framebuffer only; no physical audio/button acceptance",
            "frame_count": len(frames), "metadata": metadata, "checks": checks,
            "passed": all(item["passed"] for item in checks),
            "raw_logs_saved": False, "room_audio_recorded": False,
        }
        (destination / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps({"frames": len(frames), "checks": len(checks),
                          "failed": [item["check"] for item in checks if not item["passed"]]}))
        return report["passed"]
    except Exception:
        (destination / "INCOMPLETE.txt").write_text("Capture incomplete; no acceptance claimed.\n")
        raise


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/cu.usbmodem1101")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--timeout", type=int, default=180, choices=range(30, 301))
    args = parser.parse_args()
    raise SystemExit(0 if collect(args.port, args.output, args.timeout) else 2)
