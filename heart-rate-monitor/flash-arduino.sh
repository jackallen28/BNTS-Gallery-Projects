#!/usr/bin/env bash
# Flash the Arduino FROM the Raspberry Pi (yes, the Pi can do it).
#
#   ./flash-arduino.sh                              # Arduino Uno on the first USB port found
#   BOARD=arduino:avr:nano:cpu=atmega328old ./flash-arduino.sh
#   PORT=/dev/ttyUSB0 ./flash-arduino.sh
#
# Common BOARD values:  arduino:avr:uno   arduino:avr:nano   arduino:avr:nano:cpu=atmega328old
#                       arduino:avr:mega  arduino:avr:leonardo
set -euo pipefail
cd "$(dirname "$0")"

BOARD="${BOARD:-arduino:avr:uno}"
SKETCH="arduino/bnts_heartrate"
export PATH="$HOME/.local/bin:$PATH"

say() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }

if ! command -v arduino-cli >/dev/null 2>&1; then
  say "Installing arduino-cli (official installer from arduino.cc's GitHub)"
  mkdir -p "$HOME/.local/bin"
  curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR="$HOME/.local/bin" sh
fi

say "Installing the Arduino core for $BOARD"
CORE="$(echo "$BOARD" | cut -d: -f1,2)"
arduino-cli core update-index
arduino-cli core install "$CORE"

PORT="${PORT:-}"
if [ -z "$PORT" ]; then
  for p in /dev/serial/by-id/* /dev/ttyACM* /dev/ttyUSB*; do [ -e "$p" ] && { PORT="$p"; break; }; done
fi
[ -n "$PORT" ] || { echo "No Arduino found. Plug it in (data-capable USB cable) or set PORT=/dev/ttyACM0" >&2; exit 1; }
echo "Using port: $PORT"

say "Compiling"
arduino-cli compile --fqbn "$BOARD" "$SKETCH"

# The display service holds the serial port open, so pause it while uploading.
RESTART=0
if systemctl is-active --quiet bnts-heart.service 2>/dev/null; then
  sudo systemctl stop bnts-heart.service; RESTART=1
fi
trap '[ "$RESTART" = 1 ] && sudo systemctl start bnts-heart.service' EXIT

say "Uploading"
arduino-cli upload -p "$PORT" --fqbn "$BOARD" "$SKETCH"
say "Flashed. The heart rate service restarts automatically."
