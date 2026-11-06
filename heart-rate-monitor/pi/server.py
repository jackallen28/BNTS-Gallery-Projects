#!/usr/bin/env python3
"""BNTS heart rate — Raspberry Pi side.

Reads lines from the Arduino over USB serial, and serves the display page plus a
live Server-Sent-Events stream (/events) to the browser.

Arduino line format:   H,<signal>,<bpm>,<beat>,<amplitude>

Run `python3 server.py --simulate` to try the display without an Arduino.
"""
import argparse
import glob
import json
import logging
import math
import os
import queue
import random
import threading
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
WEB_DIR = os.path.join(HERE, "web")
log = logging.getLogger("bnts")


class Hub:
    """Fan-out of JSON messages to every connected browser."""

    def __init__(self):
        self._lock = threading.Lock()
        self._clients = set()
        self.status = {"type": "status", "connected": False, "receiving": False,
                       "port": None, "simulated": False}
        self.last_data = 0.0

    def subscribe(self):
        q = queue.Queue(maxsize=500)
        with self._lock:
            self._clients.add(q)
        q.put_nowait(json.dumps(self.status))
        return q

    def unsubscribe(self, q):
        with self._lock:
            self._clients.discard(q)

    def publish(self, msg):
        data = json.dumps(msg, separators=(",", ":"))
        with self._lock:
            for q in list(self._clients):
                try:
                    q.put_nowait(data)
                except queue.Full:  # a stuck browser must never block the serial thread
                    pass

    def data(self, sig, bpm, beat, amp):
        self.last_data = time.monotonic()
        self.publish({"type": "h", "s": sig, "b": bpm, "x": beat, "a": amp})

    def set_status(self, **kw):
        self.status.update(kw)
        self.publish(self.status)


# --------------------------------------------------------------------------- serial
def find_port(wanted):
    if wanted and wanted != "auto":
        return wanted if os.path.exists(wanted) else None
    for pattern in ("/dev/serial/by-id/*", "/dev/ttyACM*", "/dev/ttyUSB*"):
        found = sorted(glob.glob(pattern))
        if found:
            return found[0]
    return None


def parse_line(raw):
    """b'H,512,72,1,80\\r\\n' -> (512, 72, 1, 80); anything else -> None."""
    try:
        text = raw.decode("ascii", "ignore").strip()
    except Exception:
        return None
    if not text.startswith("H,"):
        return None
    parts = text.split(",")
    if len(parts) != 5:
        return None
    try:
        sig, bpm, beat, amp = (int(p) for p in parts[1:])
    except ValueError:
        return None
    if not (0 <= sig <= 1023 and 0 <= bpm <= 300 and beat in (0, 1)):
        return None
    return sig, bpm, beat, amp


def serial_loop(hub, port_arg, baud, stop):
    try:
        import serial  # pyserial
    except ImportError:
        log.error("pyserial is missing. Install it with: sudo apt install python3-serial")
        return
    while not stop.is_set():
        port = find_port(port_arg)
        if not port:
            hub.set_status(connected=False, receiving=False, port=None)
            stop.wait(2)
            continue
        try:
            with serial.Serial(port, baud, timeout=1) as ser:
                log.info("Connected to %s @ %d", port, baud)
                hub.set_status(connected=True, port=port)
                while not stop.is_set():
                    line = ser.readline()
                    if not line:
                        continue
                    parsed = parse_line(line)
                    if parsed:
                        hub.data(*parsed)
        except (serial.SerialException, OSError) as e:
            log.warning("Serial problem (%s) — retrying", e)
        hub.set_status(connected=False, receiving=False, port=None)
        stop.wait(2)


def status_loop(hub, stop):
    """Once a second: tell the page whether data is actually flowing."""
    while not stop.wait(1):
        hub.set_status(receiving=(time.monotonic() - hub.last_data) < 3)


# ------------------------------------------------------------------------- simulator
def _g(x, mu, sd):
    d = (x - mu) / sd
    return math.exp(-d * d)


def simulate_loop(hub, mode, stop):
    """Fake Arduino: finger on/off cycle with a wandering heart rate."""
    hub.set_status(connected=True, receiving=True, port="simulator", simulated=True)
    t0 = time.monotonic()
    phase = 0.0
    bpm_f = 72.0
    shown_bpm = 0
    beats_since_on = 0
    was_on = False
    while not stop.is_set():
        t = time.monotonic() - t0
        if mode == "on":
            finger = True
        elif mode == "off":
            finger = False
        else:  # cycle: 8 s idle, 30 s finger
            finger = (t % 38) > 8
        if finger and not was_on:
            beats_since_on = 0
            bpm_f = random.choice([62, 71, 78, 88, 104])
        was_on = finger
        beat = 0
        if finger:
            bpm_f += random.uniform(-0.15, 0.15) + (72 - bpm_f) * 0.0005
            phase += bpm_f / 60.0 * 0.02
            if phase >= 1:
                phase -= 1
                beat = 1
                beats_since_on += 1
            sig = 512 + 90 * (_g(phase, 0.18, 0.08) + 0.35 * _g(phase, 0.5, 0.09)) + random.uniform(-3, 3)
            shown_bpm = int(round(bpm_f)) if beats_since_on >= 4 else 0
            amp = 110
            if beats_since_on < 4:
                beat = 0
        else:
            sig = 505 + random.uniform(-4, 4)
            shown_bpm, amp, beat = 0, random.randint(3, 8), 0
        hub.data(int(sig), shown_bpm, beat, amp)
        stop.wait(0.02)


# ---------------------------------------------------------------------------- http
def make_handler(hub):
    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *a, **k):
            super().__init__(*a, directory=WEB_DIR, **k)

        def log_message(self, fmt, *args):  # keep the journal quiet
            pass

        def end_headers(self):
            self.send_header("Cache-Control", "no-store")
            super().end_headers()

        def do_GET(self):
            path = self.path.split("?")[0]
            if path == "/events":
                return self.stream()
            if path == "/api/status":
                body = json.dumps(hub.status).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            return super().do_GET()

        def stream(self):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            q = hub.subscribe()
            try:
                while True:
                    try:
                        data = q.get(timeout=15)
                        self.wfile.write(b"data: " + data.encode() + b"\n\n")
                    except queue.Empty:
                        self.wfile.write(b": keepalive\n\n")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            finally:
                hub.unsubscribe(q)

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="auto", help="serial port, e.g. /dev/ttyACM0 (default: auto-detect)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--host", default="127.0.0.1",
                    help="address to serve on; use 0.0.0.0 to open the display from another device")
    ap.add_argument("--http-port", type=int, default=8080)
    ap.add_argument("--simulate", nargs="?", const="cycle", choices=["cycle", "on", "off"],
                    help="fake sensor data instead of reading the Arduino")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")

    hub = Hub()
    stop = threading.Event()
    if args.simulate:
        threading.Thread(target=simulate_loop, args=(hub, args.simulate, stop), daemon=True).start()
    else:
        threading.Thread(target=serial_loop, args=(hub, args.port, args.baud, stop), daemon=True).start()
    threading.Thread(target=status_loop, args=(hub, stop), daemon=True).start()

    httpd = ThreadingHTTPServer((args.host, args.http_port), make_handler(hub))
    httpd.daemon_threads = True
    log.info("Display at http://%s:%d/", args.host, args.http_port)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()


if __name__ == "__main__":
    main()
