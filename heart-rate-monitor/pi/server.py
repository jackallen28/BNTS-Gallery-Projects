#!/usr/bin/env python3
"""BNTS heart rate — Raspberry Pi side.

Reads lines from the Arduino over USB serial, and serves the display page plus a live Server-Sent-Events stream
(/events) to the browser. The page does the measuring (press the screen, average the beats); this just passes the
Arduino's lines on.

Arduino line format:   H,<signal>,<beat>,<beatMs>
  signal  the pulse waveform 0..1023     beat    1 on the line where a beat was just detected
  beatMs  the Arduino's millisecond clock at the most recent beat (the display uses these times for the average)
Older firmware sent H,<signal>,<bpm>,<beat>,<amplitude>[,<stage>[,<progress>]]; it is recognised and the screen asks
for the new firmware to be flashed.

Run `python3 server.py --simulate` to try the display without an Arduino.
"""
import argparse
import glob
import hashlib
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


def web_version():
    """A short fingerprint of the display files, so an open page can notice when an update changed them."""
    h = hashlib.sha1()
    for root, _dirs, files in sorted(os.walk(WEB_DIR)):
        for name in sorted(files):
            p = os.path.join(root, name)
            try:
                st = os.stat(p)
                h.update(f"{os.path.relpath(p, WEB_DIR)}:{st.st_size}:{st.st_mtime_ns}".encode())
            except OSError:
                pass
    return h.hexdigest()[:10]


class Hub:
    """Fan-out of JSON messages to every connected browser."""

    def __init__(self):
        self._lock = threading.Lock()
        self._clients = set()
        self._raw_clients = set()
        self.status = {"type": "status", "connected": False, "receiving": False, "wrong_program": False,
                       "old_firmware": False, "other_line": "", "port": None, "simulated": False}
        self.last_data = 0.0
        self.last_other = 0.0          # last time the Arduino sent text that isn't our data format
        self.last_old = 0.0            # last time the Arduino sent lines in the OLD (pre-v3) data format
        self.last_other_line = ""
        self._last_warn = 0.0

    def subscribe(self):
        q = queue.Queue(maxsize=500)
        with self._lock:
            self._clients.add(q)
        q.put_nowait(json.dumps(self.status))
        return q

    def unsubscribe(self, q):
        with self._lock:
            self._clients.discard(q)

    # --- raw serial lines, for `bnts-monitor` (a Serial Monitor over SSH) ---
    def raw_subscribe(self):
        q = queue.Queue(maxsize=2000)
        with self._lock:
            self._raw_clients.add(q)
        return q

    def raw_unsubscribe(self, q):
        with self._lock:
            self._raw_clients.discard(q)

    def raw(self, text):
        """Pass one line the Arduino sent to anyone watching with bnts-monitor."""
        if not self._raw_clients:
            return
        with self._lock:
            for q in list(self._raw_clients):
                try:
                    q.put_nowait(text)
                except queue.Full:
                    pass

    def publish(self, msg):
        data = json.dumps(msg, separators=(",", ":"))
        with self._lock:
            for q in list(self._clients):
                try:
                    q.put_nowait(data)
                except queue.Full:  # a stuck browser must never block the serial thread
                    pass

    def data(self, sig, beat, beat_ms):
        """One data line from the Arduino, stamped with the time the Pi read it."""
        self.last_data = time.monotonic()
        self.publish({"type": "h", "s": sig, "x": beat, "bm": beat_ms, "t": int(self.last_data * 1000)})

    def old_format(self):
        self.last_old = time.monotonic()

    def other_line(self, text):
        """The Arduino sent something that is not an 'H,...' data line (usually a test sketch is loaded)."""
        now = time.monotonic()
        self.last_other, self.last_other_line = now, text[:80]
        if now - self._last_warn > 30:
            self._last_warn = now
            log.warning("The Arduino is sending text the display doesn't understand (%r). It is probably running a "
                        "test sketch: flash the real firmware with ./flash-arduino.sh", self.last_other_line)

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
    """Parse one serial line.
    b'H,512,1,48213'   -> {"kind": "new", "s": 512, "x": 1, "bm": 48213}
    old-format H lines -> {"kind": "old"}       anything else -> None"""
    try:
        text = raw.decode("ascii", "ignore").strip()
    except Exception:
        return None
    if not text.startswith("H,"):
        return None
    parts = text.split(",")
    if len(parts) in (5, 6, 7):
        return {"kind": "old"}
    if len(parts) != 4:
        return None
    try:
        sig, beat, beat_ms = (int(p) for p in parts[1:])
    except ValueError:
        return None
    if not (0 <= sig <= 1023 and beat in (0, 1) and 0 <= beat_ms <= 0xFFFFFFFF):
        return None
    return {"kind": "new", "s": sig, "x": beat, "bm": beat_ms}


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
                    text = line.decode("ascii", "replace").strip()
                    if text:
                        hub.raw(text)
                    parsed = parse_line(line)
                    if parsed and parsed["kind"] == "new":
                        hub.data(parsed["s"], parsed["x"], parsed["bm"])
                    elif parsed:
                        hub.old_format()
                    elif text and not text.startswith("#"):     # '# ...' lines are our own status messages
                        hub.other_line(text)
        except (serial.SerialException, OSError) as e:
            log.warning("Serial problem (%s) — retrying", e)
        hub.set_status(connected=False, receiving=False, port=None)
        stop.wait(2)


