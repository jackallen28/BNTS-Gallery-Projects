#!/usr/bin/env bash
# Is the Arduino plugged into the Pi and sending heart rate data the display understands?
#
#   ./check-arduino.sh          quick check (asks the running display service what it is seeing)
#   ./check-arduino.sh --raw    pauses the display service for ~6 s and shows the raw lines coming from the Arduino
#
# Exit code 0 = the Pi is receiving data from the Arduino.
set -u
URL="${BNTS_STATUS_URL:-http://127.0.0.1:8080/api/status}"

good() { printf '  \033[1;32mOK\033[0m       %s\n' "$*"; }
bad()  { printf '  \033[1;31mPROBLEM\033[0m  %s\n' "$*"; }
note() { printf '            %s\n' "$*"; }

if [ "${1:-}" = "--raw" ]; then
  PORT="$(ls /dev/serial/by-id/* /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1)"
  [ -n "$PORT" ] || { bad "No Arduino found on USB."; exit 1; }
  RESTART=0
  if systemctl is-active --quiet bnts-heart.service 2>/dev/null; then sudo systemctl stop bnts-heart.service; RESTART=1; fi
  trap '[ "$RESTART" = 1 ] && sudo systemctl start bnts-heart.service' EXIT
  echo "Listening to $PORT for 6 seconds (the Arduino restarts when the port opens, so the first 2 s may be quiet)..."
  python3 - "$PORT" <<'PY'
import sys, time
try:
    import serial
except ImportError:
    sys.exit("pyserial is missing: sudo apt install python3-serial")
ser = serial.Serial(sys.argv[1], 115200, timeout=0.5)
t0, shown, h, other = time.time(), 0, 0, 0
while time.time() - t0 < 6:
    raw = ser.readline()
    if not raw:
        continue
    text = raw.decode("ascii", "replace").strip()
    if text.startswith("H,"):
        h += 1
    elif text:
        other += 1
    if shown < 12 and text:
        print("   ", text[:100]); shown += 1
print()
if h:
    print(f"  Heart rate data lines (H,...): {h} in 6 s (the real firmware sends about 50 per second).")
elif other:
    print("  The Arduino IS transmitting, but not in the heart rate format: a TEST sketch is loaded on it.")
    print("  Fix: ./flash-arduino.sh   (this puts the real firmware back)")
else:
    print("  Nothing received. Check the USB cable, or flash the firmware with ./flash-arduino.sh")
PY
  exit 0
fi

echo "Checking the Arduino connection..."

ports="$(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | tr '\n' ' ')"
if [ -n "$ports" ]; then
  good "Arduino found on USB: $ports"
else
  bad "No Arduino found on USB (no /dev/ttyACM* or /dev/ttyUSB*)."
  note "Try another USB cable (it must carry data, not only power) or another USB port on the Pi."
fi

if command -v systemctl >/dev/null 2>&1; then
  if systemctl is-active --quiet bnts-heart.service; then
    good "display service (bnts-heart) is running"
  else
    bad "display service (bnts-heart) is not running."
    note "Start it:  sudo systemctl start bnts-heart     See why:  journalctl -u bnts-heart -n 20"
  fi
fi

json="$(curl -sf -m 4 "$URL" 2>/dev/null)" || { bad "the display service didn't answer on $URL"; exit 1; }

python3 - "$json" <<'PY'
import json, sys
st = json.loads(sys.argv[1])
G, R, N = "\033[1;32mOK\033[0m      ", "\033[1;31mPROBLEM\033[0m ", "           "
if not st.get("connected"):
    print(f"  {R} The display service can't open the Arduino's serial port.")
    print(f"{N} Unplug and re-plug the Arduino, then run this again.")
    sys.exit(1)
print(f"  {G} the display service has the Arduino's port open ({st.get('port')})")
if st.get("receiving"):
    print(f"  {G} the Pi is receiving heart rate data from the Arduino.")
    print(f"{N} Rest a finger on the sensor: after about 3 seconds the live screen appears.")
    sys.exit(0)
if st.get("wrong_program"):
    print(f"  {R} The Arduino is transmitting, but not the heart rate data the display needs.")
    print(f"{N} It is sending:  {st.get('other_line')!r}")
    print(f"{N} A TEST sketch is probably loaded on it. Put the real firmware back:  ./flash-arduino.sh")
    sys.exit(1)
print(f"  {R} Connected, but no data is arriving from the Arduino.")
print(f"{N} If you just plugged it in, wait 5 seconds and run this again (the Arduino restarts when the port opens).")
print(f"{N} See the raw lines:  ./check-arduino.sh --raw        Reload the firmware:  ./flash-arduino.sh")
sys.exit(1)
PY
