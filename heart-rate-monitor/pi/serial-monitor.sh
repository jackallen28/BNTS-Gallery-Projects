#!/usr/bin/env bash
# bnts-monitor - watch what the Arduino is sending, like the Arduino IDE's Serial Monitor, from an SSH session.
#
#   bnts-monitor           the Arduino's status messages, plus one decoded data line per second   (Ctrl+C to quit)
#   bnts-monitor --all     every single line exactly as the Arduino sends it (about 50 data lines a second)
#   bnts-monitor --data    only the data lines, decoded, one per second
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

# Turn the raw stream into what is shown. H,<signal>,<bpm>,<beat>,<amplitude>[,<stage>[,<progress>]]
# (Python, not awk: the awk on Raspberry Pi OS reads piped input in big blocks, so lines would arrive seconds late.)
filter() {
  if [ "$MODE" = all ]; then cat; return; fi
  python3 -u -c '
import sys
mode = sys.argv[1]
names = ["no finger", "finger detected (verifying)", "heart rate VERIFIED"]
n = 0
for line in sys.stdin:
    line = line.rstrip("\r\n")
    if line.startswith("H,"):
        n += 1
        if n % 50 == 1:                                   # about once a second
            f = line.split(",")
            if len(f) >= 5:
                stage = names[int(f[5])] if len(f) > 5 and f[5].isdigit() and int(f[5]) < 3 else "-"
                progress = f[6] if len(f) > 6 else "-"
                print(f"  live: bpm={f[2]:<3} signal={f[1]:<4} beat={f[3]} amplitude={f[4]:<3} stage={stage}  progress={progress}", flush=True)
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
