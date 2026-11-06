"""End-to-end: fake Arduino -> (pseudo serial port) -> server.py -> /events stream.
Run:  python3 tests/test_serial_e2e.py        (needs pyserial)"""
import json, os, pty, subprocess, sys, threading, time, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
master, slave = pty.openpty()
port = os.ttyname(slave)
HTTP = 8791
srv = subprocess.Popen([sys.executable, os.path.join(HERE, "..", "pi", "server.py"),
                        "--port", port, "--http-port", str(HTTP)], stderr=subprocess.PIPE)
stop = False
def arduino():
    os.write(master, b"# BNTS heart rate v1\r\n")
    os.write(master, b"garbage line\r\nH,12,x,0,1\r\n")             # must be ignored
    i = 0
    while not stop:
        beat = 1 if i % 40 == 0 else 0
        os.write(master, f"H,{500+i%50},{72 if i>10 else 0},{beat},90\r\n".encode())
        i += 1; time.sleep(0.02)
threading.Thread(target=arduino, daemon=True).start()
time.sleep(1.5)

ok = True
def check(c, msg):
    global ok; ok &= bool(c); print(("  ok:   " if c else "  FAIL: ") + msg)

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
page = urllib.request.urlopen(f"http://127.0.0.1:{HTTP}/").read().decode()
check("BNTS Heart Rate" in page, "index.html is served")
stop = True; srv.terminate()
print("ALL PASSED" if ok else "FAILED"); sys.exit(0 if ok else 1)
