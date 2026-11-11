/*
  BNTS heart rate sensor - standalone test

  Use this on an Arduino plugged into your Mac to check the sensor and wiring
  BEFORE using the full system. It does not need the Raspberry Pi.

  Wiring:  sensor signal -> A0,  sensor VCC -> 5V,  sensor GND -> GND

  Serial Monitor (set to 115200 baud) shows one line every 0.1 s:

    raw  512 |##############                 | range   70  PULSE    beats 12  <3

    raw     the sensor reading, 0-1023
    bar     the reading as a bar, zoomed to the last 2 s: it rises and falls with each pulse
    range   how much the reading moved in the last 2 seconds
            under ~20  = nothing on the sensor (or finger not making contact)
            over  ~20  = a pulse wave is coming through
    status  PULSE or NO FINGER
    beats   count of heartbeats seen since reset;  <3 flashes on each beat

  Serial Plotter: change PLOTTER_MODE to 1 below, re-upload, then open
  Tools > Serial Plotter (115200 baud) to see the pulse wave as a graph.
*/

#define SENSOR_PIN   A0
#define PLOTTER_MODE 0     // 0 = readable text for Serial Monitor, 1 = graph for Serial Plotter
#define MIN_RANGE    20    // pulse wave must move at least this much to count as a finger

unsigned long lastSample = 0, lastPrint = 0, windowStart = 0;
int  curMin = 1023, curMax = 0;   // min/max in the current 2 s window
int  lastMin = 0, lastMax = 0;    // min/max of the previous, completed window
bool above = false;               // are we currently above the beat threshold?
bool beatFlash = false;           // a beat happened since the last printed line
unsigned long beats = 0;

void printPadded(int value, int width) {
  int digits = 1;
  for (int v = value; v >= 10; v /= 10) digits++;
  for (int i = digits; i < width; i++) Serial.print(' ');
  Serial.print(value);
}

void setup() {
  Serial.begin(115200);
  delay(300);
#if !PLOTTER_MODE
  Serial.println(F("BNTS sensor test  (Serial Monitor must be set to 115200 baud)"));
  Serial.println(F("Nothing on the sensor: range stays small. Rest a fingertip on it: range jumps and beats count up."));
  Serial.println();
#endif
  windowStart = millis();
}

void loop() {
  unsigned long now = millis();
  if (now - lastSample < 10) return;          // sample at 100 Hz
  lastSample = now;

  int raw = analogRead(SENSOR_PIN);

  // Track min/max over rolling 2-second windows.
  if (raw < curMin) curMin = raw;
  if (raw > curMax) curMax = raw;
  if (now - windowStart >= 2000) {
    lastMin = curMin;
    lastMax = curMax;
    curMin = curMax = raw;
    windowStart = now;
  }

  int range = lastMax - lastMin;
  int hi = lastMin + (range * 6) / 10;        // beat when rising above 60% of the range...
  int lo = lastMin + (range * 4) / 10;        // ...and re-arm when falling below 40%
  bool fingerOn = range >= MIN_RANGE;

  if (fingerOn) {
    if (!above && raw > hi) { above = true; beats++; beatFlash = true; }
    else if (above && raw < lo) { above = false; }
  } else {
    above = false;
  }

#if PLOTTER_MODE
  Serial.print(F("Signal:"));    Serial.print(raw);
  Serial.print(F(",Threshold:")); Serial.println(fingerOn ? hi : raw);
#else
  if (now - lastPrint >= 100) {               // print 10 lines per second
    lastPrint = now;
    Serial.print(F("raw "));
    printPadded(raw, 4);
    Serial.print(F(" |"));
    int span = range < 40 ? 40 : range;       // zoomed to the last 2 s so the pulse is easy to see
    int filled = (int)((long)(raw - lastMin) * 30 / span);
    if (filled < 0) filled = 0;
    if (filled > 30) filled = 30;
    for (int i = 0; i < 30; i++) Serial.print(i < filled ? '#' : ' ');
    Serial.print(F("| range "));
    printPadded(range, 4);
    Serial.print(fingerOn ? F("  PULSE    ") : F("  NO FINGER"));
    Serial.print(F("  beats "));
    printPadded((int)beats, 4);
    if (beatFlash) { Serial.print(F("  <3")); beatFlash = false; }
    Serial.println();
  }
#endif
}
