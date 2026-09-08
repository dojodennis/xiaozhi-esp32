"""Build and run the real Orbit LVGL fixture demo on localhost; no board or network dependency.

    python3 run_service_schedule_demo.py
    python3 run_service_schedule_demo.py --capture /tmp/orbit-schedule-frames

Requires CMake, a C/C++ compiler and existing managed LVGL, cJSON and board fonts.
Set ORBIT_MANAGED_COMPONENTS if they are not in a sibling firmware worktree.
Set ORBIT_CMAKE to an existing CMake binary if it is not on PATH.
The only listener binds 127.0.0.1. All time and receipt events are synthetic.
"""

import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = Path(__file__).resolve().parent
SIZE = 466
BUILD = Path(tempfile.gettempdir()) / (
    "orbit-schedule-demo-" + hashlib.sha256(str(HERE).encode()).hexdigest()[:10]
)


def managed_components():
    candidates = []
    if os.environ.get("ORBIT_MANAGED_COMPONENTS"):
        candidates.append(Path(os.environ["ORBIT_MANAGED_COMPONENTS"]))
    result = subprocess.run(
        ["git", "worktree", "list", "--porcelain"], cwd=HERE,
        capture_output=True, text=True, check=True,
    )
    candidates.extend(Path(line[9:]) / "managed_components"
                      for line in result.stdout.splitlines() if line.startswith("worktree "))
    required = ("lvgl__lvgl/CMakeLists.txt", "espressif__cjson/cJSON/cJSON.c",
                "78__xiaozhi-fonts/src/font_noto_sans_basic_16_4.c",
                "78__xiaozhi-fonts/src/font_noto_sans_basic_30_4.c")
    for candidate in candidates:
        if all((candidate / name).is_file() for name in required):
            return candidate
    raise RuntimeError("Existing LVGL/cJSON/board fonts required; set ORBIT_MANAGED_COMPONENTS")


def build(directory=BUILD):
    directory = Path(directory)
    cmake = os.environ.get("ORBIT_CMAKE") or shutil.which("cmake")
    if not cmake:
        raise RuntimeError("CMake is required")
    commands = [
        [cmake, "-S", str(HERE), "-B", str(directory),
         "-DCMAKE_BUILD_TYPE=Debug", f"-DORBIT_MANAGED_COMPONENTS={managed_components()}"],
        [cmake, "--build", str(directory), "--parallel", str(min(os.cpu_count() or 2, 8))],
    ]
    for command in commands:
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
    return directory / "service_schedule_demo_host"


