#!/usr/bin/env bash
# Builds the REAL firmware for an Arduino Uno exactly as the Arduino IDE does (avr-gcc -Os) and runs it in QEMU's
# AVR emulator, then checks what it sends over its serial port.
#
# QEMU has no Timer0 and no way to drive a pin, so for this test only the clock and the sensor are swapped for
# software ones (a clock that advances 1 ms per loop pass, and a sensor with the same 5 ms edge chatter as the real
# one). The detector, the state machine and the printing code are the unmodified firmware.
#
#   sudo apt install gcc-avr avr-libc binutils-avr arduino-core-avr qemu-system-misc
#   bash tests/qemu_uno.sh
set -eu
HERE="$(cd "$(dirname "$0")/.." && pwd)"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
mkdir -p "$W/fw"; cp "$HERE"/arduino/bnts_heartrate/*.h "$W/fw/"
python3 - "$HERE" "$W/fw/fw.ino" <<'PY'
import sys
s = open(sys.argv[1] + "/arduino/bnts_heartrate/bnts_heartrate.ino").read()
fake = '''
volatile uint32_t fakeMs = 0;
#define millis() fakeMs
#define micros() (fakeMs * 1000UL)
bool fakeSensor(uint32_t ms) {
  uint32_t t0, period, width;
  if (ms < 3000) return false;
  if (ms < 26000)      { t0 = 3000;  period = 833; width = 250; }   // 72 BPM
  else if (ms < 29300) { t0 = 26000; period = 536; width = 180; }   // 3.3 s glitch at 112 BPM
  else if (ms < 40000) { t0 = 29300; period = 833; width = 250; }   // back to 72 BPM
  else return false;                                                // finger removed
  uint32_t ph = (ms - t0) % period; bool toggle = ((ph / 5) % 2) == 0;
  if (ph < 15) return toggle;
  if (ph < width) return true;
  if (ph < width + 15) return !toggle;
  return false;
}
'''
assert "const long BAUD = 115200;" in s and "detector.update(digitalRead(SENSOR_PIN), millis());" in s and "void loop() {\n  uint32_t now = micros();" in s
s = s.replace("const long BAUD = 115200;", "const long BAUD = 115200;\n" + fake, 1)
s = s.replace("detector.update(digitalRead(SENSOR_PIN), millis());", "detector.update(fakeSensor(millis()), millis());")
s = s.replace("void loop() {\n  uint32_t now = micros();", "void loop() {\n  fakeMs++;\n  uint32_t now = micros();")
open(sys.argv[2], "w").write(s)
PY
echo "Building the firmware for an Uno (-Os)..."
OUT_ELF="$W/fw.elf" bash "$HERE/tests/compile_uno.sh" "$W/fw"
echo "Running it in QEMU (about 30-60 s)..."
timeout 150 qemu-system-avr -M uno -bios "$W/fw.elf" -display none -serial stdio 2>/dev/null | tr -d '\r' > "$W/all.txt" || true
grep -a '^#' "$W/all.txt" > "$W/out.txt" || true
cat "$W/out.txt" | sed 's/^/    /'
fails=0
check() { if grep -aq -- "$2" "$W/out.txt"; then echo "  ok:   $1"; else echo "  FAIL: $1"; fails=$((fails+1)); fi; }
check "finger detected"                       "Something detected"
check "finger confirmed with a 72 BPM reading" "Finger confirmed. READING: 72 BPM"
check "glitch skipped, then dropped"           "was a glitch - ignored"
check "finger removal noticed"                 "Finger removed"
checkh() { if grep -aq -- "$2" "$W/all.txt"; then echo "  ok:   $1"; else echo "  FAIL: $1"; fails=$((fails+1)); fi; }
checkh "stage 1 (finger detected) sent before any reading" '^H,[0-9]*,0,[01],100,1,[0-9]*$'
checkh "real beats flagged while verifying"                '^H,[0-9]*,0,1,100,1,[0-9]*$'
checkh "progress reaches 4 on the way to verified"         '^H,[0-9]*,[0-9]*,[01],100,[12],4$'
checkh "stage 2 (verified) sent with the reading"          '^H,[0-9]*,72,[01],100,2,4$'
checkh "all lines well formed (7 fields)"                  '^H,[0-9]*,[0-9]*,[01],[0-9]*,[012],[0-4]$'
bad=$(grep -a '^H,' "$W/all.txt" | head -n -1 | grep -avc '^H,[0-9]*,[0-9]*,[01],[0-9]*,[012],[0-4]$' || true)
if [ "${bad:-0}" = 0 ]; then echo "  ok:   no malformed data lines"; else echo "  FAIL: $bad malformed data lines"; fails=$((fails+1)); fi
[ $fails = 0 ] && echo "ALL PASSED" || echo "FAILED ($fails)"
exit $fails
