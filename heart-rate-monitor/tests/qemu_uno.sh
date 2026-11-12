#!/usr/bin/env bash
# Builds the REAL firmware for an Arduino Uno exactly as the Arduino IDE does (avr-gcc -Os) and runs it in QEMU's AVR
# emulator, then checks what it sends over its serial port.
#
# QEMU has no Timer0 and no way to drive a pin, so for this test only the clock and the sensor are swapped for
# software ones (a clock that advances 1 ms per loop pass, and a sensor with the same 5 ms edge flicker as the real
# one). The beat code, the LED code and the serial output code are the unmodified firmware.
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
// 24 pulses at 72 BPM (833 ms apart, 250 ms wide) from 3 s, each with 5 ms flicker on both edges, then nothing.
bool fakeSensor(uint32_t ms) {
  if (ms < 3000 || ms >= 22900) return false;
  uint32_t ph = (ms - 3000) % 833; bool toggle = ((ph / 5) % 2) == 0;
  if (ph < 15) return toggle;
  if (ph < 250) return true;
  if (ph < 265) return !toggle;
  return false;
}
'''
for need in ("const long BAUD = 115200;", "sensor.update(digitalRead(SENSOR_PIN), millis());", "void loop() {\n  uint32_t now = micros();"):
    assert need in s, need
s = s.replace("const long BAUD = 115200;", "const long BAUD = 115200;\n" + fake, 1)
s = s.replace("sensor.update(digitalRead(SENSOR_PIN), millis());", "sensor.update(fakeSensor(millis()), millis());")
s = s.replace("void loop() {\n  uint32_t now = micros();",
              "void loop() {\n  fakeMs++;\n  if (fakeMs == 26000) Serial.println(F(\"# TEST-DONE\"));\n  uint32_t now = micros();")
open(sys.argv[2], "w").write(s)
PY
echo "Building the firmware for an Uno (-Os)..."
OUT_ELF="$W/fw.elf" bash "$HERE/tests/compile_uno.sh" "$W/fw"
echo "Running it in QEMU (about 30-90 s)..."
timeout 170 qemu-system-avr -M uno -bios "$W/fw.elf" -display none -serial stdio 2>/dev/null | tr -d '\r' | sed '/# TEST-DONE/q' > "$W/all.txt" || true
grep -a '^#' "$W/all.txt" | sed 's/^/    /'
fails=0
ok()   { echo "  ok:   $1"; }
bad()  { echo "  FAIL: $1"; fails=$((fails+1)); }

grep -aq '^# BNTS heart rate v3' "$W/all.txt" && ok "firmware banner (v3) sent" || bad "firmware banner missing"
grep -aq '^# TEST-DONE' "$W/all.txt" && ok "ran to the end of the test" || bad "did not finish"

n=$(grep -a '^H,' "$W/all.txt" | wc -l)
malformed=$(grep -a '^H,' "$W/all.txt" | head -n -1 | grep -avc '^H,[0-9]*,[01],[0-9]*$' || true)
[ "$malformed" = 0 ] && ok "all $n data lines well formed: H,signal,beat,beatMs" || bad "$malformed malformed data lines"

# every pulse is exactly one beat, and the beat times are perfectly spaced
python3 - "$W/all.txt" <<'PY' || fails=$((fails+1))
import sys, re
flags, clocks = 0, []
for line in open(sys.argv[1], errors="replace"):
    m = re.match(r"H,(\d+),([01]),(\d+)$", line.strip())
    if not m: continue
    flags += int(m.group(2))
    bm = int(m.group(3))
    if bm and (not clocks or bm != clocks[-1]): clocks.append(bm)
gaps = [b - a for a, b in zip(clocks, clocks[1:])]
avg = 60000 * (len(clocks) - 1) / (clocks[-1] - clocks[0]) if len(clocks) > 1 else 0
res = [(flags == 24, f"24 pulses -> {flags} beat flags"),
       (len(clocks) == 24, f"24 pulses -> {len(clocks)} different beat times"),
       (gaps and all(g == 833 for g in gaps), f"every gap between beat times is exactly 833 ms (gaps seen: {sorted(set(gaps))})"),
       (abs(avg - 72.03) < 0.05, f"average rate from the beat times: {avg:.2f} BPM (true 72.03)")]
bad = 0
for good, msg in res:
    print(("  ok:   " if good else "  FAIL: ") + msg); bad += (not good)
sys.exit(1 if bad else 0)
PY
[ $fails = 0 ] && echo "ALL PASSED" || echo "FAILED ($fails)"
exit $fails
