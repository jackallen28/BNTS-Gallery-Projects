"""End-to-end: fake Arduino -> (pseudo serial port) -> server.py -> browser stream, check-arduino.sh and bnts-monitor.

A. the real firmware's output (H,signal,beat,beatMs)               -> data flows with the beat clock; check-arduino.sh OK
B. a TEST sketch's text output (wrong program loaded)               -> flagged as wrong_program, check-arduino.sh says so
C. the Arduino sends nothing                                        -> connected but nothing arriving
D. bnts-monitor (Serial Monitor over SSH) while the display runs    -> shows beats without stopping the display
E. OLD firmware (the previous data format)                          -> recognised: "flash the new firmware"
F. bnts-monitor when the display service is NOT running             -> reads the port directly

Run:  python3 tests/test_serial_e2e.py        (needs pyserial)"""
import json, os, pty, subprocess, sys, threading, time, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
ok = True
def check(c, msg):
    global ok; ok &= bool(c); print(("  ok:   " if c else "  FAIL: ") + msg)

def start(http, writer):
    """Start server.py on a pseudo serial port; `writer(master, should_stop)` plays the Arduino."""
    master, slave = pty.openpty()
    port = os.ttyname(slave)
    srv = subprocess.Popen([sys.executable, os.path.join(ROOT, "pi", "server.py"),
                            "--port", port, "--http-port", str(http)], stderr=subprocess.DEVNULL)
    flag = {"stop": False}
    threading.Thread(target=writer, args=(master, lambda: flag["stop"]), daemon=True).start()
    def finish():
        flag["stop"] = True; srv.terminate(); srv.wait(timeout=5)
    finish.port = port
    return finish

def status(http):
    return json.loads(urllib.request.urlopen(f"http://127.0.0.1:{http}/api/status", timeout=3).read())

def run_check(http):
    env = dict(os.environ, BNTS_STATUS_URL=f"http://127.0.0.1:{http}/api/status")
    p = subprocess.run(["bash", os.path.join(ROOT, "check-arduino.sh")], env=env, capture_output=True, text=True, timeout=20)
    return p.returncode, p.stdout

def monitor(args, seconds, env_extra):
    """Run bnts-monitor (pi/serial-monitor.sh) for a few seconds and return what it printed."""
    env = dict(os.environ, **env_extra)
    p = subprocess.run(["timeout", str(seconds), "bash", os.path.join(ROOT, "pi", "serial-monitor.sh")] + args,
                       env=env, capture_output=True, text=True)
    return p.stdout

def new_firmware(master, should_stop):
    """What the real firmware sends: 50 lines a second, a beat every 833 ms (72 BPM), the beat clock in milliseconds."""
    os.write(master, b"# BNTS heart rate v3 (digital sensor mode, pin D2): reports every beat\r\n")
    os.write(master, b"garbage line\r\nH,12,x,0\r\nH,5000,1,0\r\n")                   # all must be ignored
    i = 0; bm = 0
    while not should_stop():
        ms = i * 20
        beat = 1 if ms % 833 < 20 else 0
        if beat: bm = 48000 + ms
        os.write(master, f"H,{500 + i % 50},{beat},{bm}\r\n".encode())
        if i % 50 == 0:
            os.write(master, f"# [{i // 50} s] still running\r\n".encode())
        i += 1; time.sleep(0.02)

# ---------------------------------------------------------------- A: the real firmware
print("A. real firmware output")
HTTP = 8791
done = start(HTTP, new_firmware); time.sleep(1.5)
got = []
req = urllib.request.urlopen(f"http://127.0.0.1:{HTTP}/events", timeout=5)
t0 = time.time()
while time.time() - t0 < 4 and len(got) < 300:
    line = req.readline().decode().strip()
    if line.startswith("data:"): got.append(json.loads(line[5:]))
h = [m for m in got if m["type"] == "h"]; st = [m for m in got if m["type"] == "status"]
check(len(h) >= 40, f"{len(h)} data messages in ~4 s (expect ~50/s)")
check(all(0 <= m["s"] <= 1023 for m in h) and not any(m["s"] in (12, 5000) for m in h), "garbage and out-of-range lines ignored")
check(all(set(m) >= {"s", "x", "bm", "t"} for m in h), "every message carries signal, beat flag, the Arduino's beat clock and the Pi's read time")
changes = sum(1 for a, b in zip(h, h[1:]) if a["bm"] != b["bm"])
flags = sum(m["x"] for m in h)
check(changes >= 3 and abs(changes - flags) <= 1, f"beat clock changed {changes} times, {flags} beat flags: one per beat")
check(all(b["t"] >= a["t"] for a, b in zip(h, h[1:])), "read times only go forward")
check(st and st[0]["connected"] is True, f"status says connected ({st[0]['port'] if st else None})")
check("BNTS Heart Rate" in urllib.request.urlopen(f"http://127.0.0.1:{HTTP}/").read().decode(), "index.html is served")
time.sleep(1.2)
s = status(HTTP)
check(s["receiving"] is True and s["wrong_program"] is False and s["old_firmware"] is False and "version" in s, f"status: receiving, version {s.get('version')}")
rc, out = run_check(HTTP)
check(rc == 0 and "receiving heart rate data" in out and "Press the screen" in out, "check-arduino.sh says the Pi is receiving data (exit 0)")

