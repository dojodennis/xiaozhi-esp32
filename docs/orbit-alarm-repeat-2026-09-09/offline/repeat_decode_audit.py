#!/usr/bin/env python3
"""Reproduce the Orbit exclamation clip's repeated host decode/resample path.

This is an offline diagnostic. It does not invoke ESP-IDF, serial, a device,
the phone, a provider, or a firmware build.
"""

from __future__ import annotations

import argparse
import array
import ctypes
import hashlib
import json
import math
import pathlib
import platform
import shutil
import subprocess
from typing import Iterable


DEFAULT_ASSET = pathlib.Path(__file__).resolve().parents[3] / "main/assets/common/exclamation.ogg"
DEFAULT_COMMIT = "45772685c082a55ef26673da078979a96facdbf6"
EXPECTED_ASSET_SHA256 = "61d0a91d7658e62c2ee0a940bd8b99e2062c4b760848f0ba2d793c852def4716"


def parse_ogg_packets(blob: bytes) -> list[bytes]:
    packets: list[bytes] = []
    packet = bytearray()
    offset = 0
    while offset + 27 <= len(blob):
        if blob[offset : offset + 4] != b"OggS":
            offset += 1
            continue
        segment_count = blob[offset + 26]
        table_start = offset + 27
        table = blob[table_start : table_start + segment_count]
        body_start = table_start + segment_count
        body_size = sum(table)
        body = blob[body_start : body_start + body_size]
        body_offset = 0
        for segment_size in table:
            packet.extend(body[body_offset : body_offset + segment_size])
            body_offset += segment_size
            if segment_size < 255:
                packets.append(bytes(packet))
                packet.clear()
        offset = body_start + body_size
    if packet:
        raise ValueError("unterminated Ogg packet")
    return packets


def stats(samples: Iterable[int]) -> dict[str, float | int]:
    values = list(samples)
    if not values:
        raise ValueError("empty PCM segment")
    peak = max(abs(value) for value in values)
    rms = math.sqrt(sum(value * value for value in values) / len(values))
    return {
        "samples": len(values),
        "peak": peak,
        "peak_dbfs": round(20 * math.log10(peak / 32768), 3),
        "rms": round(rms, 2),
        "rms_dbfs": round(20 * math.log10(rms / 32768), 3),
    }


def load_libopus() -> tuple[ctypes.CDLL, str, str]:
    candidates = [
        "/opt/homebrew/lib/libopus.0.dylib",
        "/opt/homebrew/lib/libopus.dylib",
        "libopus.so.0",
        "libopus.so",
    ]
    for candidate in candidates:
        try:
            library = ctypes.CDLL(candidate)
            library.opus_get_version_string.restype = ctypes.c_char_p
            version = library.opus_get_version_string().decode("ascii")
            return library, candidate, version
        except OSError:
            continue
    raise RuntimeError("libopus was not found")


def decode_repeatedly(audio_packets: list[bytes], passes: int) -> dict:
    library, path, version = load_libopus()
    error = ctypes.c_int()
    library.opus_decoder_create.argtypes = [
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_int),
    ]
    library.opus_decoder_create.restype = ctypes.c_void_p
    library.opus_decode.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_int16),
        ctypes.c_int,
        ctypes.c_int,
    ]
    library.opus_decode.restype = ctypes.c_int
    library.opus_decoder_destroy.argtypes = [ctypes.c_void_p]
    decoder = library.opus_decoder_create(16000, 1, ctypes.byref(error))
    if not decoder or error.value != 0:
        raise RuntimeError(f"opus_decoder_create failed: {error.value}")

    results = []
    try:
        for repeat in range(passes):
            samples: list[int] = []
            for payload in audio_packets:
                pcm = (ctypes.c_int16 * 960)()
                raw = ctypes.create_string_buffer(payload)
                decoded = library.opus_decode(decoder, raw, len(payload), pcm, 960, 0)
                if decoded < 0:
                    raise RuntimeError(f"opus_decode failed on pass {repeat}: {decoded}")
                samples.extend(pcm[:decoded])
            result = stats(samples)
            result["pass"] = repeat
            result["packets_decoded"] = len(audio_packets)
            results.append(result)
    finally:
        library.opus_decoder_destroy(decoder)
    return {"library": path, "version": version, "passes": results}


