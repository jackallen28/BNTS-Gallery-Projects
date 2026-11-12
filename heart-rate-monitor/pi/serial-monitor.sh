#!/usr/bin/env bash
# bnts-monitor - watch what the Arduino is sending, like the Arduino IDE's Serial Monitor, from an SSH session.
#
#   bnts-monitor           the Arduino's messages, plus one line for every beat (the gap since the last beat and the rate it works out to)
#   bnts-monitor --all     every single line exactly as the Arduino sends it (about 50 data lines a second)
#   bnts-monitor --data    only the beat lines
#   (Ctrl+C to quit)
#
# The display service already has the Arduino's serial port open, so this asks the service for a copy of what it
# receives (the display keeps running). If the service isn't running, it reads the Arduino's port directly instead.
set -u
URL="${BNTS_SERIAL_URL:-http://127.0.0.1:8080/serial}"
MODE=default
case "${1:-}" in
  "" ) ;;
  --all) MODE=all ;;
  --data) MODE=data ;;
  -h|--help) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
  *) echo "Unknown option '$1'. Try: bnts-monitor --help" >&2; exit 2 ;;
esac

# Turn the raw stream into what is shown. Data lines look like  H,<signal>,<beat>,<beatMs>.
# (Python, not awk: the awk on Raspberry Pi OS reads piped input in big blocks, so lines would arrive seconds late.)
filter() {
  if [ "$MODE" = all ]; then cat; return; fi
  python3 -u -c '
import sys
mode = sys.argv[1]
last = None
count = 0
warned = False
for line in sys.stdin:
    line = line.rstrip("\r\n")
    if line.startswith("H,"):
        f = line.split(",")
        if len(f) == 4 and f[3].isdigit():                  # current firmware: H,signal,beat,beatMs
            bm = int(f[3])
            if last is not None and bm != last:             # the beat clock moved on: a new beat
                gap = (bm - last) & 0xFFFFFFFF
                count += 1
                rate = 60000.0 / gap if gap else 0
                print(f"  beat {count:<3} at {bm} ms   gap {gap} ms = {rate:.0f} BPM", flush=True)
            last = bm
        elif not warned:
            warned = True
            print("  (these data lines are from OLD firmware: flash the new one with ./flash-arduino.sh)", flush=True)
        continue
    if mode == "data":
        continue
    print(line, flush=True)
' "$MODE"
}

if curl -sf -m 3 -o /dev/null "${URL%/serial}/api/status" 2>/dev/null; then      # is the display service answering?
  echo "Showing what the Arduino sends (Ctrl+C to quit). Mode: $MODE"
  curl -sN "$URL" | filter
  exit 0
fi

# The display service isn't answering: read the Arduino directly.
PORT="${BNTS_PORT:-$(ls /dev/serial/by-id/* /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1)}"
[ -n "$PORT" ] || { echo "The display service isn't running and no Arduino was found on USB." >&2; exit 1; }
echo "The display service isn't running, so reading $PORT directly (Ctrl+C to quit). Mode: $MODE"
python3 -u - "$PORT" <<'PY' | filter
import sys
try:
    import serial
except ImportError:
    sys.exit("pyserial is missing: sudo apt install python3-serial")
ser = serial.Serial(sys.argv[1], 115200, timeout=1)
try:
    while True:
        raw = ser.readline()
        if raw:
            print(raw.decode("ascii", "replace").rstrip(), flush=True)
except KeyboardInterrupt:
    pass
PY
