"""End-to-end: fake Arduino -> (pseudo serial port) -> server.py -> browser stream, plus check-arduino.sh.

Scenario A: the real firmware's output (H,... lines)          -> data flows, check-arduino.sh says OK.
Scenario B: a TEST sketch's text output (wrong program loaded) -> flagged as wrong_program, check-arduino.sh says so.
Scenario C: the Arduino sends nothing                          -> connected but nothing arriving.

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
    srv = subprocess.Popen([sys.executable, os.path.join(ROOT, "pi", "server.py"),
                            "--port", os.ttyname(slave), "--http-port", str(http)], stderr=subprocess.DEVNULL)
    flag = {"stop": False}
    threading.Thread(target=writer, args=(master, lambda: flag["stop"]), daemon=True).start()
    def finish():
        flag["stop"] = True; srv.terminate(); srv.wait(timeout=5)
    return finish

def status(http):
    return json.loads(urllib.request.urlopen(f"http://127.0.0.1:{http}/api/status", timeout=3).read())

def run_check(http):
    env = dict(os.environ, BNTS_STATUS_URL=f"http://127.0.0.1:{http}/api/status")
    p = subprocess.run(["bash", os.path.join(ROOT, "check-arduino.sh")], env=env, capture_output=True, text=True, timeout=20)
    return p.returncode, p.stdout

# ---------------------------------------------------------------- A: the real firmware
print("A. real firmware output")
def real_firmware(master, should_stop):
    os.write(master, b"# BNTS heart rate v2 (digital sensor mode, pin D2)\r\n")
    os.write(master, b"garbage line\r\nH,12,x,0,1\r\n")                  # must be ignored
    i = 0
    while not should_stop():
        beat = 1 if i % 40 == 0 else 0
        os.write(master, f"H,{500+i%50},{72 if i>10 else 0},{beat},100\r\n".encode())
        i += 1; time.sleep(0.02)
HTTP = 8791
done = start(HTTP, real_firmware); time.sleep(1.5)
got = []
req = urllib.request.urlopen(f"http://127.0.0.1:{HTTP}/events", timeout=5)
t0 = time.time()
while time.time() - t0 < 2.5 and len(got) < 200:
    line = req.readline().decode().strip()
    if line.startswith("data:"): got.append(json.loads(line[5:]))
h = [m for m in got if m["type"] == "h"]; st = [m for m in got if m["type"] == "status"]
check(len(h) >= 40, f"{len(h)} data messages in ~2.5 s (expect ~50/s)")
check(all(1 <= m["s"] <= 1023 for m in h) and any(m["b"] == 72 for m in h), "bpm 72 reaches the browser stream")
check(any(m["x"] == 1 for m in h), "beat flag is passed through")
check(not any(m["s"] == 12 for m in h), "garbage lines ignored")
check(st and st[0]["connected"] is True, f"status says connected ({st[0]['port'] if st else None})")
check("BNTS Heart Rate" in urllib.request.urlopen(f"http://127.0.0.1:{HTTP}/").read().decode(), "index.html is served")
time.sleep(1.2)
s = status(HTTP)
check(s["receiving"] is True and s["wrong_program"] is False and "version" in s, f"status: receiving, not flagged as wrong program, version {s.get('version')}")
rc, out = run_check(HTTP)
check(rc == 0 and "receiving heart rate data" in out, "check-arduino.sh says the Pi is receiving data (exit 0)")
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

print("ALL PASSED" if ok else "FAILED"); sys.exit(0 if ok else 1)