def status_loop(hub, stop):
    """Once a second: tell the page whether data is actually flowing."""
    while not stop.wait(1):
        now = time.monotonic()
        receiving = (now - hub.last_data) < 3
        wrong = (not receiving) and (now - hub.last_other) < 3
        old = (not receiving) and (now - hub.last_old) < 3
        hub.set_status(receiving=receiving, wrong_program=wrong, old_firmware=old,
                       other_line=hub.last_other_line if wrong else "")


# ------------------------------------------------------------------------- simulator
def _g(x, mu, sd):
    d = (x - mu) / sd
    return math.exp(-d * d)


def simulate_loop(hub, mode, stop):
    """Fake Arduino: sends the same lines as the real firmware, with beats whenever a "finger" is on the sensor.
    on = always,  off = never,  cycle = finger on for 30 s, off for 8 s."""
    hub.set_status(connected=True, receiving=True, port="simulator", simulated=True)
    t0 = time.monotonic()
    phase = random.random()
    bpm_f = 72.0
    beat_ms = 0
    was_on = False
    while not stop.is_set():
        t = time.monotonic() - t0
        if mode == "on":
            finger = True
        elif mode == "off":
            finger = False
        else:  # cycle
            finger = (t % 38) > 8
        if finger and not was_on:
            bpm_f = random.choice([62, 71, 78, 88, 104])
        was_on = finger
        beat = 0
        if finger:
            bpm_f += random.uniform(-0.15, 0.15) + (72 - bpm_f) * 0.0005
            phase += bpm_f / 60.0 * 0.02
            if phase >= 1:
                phase -= 1
                beat = 1
                beat_ms = int(t * 1000) & 0xFFFFFFFF
            sig = 512 + 90 * (_g(phase, 0.18, 0.08) + 0.35 * _g(phase, 0.5, 0.09)) + random.uniform(-3, 3)
        else:
            sig = 512
        hub.data(int(sig), beat, beat_ms)
        hub.raw(f"H,{int(sig)},{beat},{beat_ms}")
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
            if path == "/serial":
                return self.serial_stream()
            if path == "/api/status":
                body = json.dumps(hub.status).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            return super().do_GET()

        def serial_stream(self):
            """Plain-text copy of every line the Arduino sends (used by bnts-monitor). Local to the Pi unless --host is changed."""
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Connection", "close")
            self.end_headers()
            q = hub.raw_subscribe()
            try:
                while True:
                    try:
                        self.wfile.write(q.get(timeout=15).encode("utf-8", "replace") + b"\n")
                    except queue.Empty:
                        self.wfile.write(b"# (nothing received from the Arduino in the last 15 s - still listening)\n")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            finally:
                hub.raw_unsubscribe(q)

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
    hub.status["version"] = web_version()
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