def ffmpeg_loop(asset: pathlib.Path, passes: int) -> dict:
    ffmpeg = shutil.which("ffmpeg") or "/opt/homebrew/bin/ffmpeg"
    command = [
            ffmpeg,
            "-v",
            "error",
            "-stream_loop",
            str(passes - 1),
            "-i",
            str(asset),
            "-f",
            "s16le",
            "-acodec",
            "pcm_s16le",
            "-ar",
            "24000",
            "-ac",
            "1",
            "-",
        ]
    completed = subprocess.run(
        command,
        check=True,
        capture_output=True,
    )
    pcm = array.array("h")
    pcm.frombytes(completed.stdout)
    results = []
    for repeat in range(passes):
        start = round(repeat * len(pcm) / passes)
        end = round((repeat + 1) * len(pcm) / passes)
        result = stats(pcm[start:end])
        result["pass"] = repeat
        results.append(result)
    return {
        "command": command,
        "version": subprocess.run(
            [ffmpeg, "-version"], check=True, capture_output=True, text=True
        ).stdout.splitlines()[0],
        "total_samples": len(pcm),
        "passes": results,
    }


def ffprobe_metadata(asset: pathlib.Path) -> dict:
    ffprobe = shutil.which("ffprobe") or "/opt/homebrew/bin/ffprobe"
    return json.loads(
        subprocess.run(
            [
                ffprobe,
                "-v",
                "error",
                "-show_streams",
                "-show_format",
                "-of",
                "json",
                str(asset),
            ],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--asset", type=pathlib.Path, default=DEFAULT_ASSET)
    parser.add_argument("--commit", default=DEFAULT_COMMIT)
    parser.add_argument("--passes", type=int, default=10)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    if not 1 <= args.passes <= 100:
        parser.error("passes must be between 1 and 100")
    blob = args.asset.read_bytes()
    if hashlib.sha256(blob).hexdigest() != EXPECTED_ASSET_SHA256:
        raise ValueError("asset differs from the frozen supervised bench clip")
    packets = parse_ogg_packets(blob)
    audio_packets = [
        packet for packet in packets if not packet.startswith((b"OpusHead", b"OpusTags"))
    ]
    evidence = {
        "source_commit": args.commit,
        "asset": {
            "path": str(args.asset),
            "bytes": len(blob),
            "sha256": hashlib.sha256(blob).hexdigest(),
            "ffprobe": ffprobe_metadata(args.asset),
        },
        "host": {
            "python": platform.python_version(),
            "platform": platform.platform(),
            "ffmpeg": (shutil.which("ffmpeg") or "/opt/homebrew/bin/ffmpeg"),
            "ffprobe": (shutil.which("ffprobe") or "/opt/homebrew/bin/ffprobe"),
        },
        "ogg": {
            "packets_total": len(packets),
            "audio_packets": len(audio_packets),
            "audio_packet_bytes_min": min(map(len, audio_packets)),
            "audio_packet_bytes_max": max(map(len, audio_packets)),
        },
        "same_decoder_repeated": decode_repeatedly(audio_packets, args.passes),
        "ffmpeg_resampled_repeated": ffmpeg_loop(args.asset, args.passes),
        "limitations": [
            "Host libopus and FFmpeg only; this does not exercise the ESP Opus wrapper.",
            "Host resampling is FFmpeg/libswresample, not the ESP audio-effects resampler.",
            "No ES8311, DMA, PA_EN, speaker, enclosure, power, or physical audibility proof.",
            "The experiment does not prove the frozen device cannot lose output at runtime.",
        ],
    }
    encoded = json.dumps(evidence, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8")
    else:
        print(encoded, end="")


if __name__ == "__main__":
    main()
