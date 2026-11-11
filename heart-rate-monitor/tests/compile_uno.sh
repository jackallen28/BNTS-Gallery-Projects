#!/usr/bin/env bash
# Compiles an Arduino sketch for an Uno with the real AVR toolchain and prints its flash/RAM use.
# Needs (Debian/Ubuntu):  sudo apt install gcc-avr avr-libc binutils-avr arduino-core-avr
#   bash tests/compile_uno.sh arduino/bnts_heartrate            # the firmware
#   bash tests/compile_uno.sh arduino/bnts_heartrate 0          # ...in analog mode (SENSOR_MODE_DIGITAL 0)
set -eu
SKETCH_DIR="$(cd "$1" && pwd)"; NAME="$(basename "$SKETCH_DIR")"; MODE="${2:-}"
CORE=/usr/share/arduino/hardware/arduino/avr
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
FLAGS="${OPT:--Os} -mmcu=atmega328p -DF_CPU=16000000L -DARDUINO=10819 -DARDUINO_AVR_UNO -DARDUINO_ARCH_AVR \
  -ffunction-sections -fdata-sections -w -I$CORE/cores/arduino -I$CORE/variants/standard"

# an .ino is a .cpp with Arduino.h on top
{ echo '#include <Arduino.h>'; cat "$SKETCH_DIR/$NAME.ino"; } > "$OUT/sketch.cpp"
[ -n "$MODE" ] && sed -i "s/#define SENSOR_MODE_DIGITAL [01]/#define SENSOR_MODE_DIGITAL $MODE/" "$OUT/sketch.cpp"

avr-g++ $FLAGS -std=gnu++11 -fno-exceptions -fno-threadsafe-statics -I"$SKETCH_DIR" -c "$OUT/sketch.cpp" -o "$OUT/sketch.o"
for f in "$CORE"/cores/arduino/*.c;   do avr-gcc $FLAGS -std=gnu11 -c "$f" -o "$OUT/$(basename "$f").o"; done
for f in "$CORE"/cores/arduino/*.cpp; do avr-g++ $FLAGS -std=gnu++11 -fno-exceptions -fno-threadsafe-statics -c "$f" -o "$OUT/$(basename "$f").o"; done
avr-gcc -Os -mmcu=atmega328p -Wl,--gc-sections -o "$OUT/fw.elf" "$OUT"/*.o -lm
[ -n "${OUT_ELF:-}" ] && cp "$OUT/fw.elf" "$OUT_ELF"
avr-size "$OUT/fw.elf" | awk 'NR==2 { printf "flash %d of 32256 bytes (%.0f%%), RAM %d of 2048 bytes (%.0f%%)\n", $1+$2, ($1+$2)/322.56, $2+$3, ($2+$3)/20.48 }'
