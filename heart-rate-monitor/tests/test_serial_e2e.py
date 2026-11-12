"""End-to-end: fake Arduino -> (pseudo serial port) -> server.py -> browser stream, check-arduino.sh and bnts-monitor.

A. the real firmware's output (H,... lines with the stage field)   -> data flows, stage is passed on, check-arduino.sh OK
B. a TEST sketch's text output (wrong program loaded)               -> flagged as wrong_program, check-arduino.sh says so
C. the Arduino sends nothing                                        -> connected but nothing arriving
D. bnts-monitor (Serial Monitor over SSH) while the display runs    -> shows the Arduino's lines without stopping the display
E. older firmware without the stage field                           -> still works (no stage sent on)
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

def stage_of(i):
    return 0 if i <= 10 else (1 if i <= 150 else 2)

# ---------------------------------------------------------------- A: the real firmware
print("A. real firmware output (with the stage field)")
def real_firmware(master, should_stop):
    os.write(master, b"# BNTS heart rate v2 (digital sensor mode, pin D2)\r\n")
    os.write(master, b"garbage line\r\nH,12,x,0,1\r\n")                  # must be ignored
    i = 0
    while not should_stop():
        beat = 1 if i % 40 == 0 else 0
        bpm = 72 if i > 150 else 0
        os.write(master, f"H,{500+i%50},{bpm},{beat},100,{stage_of(i)},{min(4, i//15) if i > 10 else 0}\r\n".encode())
        if i % 25 == 0:
            os.write(master, f"# [{i//50} s] Update: 72 BPM\r\n".encode())
        i += 1; time.sleep(0.02)
HTTP = 8791
done = start(HTTP, real_firmware); time.sleep(1.5)
got = []
req = urllib.request.urlopen(f"http://127.0.0.1:{HTTP}/events", timeout=5)
t0 = time.time()
while time.time() - t0 < 3.5 and len(got) < 250:
    line = req.readline().decode().strip()
    if line.startswith("data:"): got.append(json.loads(line[5:]))
h = [m for m in got if m["type"] == "h"]; st = [m for m in got if m["type"] == "status"]
check(len(h) >= 40, f"{len(h)} data messages in ~3.5 s (expect ~50/s)")
check(all(1 <= m["s"] <= 1023 for m in h) and any(m["b"] == 72 for m in h), "bpm 72 reaches the browser stream")
check(any(m["x"] == 1 for m in h), "beat flag is passed through")
check(not any(m["s"] == 12 for m in h), "garbage lines ignored")
check(all("g" in m for m in h) and {m["g"] for m in h} >= {1, 2}, f"stage field passed on (seen: {sorted({m.get('g') for m in h})})")
check(all("p" in m for m in h) and max(m["p"] for m in h) == 4, f"progress field passed on (max seen: {max(m.get('p', -1) for m in h)})")
check(st and st[0]["connected"] is True, f"status says connected ({st[0]['port'] if st else None})")
check("BNTS Heart Rate" in urllib.request.urlopen(f"http://127.0.0.1:{HTTP}/").read().decode(), "index.html is served")
time.sleep(1.2)
s = status(HTTP)
check(s["receiving"] is True and s["wrong_program"] is False and "version" in s, f"status: receiving, not flagged as wrong program, version {s.get('version')}")
rc, out = run_check(HTTP)
check(rc == 0 and "receiving heart rate data" in out, "check-arduino.sh says the Pi is receiving data (exit 0)")

# ---------------------------------------------------------------- D: Serial Monitor over SSH (same server as A)
print("D. bnts-monitor while the display service keeps running")
url = {"BNTS_SERIAL_URL": f"http://127.0.0.1:{HTTP}/serial"}
raw = urllib.request.urlopen(f"http://127.0.0.1:{HTTP}/serial", timeout=5)
lines = []; t0 = time.time()
while time.time() - t0 < 2 and len(lines) < 120: lines.append(raw.readline().decode().strip())
check(sum(l.startswith("H,") for l in lines) >= 30, f"/serial streams the Arduino's data lines ({sum(l.startswith('H,') for l in lines)} in 2 s)")
check(any(l.startswith("# [") for l in lines), "/serial includes the Arduino's '# ...' status messages")
raw.close()
out = monitor([], 4, url)
check("Update: 72 BPM" in out, "default mode shows the Arduino's status messages")
check("bpm=72" in out and "heart rate VERIFIED" in out, "default mode shows decoded live data (bpm and stage)")
check(sum(l.startswith("H,") for l in out.splitlines()) == 0, "default mode doesn't flood the screen with raw data lines")
check(len([l for l in out.splitlines() if "live:" in l]) <= 6, "about one decoded data line per second")
out = monitor(["--all"], 3, url)
check(sum(l.startswith("H,") for l in out.splitlines()) >= 60, f"--all shows every raw line ({sum(l.startswith('H,') for l in out.splitlines())} data lines in 3 s)")
out = monitor(["--data"], 3, url)
check("live:" in out and "Update: 72 BPM" not in out, "--data shows only the decoded data lines")
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
check(s["connected"] and not s["receiving"] and s["wrong_program"] is False, "connected, nothing arriving, not blamed on the wrong program")
rc, out = run_check(HTTP)
check(rc == 1 and "no data is arriving" in out, "check-arduino.sh says no data is arriving (exit 1)")
done()

# ---------------------------------------------------------------- E: older firmware (5 fields)
print("E. older firmware that doesn't send the stage field")
def old_firmware(master, should_stop):
    i = 0
    while not should_stop():
        os.write(master, f"H,{500+i%50},{72 if i>10 else 0},{1 if i%40==0 else 0},100\r\n".encode()); i += 1; time.sleep(0.02)
HTTP = 8794
done = start(HTTP, old_firmware); time.sleep(1.5)
got = []
req = urllib.request.urlopen(f"http://127.0.0.1:{HTTP}/events", timeout=5); t0 = time.time()
while time.time() - t0 < 2 and len(got) < 100:
    line = req.readline().decode().strip()
    if line.startswith("data:"): got.append(json.loads(line[5:]))
h = [m for m in got if m["type"] == "h"]
check(len(h) >= 30 and any(m["b"] == 72 for m in h), "5-field lines still parse and reach the display")
check(not any("g" in m or "p" in m for m in h), "no stage or progress is invented for them (the page works it out itself)")
done()

# ---------------------------------------------------------------- F: no display service, read the port directly
print("F. bnts-monitor with the display service stopped")
def direct(master, should_stop):
    while not should_stop():
        os.write(master, b"# [5 s] Finger confirmed. READING: 70 BPM\r\n"); os.write(master, b"H,520,70,1,100,2\r\n"); time.sleep(0.1)
master, slave = pty.openpty(); flag = {"stop": False}
threading.Thread(target=direct, args=(master, lambda: flag["stop"]), daemon=True).start()
out = monitor([], 4, {"BNTS_SERIAL_URL": "http://127.0.0.1:8899/serial", "BNTS_PORT": os.ttyname(slave)})   # nothing listens on 8899
flag["stop"] = True
check("display service isn't running" in out and "READING: 70 BPM" in out, "falls back to reading the Arduino's port directly")

print("ALL PASSED" if ok else "FAILED"); sys.exit(0 if ok else 1)
