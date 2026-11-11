/*
  BNTS heart rate sensor - ANALOG test with 5 second readings

  Wiring:  sensor signal -> A0,  VCC -> 5V,  GND -> GND   (sensor switch on A)
  Serial Monitor: 115200 baud.

  What it does, in a loop:
    1. WAITING    Looks for a finger (two clear heartbeats in a row).
    2. ANALYSING  "Finger detected" - listens for 5 seconds.
    3. RESULT     Prints your heart rate in BPM (or says it couldn't get a clear reading).
    4. PAUSED     Waits 5 seconds, then goes back to step 1.

  While waiting it also prints the raw signal and the "pulse size" every 3 seconds, and warns
  you if the signal looks stuck (that usually means wiring, power, or the A/D switch).

  Tweak the numbers below if you like. Set SHOW_SIGNAL to 1 for a live raw/pulse-size readout.
*/

#define SENSOR_PIN      A0
#define SAMPLE_MS       5        // 200 readings per second
#define MIN_RANGE       20       // pulse must be at least this big (0-1023 scale) to count as a finger
#define ANALYSE_MS      5000UL   // how long to listen once a finger is detected
#define PAUSE_MS        5000UL   // wait after a result before checking again
#define MIN_IBI_MS      300UL    // fastest believable heartbeat gap (200 BPM)
#define MAX_IBI_MS      1500UL   // slowest believable heartbeat gap (40 BPM)
#define MIN_INTERVALS   3        // good beat gaps needed for a reading
#define TOLERANCE_PCT   25       // a gap may differ this much from the median and still count
#define SHOW_SIGNAL     0        // 1 = print raw value and pulse size 4 times a second
#define MAX_BEATS       32

enum State { WAITING, ANALYSING, PAUSED };

State state = WAITING;
unsigned long lastSampleMs = 0;
float filt = 0, envMax = 0, envMin = 0;
bool started = false, armed = false;
unsigned long lastBeatMs = 0, lastIbi = 0;
bool haveBeat = false;
unsigned long beatTimes[MAX_BEATS];
uint8_t nBeats = 0;
unsigned long stateStartMs = 0, lastNoteMs = 0;
uint8_t lastSecondShown = 0;
int lastRaw = 0;
int pulseSize = 0;

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

// Called once per detected heartbeat.
void onBeat(unsigned long now) {
  if (state == WAITING) {
    // Two beats in a row, a believable distance apart = a finger.
    if (haveBeat && (now - lastBeatMs) <= MAX_IBI_MS) startAnalysing(now);   // uses lastBeatMs as beat #1
  } else {
    addBeat(now);                    // ANALYSING: just collect beats
  }
  if (haveBeat) {
    unsigned long gap = now - lastBeatMs;
    if (gap >= MIN_IBI_MS && gap <= MAX_IBI_MS) lastIbi = gap;
  }
  lastBeatMs = now;
  haveBeat = true;
}

// Process one sensor reading: filter, track the pulse's high/low, spot beats.
void processSample(int raw, unsigned long now) {
  if (!started) { filt = raw; envMax = envMin = raw; started = true; }
  lastRaw = raw;
  filt += (raw - filt) * 0.30f;

  float step = (envMax - envMin) * 0.003f;          // envelope: jumps to new peaks, relaxes slowly
  envMax = (filt > envMax - step) ? filt : envMax - step;
  envMin = (filt < envMin + step) ? filt : envMin + step;
  float range = envMax - envMin;
  pulseSize = (int)range;

  float hi = envMin + range * 0.60f;                // a beat = rising above 60% of the pulse...
  float lo = envMin + range * 0.40f;                // ...re-armed after dropping below 40%
  bool railed = (raw <= 3 || raw >= 1020);

  if (!armed && filt < lo) armed = true;

  if (armed && !railed && range >= MIN_RANGE && filt > hi && state != PAUSED) {
    unsigned long refractory = MIN_IBI_MS;
    if (lastIbi > 0 && lastIbi * 6 / 10 > refractory) refractory = lastIbi * 6 / 10;   // ignore the notch after a beat
    if (!haveBeat || (now - lastBeatMs) >= refractory) {
      armed = false;
      onBeat(now);
    }
  }
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
    Serial.print(F(" beats heard, steady rhythm, pulse size "));
    Serial.print(pulseSize);
    Serial.println(F(")"));
  } else {
    Serial.println(F("  No clear reading this time."));
    Serial.print(F("  ("));
    Serial.print(nBeats);
    Serial.print(F(" beats heard, but the rhythm wasn't steady enough. Pulse size "));
    Serial.print(pulseSize);
    Serial.println(F(")"));
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
  delay(300);
  Serial.println(F("BNTS analog heart rate test  (Serial Monitor must be at 115200 baud)"));
  Serial.println(F("Rest a fingertip lightly on the sensor. Waiting for a finger..."));
  lastNoteMs = millis();
}

void loop() {
  unsigned long now = millis();

  if (now - lastSampleMs >= SAMPLE_MS) {
    lastSampleMs = now;
    processSample(analogRead(SENSOR_PIN), now);
  }

#if SHOW_SIGNAL
  static unsigned long lastShow = 0;
  if (now - lastShow >= 250) {
    lastShow = now;
    Serial.print(F("raw "));
    Serial.print(lastRaw);
    Serial.print(F("   pulse size "));
    Serial.println(pulseSize);
  }
#endif

  if (state == WAITING) {
    if (now - lastNoteMs >= 3000) {
      lastNoteMs = now;
      Serial.print(F("Waiting for a finger...  (signal "));
      Serial.print(lastRaw);
      Serial.print(F(", pulse size "));
      Serial.print(pulseSize);
      Serial.println(F(")"));
      if (lastRaw <= 3)         Serial.println(F("  ! Signal is stuck at 0: check power/GND, the signal wire on A0, and that the A/D switch is on A."));
      else if (lastRaw >= 1020) Serial.println(F("  ! Signal is stuck at maximum: check the signal wire, and that the A/D switch is on A."));
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
      lastIbi = 0;
      armed = false;
      lastNoteMs = now;
      Serial.println();
      Serial.println(F("Ready - rest a fingertip on the sensor..."));
    }
  }
}
