#!/usr/bin/env python3
"""Cte USB Serial z UNO a zobrazi teplotu v prohlizeci."""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import webbrowser
from collections import OrderedDict
from datetime import datetime, timedelta
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Optional
from urllib.parse import parse_qs, urlparse

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("Chybi pyserial.  pip install -r requirements.txt", file=sys.stderr)
    sys.exit(1)

STATIC = Path(__file__).resolve().parent / "static"
BAUD = 115200
MAX_POINTS = 20000
CHART_MAX = 1600
PORT_HINTS = ("arduino", "ch340", "ch341", "usb-serial", "cp210", "ftdi", "uno", "wch")
CSV_LINE = re.compile(
    r"^(\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}:\d{2});(-?\d+(?:\.\d+)?)$"
)
ISO = "%Y-%m-%d %H:%M:%S"

lock = threading.Lock()
points: OrderedDict[str, float] = OrderedDict()
state = {
    "connected": False,
    "port": "",
    "rtc": False,
    "sd": False,
    "ds18": False,
    "temp": None,
    "last_t": "",
    "demo": False,
}
ser: Optional[serial.Serial] = None
dump_event = threading.Event()
dump_error = ""
dump_added = 0
dumping = False
dump_wait_all = False


def find_port(explicit: Optional[str]) -> Optional[str]:
    if explicit:
        return explicit
    ports = list(list_ports.comports())
    for p in ports:
        blob = f"{p.description} {p.manufacturer} {p.hwid}".lower()
        if any(h in blob for h in PORT_HINTS):
            return p.device
    return ports[0].device if ports else None


def parse_temp(raw: str) -> Optional[float]:
    try:
        t = float(raw)
    except ValueError:
        return None
    if t <= -126 or t > 125:
        return None
    return t


def add_point(ts: str, temp: float) -> None:
    if not ts:
        ts = datetime.now().strftime(ISO)
    ts = ts.replace("T", " ")
    points[ts] = temp
    while len(points) > MAX_POINTS:
        points.popitem(last=False)
    state["temp"] = temp
    state["last_t"] = ts


def handle_line(line: str) -> None:
    global dump_added, dump_error, dumping
    line = line.strip()
    if not line:
        return

    if line.startswith("READY;"):
        state["connected"] = True
        return
    if line.startswith("RTC OK") or line.startswith("RTC;"):
        state["rtc"] = True
        return
    if line.startswith("RTC "):
        state["rtc"] = False
        return
    if line == "SD OK":
        state["sd"] = True
        return
    if line.startswith("SD ERR") or line == "ERR;SD":
        state["sd"] = False
        return
    if line.startswith("DS18B20 OK"):
        state["ds18"] = True
        return
    if line.startswith("DS18B20 ERR"):
        state["ds18"] = False
        return
    if line.startswith("STATUS;"):
        parts = dict(p.split("=", 1) for p in line.split(";")[1:] if "=" in p)
        state["rtc"] = parts.get("RTC") == "1"
        state["sd"] = parts.get("SD") == "1"
        state["ds18"] = parts.get("DS18") == "1"
        t = parse_temp(parts.get("T", ""))
        if t is not None:
            state["temp"] = t
        return
    if line.startswith("DUMP_BEGIN;") or line == "DUMP_ALL_BEGIN":
        dumping = True
        return
    if line == "DUMP_ALL_END":
        dumping = False
        dump_event.set()
        return
    if line == "DUMP_END":
        if not dump_wait_all:
            dumping = False
            dump_event.set()
        return
    if line.startswith("ERR;DUMP"):
        dump_error = line
        dump_event.set()
        dumping = False
        return
    if line.startswith("ERR;"):
        if dumping:
            dump_error = line
            dump_event.set()
            dumping = False
        return

    kind = None
    rest = line
    if line.startswith("LIVE;") or line.startswith("LOG;"):
        kind, rest = line.split(";", 1)
    m = CSV_LINE.match(rest)
    if not m:
        return
    t = parse_temp(m.group(2))
    if t is None:
        return
    with lock:
        add_point(m.group(1), t)
        if dumping and kind != "LIVE":
            dump_added += 1
        if kind == "LIVE":
            state["ds18"] = True
            if m.group(1):
                state["rtc"] = True


def reader_loop() -> None:
    global ser
    buf = b""
    while True:
        s = ser
        if s is None or not s.is_open:
            time.sleep(0.3)
            continue
        try:
            chunk = s.read(256)
        except serial.SerialException:
            state["connected"] = False
            time.sleep(1)
            continue
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            try:
                handle_line(raw.decode("ascii", errors="ignore"))
            except Exception:
                pass


def send_cmd(cmd: str) -> bool:
    s = ser
    if s is None or not s.is_open:
        return False
    s.write((cmd.strip() + "\n").encode("ascii"))
    s.flush()
    return True