def png_from_ppm(path):
    data = Path(path).read_bytes()
    header = f"P6\n{SIZE} {SIZE}\n255\n".encode()
    if not data.startswith(header) or len(data) != len(header) + SIZE * SIZE * 3:
        raise ValueError("Invalid LVGL framebuffer")
    pixels = data[len(header):]

    def chunk(kind, payload):
        return (struct.pack("!I", len(payload)) + kind + payload
                + struct.pack("!I", zlib.crc32(kind + payload)))

    rows = b"".join(b"\0" + pixels[y * SIZE * 3:(y + 1) * SIZE * 3] for y in range(SIZE))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack("!2I5B", SIZE, SIZE, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


class Demo:
    def __init__(self, executable, directory):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.frame = self.directory / "frame.ppm"
        self.process = subprocess.Popen(
            [str(executable), str(self.frame)], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, text=True, bufsize=1,
        )
        self.state = self._read()

    def _read(self):
        line = self.process.stdout.readline()
        if not line:
            raise RuntimeError(f"Demo host exited: {self.process.poll()}")
        state = json.loads(line)
        if not state["valid"] or not state["frame_written"]:
            raise RuntimeError(f"Fixture/render failure: {state}")
        self.png = png_from_ppm(self.frame)
        return state

    def command(self, command):
        if not isinstance(command, str):
            raise ValueError("Unknown fixture command")
        tick = (isinstance(command, str) and command.startswith("tick ")
                and command[5:].isascii() and command[5:].isdecimal()
                and 0 <= int(command[5:]) <= 3_600_000)
        if not tick and command not in {"next", "ack", "reset", "wrong_receipt", "receipt"}:
            raise ValueError("Unknown fixture command")
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()
        self.state = self._read()
        return self.state

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.write("quit\n")
            self.process.stdin.flush()
            self.process.wait(timeout=10)
        self.process.stdin.close()
        self.process.stdout.close()


PAGE = """<!doctype html><html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Orbit timer bench</title>
<style>body{margin:0;background:#151719;color:#eee;font:16px system-ui,sans-serif}main{max-width:1000px;margin:35px auto;padding:20px;display:flex;gap:40px;align-items:center}img{width:min(466px,85vw);height:auto;border-radius:50%;box-shadow:0 0 0 10px #303336}section{max-width:450px}h1{font-size:28px}p{line-height:1.5;color:#bcc2c6}button{border:0;border-radius:7px;padding:13px 17px;font:inherit;cursor:pointer;margin:5px 3px;background:#dfb46d;color:#151719}button:nth-child(2){background:#79b5dc}button:nth-child(3){background:#41484e;color:white}button:disabled{opacity:.4;cursor:default}#alarm{font-weight:700}details{margin-top:22px}pre{white-space:pre-wrap;word-break:break-word;font-size:12px}@media(max-width:800px){main{flex-direction:column}}</style>
<main><img id="face" alt="Actual Orbit LVGL timer face" src="/frame.png"><section>
<h1>Orbit timer integration fixture</h1><p><strong>This face is not the accepted design.</strong> Visual parity will use the existing iPhone timer. This fixture is retained to test schedule and alarm integration.</p><p>Three cooking timers. Dinner and its linked setup cue. The board’s real face renderer and alarm logic, running locally.</p>
<p id="stage"></p><p id="alarm"></p><div><button data-command="next">Next checkpoint</button><button data-command="ack">Acknowledge alarm</button><button data-command="reset">Restart</button></div>
<p>The fixture clock runs while this page is open. Next checkpoint advances the dinner scenario. Time and receipts are synthetic; hardware sound, vibration and connectivity are not measured here.</p>
<details><summary>Fixture state</summary><pre id="state"></pre></details></section></main>
<script>let busy=false;async function refresh(s){document.querySelector('#stage').textContent=s.stage;document.querySelector('#alarm').textContent=s.alarm_active?'ALARM ACTIVE · '+s.due.join(', '):'No active alarm';document.querySelector('#alarm').style.color=s.alarm_active?'#ee8276':'#9cccac';document.querySelector('#state').textContent=JSON.stringify(s,null,2);document.querySelector('#face').src='/frame.png?t='+Date.now();document.querySelector('[data-command=ack]').disabled=!s.alarm_active;document.querySelector('[data-command=next]').disabled=s.stage.startsWith('Demo complete');}for(const b of document.querySelectorAll('button'))b.onclick=async()=>{if(busy)return;busy=true;try{const r=await fetch('/command',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({command:b.dataset.command})});if(!r.ok)throw Error(await r.text());await refresh(await r.json());}catch(e){document.querySelector('#stage').textContent='Local demo error: '+e;}finally{busy=false;}};async function poll(){if(busy)return;busy=true;try{await refresh(await(await fetch('/state')).json());}finally{busy=false;}}poll();setInterval(poll,1000);</script></html>"""


def capture(demo, destination):
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    records = []
    commands = [None, "next", "next", "next", "ack", "next", "ack", "next", "ack",
                "next", "ack", "next", "next", "next", "next", "ack", "next"]
    for index, command in enumerate(commands):
        if command:
            demo.command(command)
        name = f"{index:02d}.png"
        (destination / name).write_bytes(demo.png)
        records.append({"frame": name, "command": command, **demo.state})
    (destination / "states.json").write_text(json.dumps(records, indent=2) + "\n")
    return records


def serve(demo, port):
    last_tick = time.monotonic_ns()

    def elapse():
        nonlocal last_tick
        now = time.monotonic_ns()
        delta = (now - last_tick) // 1_000_000
        # Bounded commands also cover a browser reopened after a long pause.
        while delta > 0:
            step = min(delta, 3_600_000)
            demo.command(f"tick {step}")
            last_tick += step * 1_000_000
            delta -= step

    class Handler(BaseHTTPRequestHandler):
        def respond(self, data, content_type, status=200):
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path == "/":
                self.respond(PAGE.encode(), "text/html; charset=utf-8")
            elif path == "/frame.png":
                self.respond(demo.png, "image/png")
            elif path == "/state":
                elapse()
                self.respond(json.dumps(demo.state).encode(), "application/json")
            else:
                self.respond(b"Not found", "text/plain", 404)

        def do_POST(self):
            nonlocal last_tick
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if self.path != "/command" or not 0 < length <= 200:
                    raise ValueError("Invalid command request")
                origin = self.headers.get("Origin")
                if origin and origin != f"http://{self.headers.get('Host')}":
                    raise ValueError("Cross-origin commands are unavailable")
                command = json.loads(self.rfile.read(length))["command"]
                if not isinstance(command, str) or command not in {"next", "ack", "reset"}:
                    raise ValueError("Unsupported browser control")
                elapse()
                state = demo.command(command)
                if command == "reset":
                    last_tick = time.monotonic_ns()
                self.respond(json.dumps(state).encode(), "application/json")
            except (ValueError, KeyError) as error:
                self.respond(str(error).encode(), "text/plain", 400)

        def log_message(self, *_):
            pass

    server = HTTPServer(("127.0.0.1", port), Handler)
    print(f"Orbit timer demo: http://127.0.0.1:{server.server_port}", flush=True)
    try:
        server.serve_forever()
    finally:
        server.server_close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8768)
    parser.add_argument("--capture", type=Path)
    parser.add_argument("--build-only", action="store_true")
    options = parser.parse_args()
    print("Building actual LVGL fixture demo…", file=sys.stderr, flush=True)
    executable = build()
    if options.build_only:
        print(executable)
        return
    with tempfile.TemporaryDirectory(prefix="orbit-schedule-frames-") as directory:
        demo = Demo(executable, directory)
        try:
            if options.capture:
                capture(demo, options.capture)
                print(options.capture.resolve())
            else:
                serve(demo, options.port)
        finally:
            demo.close()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
