/*
  BNTS heart rate sensor - DIGITAL mode test (clean version)

  Wiring:  sensor signal -> A0,  VCC -> 5V,  GND -> GND   (sensor switch on D)
  Serial Monitor: 115200 baud.

  What it does, in a loop:
    1. WAITING    Looks for a finger (two clean heartbeats in a row).
    2. ANALYSING  "Finger detected" - listens for 5 seconds.
    3. RESULT     Prints your heart rate in BPM (or says it couldn't get a clear reading).
    4. PAUSED     Waits 5 seconds, then goes back to step 1.

  The sensor's digital output chatters (flickers on/off every few ms) at the start and
  end of each pulse. This sketch smooths that out with a filter, so the chatter is ignored
  and each heartbeat is counted once. The board's LED (pin 13) shows the cleaned-up pulse.

  Tweak the numbers below if you like. Set SHOW_EDGES to 1 to print every cleaned beat.
*/

#define SENSOR_PIN      A0
#define ANALYSE_MS      5000UL   // how long to listen once a finger is detected
#define PAUSE_MS        5000UL   // wait after a result before checking again
#define MIN_IBI_MS      300UL    // fastest believable heartbeat gap (200 BPM)
#define MAX_IBI_MS      1500UL   // slowest believable heartbeat gap (40 BPM)
#define MIN_INTERVALS   3        // good beat gaps needed for a reading
#define TOLERANCE_PCT   25       // a gap may differ this much from the median and still count
#define SHOW_EDGES      0        // 1 = print every beat as it happens (for debugging)
#define MAX_BEATS       32

enum State { WAITING, ANALYSING, PAUSED };

State state = WAITING;
int  filt = 0;                   // smoothed pin value, 0..1000
bool level = false;              // cleaned-up pin state (high while a pulse is present)
unsigned long lastTickMs = 0;
unsigned long lastBeatMs = 0;
bool haveBeat = false;
unsigned long beatTimes[MAX_BEATS];
uint8_t nBeats = 0;
unsigned long stateStartMs = 0, lastNoteMs = 0;
uint8_t lastSecondShown = 0;

void addBeat(unsigned long t) {
  if (nBeats < MAX_BEATS) beatTimes[nBeats++] = t;
}

void startAnalysing(unsigned long now) {
  state = ANALYSING;
  stateStartMs = now;
  lastSecondShown = 0;
  nBeats = 0;
  addBeat(lastBeatMs);
  addBeat(now);
  Serial.println();
  Serial.println(F("Finger detected! Analysing your heartbeat for 5 seconds - hold still..."));
}

// Called once per cleaned-up heartbeat (rising edge of the filtered pulse).
void onBeat(unsigned long now) {
  if (state == PAUSED) return;
  if (haveBeat && (now - lastBeatMs) < MIN_IBI_MS) return;   // too soon after the last one

#if SHOW_EDGES
  Serial.print(F("  beat at "));
  Serial.print(now);
  Serial.println(F(" ms"));
#endif

  if (state == WAITING) {
    // Two beats in a row, a believable distance apart = a finger.
    if (haveBeat && (now - lastBeatMs) <= MAX_IBI_MS) startAnalysing(now);   // uses lastBeatMs as beat #1
  } else {
    addBeat(now);                    // ANALYSING: just collect beats
  }
  lastBeatMs = now;
  haveBeat = true;
}

void finishAnalysing(unsigned long now) {
  unsigned long ibi[MAX_BEATS];
  uint8_t n = 0;
  for (uint8_t i = 1; i < nBeats; i++) {
    unsigned long d = beatTimes[i] - beatTimes[i - 1];
    if (d >= MIN_IBI_MS && d <= MAX_IBI_MS) ibi[n++] = d;
  }

  bool ok = false;
  unsigned long bpm = 0;
  uint8_t kept = 0;
  if (n >= MIN_INTERVALS) {
    unsigned long sorted[MAX_BEATS];
    for (uint8_t i = 0; i < n; i++) {                 // insertion sort to find the median
      unsigned long v = ibi[i];
      int8_t j = i - 1;
      while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; j--; }
      sorted[j + 1] = v;
    }
    unsigned long med = sorted[n / 2];
    unsigned long sum = 0;
    for (uint8_t i = 0; i < n; i++) {
      long dev = (long)ibi[i] - (long)med;
      if (dev < 0) dev = -dev;
      if ((unsigned long)dev * 100 <= med * TOLERANCE_PCT) { sum += ibi[i]; kept++; }
    }
    if (kept >= MIN_INTERVALS) {
      bpm = (60000UL * kept + sum / 2) / sum;         // rounded
      ok = true;
    }
  }

  Serial.println(F("=========================================="));
  if (ok) {
    Serial.print(F("  HEART RATE:  "));
    Serial.print(bpm);
    Serial.println(F(" BPM"));
    Serial.print(F("  ("));
    Serial.print(nBeats);
    Serial.println(F(" beats heard, steady rhythm)"));
  } else {
    Serial.println(F("  No clear reading this time."));
    Serial.print(F("  ("));
    Serial.print(nBeats);
    Serial.println(F(" beats heard, but the rhythm wasn't steady enough.)"));
    Serial.println(F("  Rest your fingertip lightly on the sensor and hold still."));
  }
  Serial.println(F("=========================================="));
  Serial.println(F("Checking again in 5 seconds..."));

  state = PAUSED;
  stateStartMs = now;
}

void setup() {
  Serial.begin(115200);
  pinMode(SENSOR_PIN, INPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  delay(300);
  Serial.println(F("BNTS digital heart rate test  (Serial Monitor must be at 115200 baud)"));
  Serial.println(F("Rest a fingertip lightly on the sensor. Waiting for a finger..."));
  lastNoteMs = millis();
}

void loop() {
  unsigned long now = millis();

  // Once per millisecond: smooth the pin, ignoring chatter, and look for the start of a pulse.
  if (now != lastTickMs) {
    lastTickMs = now;
    int target = digitalRead(SENSOR_PIN) ? 1000 : 0;
    filt += (target - filt) / 32;                    // low-pass filter (~30 ms)
    if (!level && filt > 750) {                      // hysteresis: up at 75%, down at 25%
      level = true;
      digitalWrite(LED_BUILTIN, HIGH);
      onBeat(now);
    } else if (level && filt < 250) {
      level = false;
      digitalWrite(LED_BUILTIN, LOW);
    }
  }

  if (state == WAITING) {
    if (now - lastNoteMs >= 5000) {
      lastNoteMs = now;
      Serial.print(F("Waiting for a finger...  (pin is "));
      Serial.print(digitalRead(SENSOR_PIN) ? F("HIGH") : F("LOW"));
      Serial.println(F(")"));
    }
  } else if (state == ANALYSING) {
    unsigned long elapsed = now - stateStartMs;
    uint8_t secs = elapsed / 1000;
    if (secs > lastSecondShown && elapsed < ANALYSE_MS) {
      lastSecondShown = secs;
      Serial.print(F("  "));
      Serial.print((uint8_t)(ANALYSE_MS / 1000 - secs));
      Serial.println(F("..."));
    }
    if (elapsed >= ANALYSE_MS) finishAnalysing(now);
  } else {                                           // PAUSED
    if (now - stateStartMs >= PAUSE_MS) {
      state = WAITING;
      haveBeat = false;                              // forget old beats
      lastNoteMs = now;
      Serial.println();
      Serial.println(F("Ready - rest a fingertip on the sensor..."));
    }
  }
}