def request_dump(all_files: bool) -> tuple[bool, int, str]:
    global dump_added, dump_error, dumping, dump_wait_all
    if state["demo"]:
        return True, 0, "demo"
    dump_event.clear()
    dump_added = 0
    dump_error = ""
    dumping = True
    dump_wait_all = all_files
    if not send_cmd("DUMP ALL" if all_files else "DUMP"):
        dumping = False
        return False, 0, "UNO neni pripojene"
    timeout = 180 if all_files else 45
    ok = dump_event.wait(timeout)
    dumping = False
    if dump_error:
        return False, dump_added, dump_error
    if not ok:
        return False, dump_added, "timeout — UNO neodpovedelo"
    return True, dump_added, ""


def downsample(items: list[dict]) -> list[dict]:
    n = len(items)
    if n <= CHART_MAX:
        return items
    step = n / CHART_MAX
    out = [items[int(i * step)] for i in range(CHART_MAX)]
    if out[-1] is not items[-1]:
        out[-1] = items[-1]
    return out


def snapshot() -> dict:
    with lock:
        items = [{"t": k, "temp": v} for k, v in sorted(points.items())]
        return {
            "connected": state["connected"] or state["demo"],
            "port": state["port"],
            "rtc": state["rtc"],
            "sd": state["sd"],
            "ds18": state["ds18"],
            "temp": state["temp"],
            "last_t": state["last_t"],
            "demo": state["demo"],
            "points": downsample(items),
        }


def demo_loop() -> None:
    state["connected"] = True
    state["port"] = "DEMO"
    state["rtc"] = state["sd"] = state["ds18"] = True
    t0 = datetime.now() - timedelta(hours=6)
    for i in range(360):
        ts = (t0 + timedelta(minutes=i)).strftime(ISO)
        temp = 48.0 + 6.0 * math.sin(i / 28.0) + 0.4 * math.sin(i / 5.0)
        with lock:
            add_point(ts, round(temp, 1))
    while True:
        time.sleep(2)
        temp = 48.0 + 6.0 * math.sin(time.time() / 40.0)
        with lock:
            add_point(datetime.now().strftime(ISO), round(temp, 1))


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(STATIC), **kwargs)

    def log_message(self, fmt, *args):
        if self.path.startswith("/api/"):
            return
        super().log_message(fmt, *args)

    def _json(self, code: int, payload: dict) -> None:
        raw = json.dumps(payload).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/api/data":
            self._json(200, snapshot())
            return
        if path in ("/", "/index.html"):
            self.path = "/index.html"
        super().do_GET()

    def do_POST(self):
        parsed = urlparse(self.path)
        if parsed.path != "/api/dump":
            self.send_error(404)
            return
        all_files = parse_qs(parsed.query).get("all", ["0"])[0] in ("1", "true")
        ok, added, err = request_dump(all_files)
        self._json(200, {"ok": ok, "added": added, "error": err})


def open_graph(url: str) -> None:
    """Male okno Edge bez karet — na Windows neni potreba dalsi prohlizec."""
    candidates = [
        shutil.which("msedge"),
        r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
        r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    ]
    for edge in candidates:
        if edge and os.path.isfile(edge):
            subprocess.Popen(
                [edge, f"--app={url}", "--window-size=960,640"],
                close_fds=True,
            )
            return
    webbrowser.open(url)


def open_serial(port: str) -> serial.Serial:
    s = serial.Serial(port, BAUD, timeout=0.2)
    return s


def boot_and_dump() -> None:
    time.sleep(4.5)
    send_cmd("STATUS")
    time.sleep(2.0)
    send_cmd("DUMP")


def main() -> int:
    global ser
    ap = argparse.ArgumentParser(description="Boiler_FVE — graf v prohlizeci")
    ap.add_argument("--port", help="COM port (jinak autodetekce)")
    ap.add_argument("--http", type=int, default=8080, help="HTTP port (8080)")
    ap.add_argument("--demo", action="store_true", help="bez UNO, umela data")
    ap.add_argument("--no-browser", action="store_true", help="neotevirat prohlizec")
    args = ap.parse_args()

    state["demo"] = args.demo
    if args.demo:
        threading.Thread(target=demo_loop, daemon=True).start()
    else:
        port = find_port(args.port)
        if not port:
            print("Nenalezen COM port. Zapoj UNO nebo pouzij --port COM10 / --demo.")
            return 1
        try:
            ser = open_serial(port)
        except serial.SerialException as e:
            print(f"Nelze otevrit {port}: {e}")
            print("Zavri PlatformIO monitor / Arduino Serial a zkus znovu.")
            return 1
        state["port"] = port
        state["connected"] = True
        threading.Thread(target=reader_loop, daemon=True).start()
        threading.Thread(target=boot_and_dump, daemon=True).start()
        print(f"UNO na {port} @ {BAUD}")

    httpd = ThreadingHTTPServer(("127.0.0.1", args.http), Handler)
    url = f"http://127.0.0.1:{args.http}/"
    print(f"Prohlizec: {url}")
    if not args.no_browser:
        threading.Timer(0.6, lambda: open_graph(url)).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print()
    finally:
        httpd.server_close()
        if ser is not None and ser.is_open:
            ser.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