# ---------------------------------------------------------------- D: Serial Monitor over SSH (same server as A)
print("D. bnts-monitor while the display service keeps running")
url = {"BNTS_SERIAL_URL": f"http://127.0.0.1:{HTTP}/serial"}
raw = urllib.request.urlopen(f"http://127.0.0.1:{HTTP}/serial", timeout=5)
lines = []; t0 = time.time()
while time.time() - t0 < 2 and len(lines) < 120: lines.append(raw.readline().decode().strip())
check(sum(l.startswith("H,") for l in lines) >= 30, f"/serial streams the Arduino's data lines ({sum(l.startswith('H,') for l in lines)} in 2 s)")
raw.close()
out = monitor([], 5, url)
beat_lines = [l for l in out.splitlines() if l.strip().startswith("beat ")]
import re
gaps = [int(re.search(r"gap (\d+) ms", l).group(1)) for l in beat_lines if "gap" in l]
check(len(beat_lines) >= 3 and gaps and all(815 <= g <= 845 for g in gaps),     # the fake Arduino only has a 20 ms clock grid
      f"default mode prints a line per beat with its gap and rate ({len(beat_lines)} beats, e.g. {beat_lines[1].strip() if len(beat_lines) > 1 else '-'})")
check("# [" in out, "default mode shows the Arduino's '# ...' messages")
check(sum(l.startswith("H,") for l in out.splitlines()) == 0, "default mode doesn't flood the screen with raw data lines")
out = monitor(["--all"], 3, url)
check(sum(l.startswith("H,") for l in out.splitlines()) >= 60, f"--all shows every raw line ({sum(l.startswith('H,') for l in out.splitlines())} data lines in 3 s)")
out = monitor(["--data"], 4, url)
check("beat " in out and "# [" not in out, "--data shows only the beat lines")
check(status(HTTP)["receiving"] is True, "the display kept receiving data the whole time the monitor was attached")
done()

# ---------------------------------------------------------------- B: a test sketch is loaded
print("B. a test sketch is on the Arduino (readable text, no H lines)")
def test_sketch(master, should_stop):
    os.write(master, b"BNTS staged heart rate test (sensor on D2, Serial Monitor at 115200 baud)\r\n")
    n = 0
    while not should_stop():
        os.write(master, f"[ {n:3d} s] Waiting for a finger...  (pin D2 is LOW)\r\n".encode()); n += 1; time.sleep(0.3)
HTTP = 8792
done = start(HTTP, test_sketch); time.sleep(5)
s = status(HTTP)
check(s["connected"] and not s["receiving"] and s["wrong_program"] is True, "flagged: connected, not receiving, wrong_program")
check("Waiting for a finger" in s["other_line"], f"shows what the Arduino is sending: {s['other_line']!r}")
rc, out = run_check(HTTP)
check(rc == 1 and "TEST sketch" in out and "./flash-arduino.sh" in out, "check-arduino.sh names the problem and the fix (exit 1)")
out = monitor([], 3, {"BNTS_SERIAL_URL": f"http://127.0.0.1:{HTTP}/serial"})
check("Waiting for a finger" in out, "bnts-monitor shows the test sketch's text, so you can see what is loaded")
done()

# ---------------------------------------------------------------- C: silence
print("C. the Arduino sends nothing")
HTTP = 8793
done = start(HTTP, lambda master, should_stop: time.sleep(6)); time.sleep(4.5)
s = status(HTTP)
check(s["connected"] and not s["receiving"] and s["wrong_program"] is False and s["old_firmware"] is False, "connected, nothing arriving, not blamed on the wrong program")
rc, out = run_check(HTTP)
check(rc == 1 and "no data is arriving" in out, "check-arduino.sh says no data is arriving (exit 1)")
done()

# ---------------------------------------------------------------- E: old firmware
print("E. OLD firmware (the previous data format)")
def old_firmware(master, should_stop):
    i = 0
    while not should_stop():
        os.write(master, (f"H,{500+i%50},{72 if i>10 else 0},{1 if i%40==0 else 0},100,2,4\r\n" if i % 2 else f"H,{500+i%50},72,0,100\r\n").encode())
        i += 1; time.sleep(0.02)
HTTP = 8794
done = start(HTTP, old_firmware); time.sleep(5)
s = status(HTTP)
check(s["old_firmware"] is True and s["receiving"] is False and s["wrong_program"] is False, "recognised as old firmware (not 'receiving', not 'wrong program')")
rc, out = run_check(HTTP)
check(rc == 1 and "OLDER firmware" in out and "./flash-arduino.sh" in out, "check-arduino.sh says to flash the new firmware (exit 1)")
out = monitor([], 3, {"BNTS_SERIAL_URL": f"http://127.0.0.1:{HTTP}/serial"})
check("OLD firmware" in out, "bnts-monitor says so too")
done()

# ---------------------------------------------------------------- F: no display service, read the port directly
print("F. bnts-monitor with the display service stopped")
def direct(master, should_stop):
    bm = 1000
    while not should_stop():
        bm += 800
        os.write(master, b"# [5 s] hello\r\n"); os.write(master, f"H,520,1,{bm}\r\n".encode()); time.sleep(0.2)
master, slave = pty.openpty(); flag = {"stop": False}
threading.Thread(target=direct, args=(master, lambda: flag["stop"]), daemon=True).start()
out = monitor([], 4, {"BNTS_SERIAL_URL": "http://127.0.0.1:8899/serial", "BNTS_PORT": os.ttyname(slave)})   # nothing listens on 8899
flag["stop"] = True
check("display service isn't running" in out and "hello" in out and "gap 800 ms = 75 BPM" in out, "falls back to reading the Arduino's port directly (messages and beats)")

print("ALL PASSED" if ok else "FAILED"); sys.exit(0 if ok else 1)
