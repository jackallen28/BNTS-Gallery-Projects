/*
  BNTS heart rate - STAGED measurement test (digital sensor, pin D2)

  Wiring:  sensor signal -> D2 (digital pin 2),  VCC -> 5V,  GND -> GND
           sensor switch on D (digital).
  Serial Monitor: 115200 baud.

  How it behaves (this is the logic that will go into the real firmware):

    1. WAITING     No finger. Nothing happens.
    2. CONFIRMING  Beats start arriving. A finger only counts once it has given steady,
                   regular beats for 3 seconds. Brief touches and flicker are ignored.
                   While it confirms, it is already averaging the beats into a reading,
                   so the moment the finger is confirmed the first reading is ready.
    3. LIVE        The reading is refreshed once every 3 seconds (never faster), averaged
                   over the last 6 seconds of beats (odd beats are filtered out).
                   A reading more than 15 BPM away from the current one is "odd": it is
                   skipped and cross-checked against the next one, which is measured from
                   the newest 3 seconds only, so it is independent of the odd one:
                     - next one agrees with the old value -> the odd one was a glitch, dropped
                     - next one agrees with the odd one   -> the change is real, accepted
                     - neither                             -> skipped again; after 3 skips it starts over
    4. Finger removed (no beats for 3 s) -> back to WAITING.

  The board's LED (pin 13) shows each detected pulse. Tweak the numbers below if you like.
*/

#define SENSOR_PIN          2
#define CONFIRM_MS          3000UL   // steady beats needed this long before a finger is believed
#define CONFIRM_MIN_BEATS   3        // ...and at least this many beats in that time
#define STEADY_PCT          30       // while confirming, each beat gap must be within this % of the last gap
#define UPDATE_MS           3000UL   // reading refreshes this often (never faster)
#define AVERAGE_WINDOW_MS   6000UL   // each reading averages the beats from this long
#define OUTLIER_BPM         15       // a reading further than this from the current one is "odd"
#define MAX_SKIPS           3        // this many odd readings in a row -> start again from scratch
#define NO_BEAT_TIMEOUT_MS  3000UL   // no beats for this long = finger removed
#define MIN_IBI_MS          300UL    // fastest believable gap between beats (200 BPM)
#define MAX_IBI_MS          1500UL   // slowest believable gap (40 BPM)
#define MIN_GAPS            3        // good beat gaps needed to make a reading
#define MIN_CHECK_GAPS      2        // ...for the quick 3-second cross-check reading
#define TOLERANCE_PCT       25       // a gap may differ this much from the median and still count
#define MAX_BEATS           24

enum State { WAITING, CONFIRMING, LIVE };

State state = WAITING;

// pin filter (the sensor's output chatters at the start and end of each pulse)
int  filt = 0;
bool level = false;
unsigned long lastTickMs = 0;

// beats
unsigned long lastBeatMs = 0;
bool haveBeat = false;
unsigned long beats[MAX_BEATS];
uint8_t nBeats = 0;

// confirming
unsigned long streakStartMs = 0, lastGap = 0;
uint8_t streakBeats = 0;

// live
unsigned long lastUpdateMs = 0, lastNoteMs = 0;
int published = 0, pending = 0;
uint8_t skips = 0, noReading = 0;

void printPadded(unsigned long value, int width) {
  int digits = 1;
  for (unsigned long v = value; v >= 10; v /= 10) digits++;
  for (int i = digits; i < width; i++) Serial.print(' ');
  Serial.print(value);
}

void stamp() {
  Serial.print(F("["));
  printPadded(millis() / 1000, 3);
  Serial.print(F(" s] "));
}

void addBeat(unsigned long t) {
  if (nBeats == MAX_BEATS) {                       // buffer full: drop the oldest
    for (uint8_t i = 1; i < MAX_BEATS; i++) beats[i - 1] = beats[i];
    nBeats--;
  }
  beats[nBeats++] = t;
}

// Average heart rate from the beats in the last windowMs, or 0 if they aren't good enough.
int windowReading(unsigned long now, unsigned long windowMs, uint8_t minGaps) {
  unsigned long cutoff = now > windowMs ? now - windowMs : 0;
  unsigned long gaps[MAX_BEATS];
  uint8_t n = 0;
  for (uint8_t i = 1; i < nBeats; i++) {
    if (beats[i - 1] < cutoff) continue;
    unsigned long d = beats[i] - beats[i - 1];
    if (d >= MIN_IBI_MS && d <= MAX_IBI_MS) gaps[n++] = d;
  }
  if (n < minGaps) return 0;

  unsigned long sorted[MAX_BEATS];
  for (uint8_t i = 0; i < n; i++) {                // insertion sort to find the median
    unsigned long v = gaps[i];
    int8_t j = i - 1;
    while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; j--; }
    sorted[j + 1] = v;
  }
  unsigned long med = sorted[n / 2];
  unsigned long sum = 0;
  uint8_t kept = 0;
  for (uint8_t i = 0; i < n; i++) {                // odd beats (far from the median) are left out
    long dev = (long)gaps[i] - (long)med;
    if (dev < 0) dev = -dev;
    if ((unsigned long)dev * 100 <= med * TOLERANCE_PCT) { sum += gaps[i]; kept++; }
  }
  if (kept < minGaps) return 0;
  return (int)((60000UL * kept + sum / 2) / sum);
}

void goWaiting() {
  state = WAITING;
  haveBeat = false;
  nBeats = 0;
  published = pending = 0;
  skips = noReading = 0;
  streakBeats = 0;
  lastNoteMs = millis();
}

// Start (or restart) confirming a finger from the beat we just heard.
void startConfirming(unsigned long beatMs) {
  state = CONFIRMING;
  nBeats = 0;
  addBeat(beatMs);
  streakStartMs = beatMs;
  streakBeats = 1;
  lastGap = 0;
  published = pending = 0;
  skips = noReading = 0;
}

void publish(int bpm, const __FlashStringHelper* label) {
  published = bpm;
  pending = 0;
  skips = 0;
  stamp();
  Serial.print(label);
  Serial.print(bpm);
  Serial.println(F(" BPM"));
}

// Skip an odd reading (hold it back) and count it; too many in a row = start again.
void holdBack(int r, unsigned long now) {
  pending = r;
  skips++;
  stamp();
  Serial.print(F("Odd reading ("));
  Serial.print(r);
  Serial.print(F(" BPM) - skipped, checking the next one. Still showing "));
  Serial.print(published);
  Serial.println(F(" BPM"));
  if (skips >= MAX_SKIPS) {
    stamp();
    Serial.println(F("Readings keep changing - checking the finger again."));
    startConfirming(lastBeatMs);
  }
}

bool closeTo(int a, int b) {
  int d = a - b;
  if (d < 0) d = -d;
  return d <= OUTLIER_BPM;
}

// Called every UPDATE_MS.
//   rLong  = average of the last 6 s of beats (0 = couldn't get a clear one)
//   rCheck = average of the newest 3 s only, which no earlier reading has used
void handleReading(int rLong, int rCheck, unsigned long now) {
  if (rLong == 0 && rCheck == 0) {
    noReading++;
    stamp();
    Serial.print(F("No clear reading this time - keeping "));
    Serial.print(published);
    Serial.println(F(" BPM"));
    if (noReading >= MAX_SKIPS) {
      stamp();
      Serial.println(F("Signal too unsteady - checking the finger again."));
      startConfirming(lastBeatMs);
    }
    return;
  }
  noReading = 0;

  if (pending) {                                    // we're cross-checking an earlier odd reading
    if (rCheck == 0) {                              // nothing usable from the newest beats: wait one more round
      stamp();
      Serial.print(F("No clear reading this time - still checking "));
      Serial.print(pending);
      Serial.println(F(" BPM"));
      return;
    }
    if (closeTo(rCheck, pending)) {                    // fresh data agrees with the odd reading: it's real
      stamp();
      Serial.println(F("Two readings agree - the change is real."));
      publish(rCheck, F("Update: "));
    } else if (closeTo(rCheck, published)) {           // fresh data agrees with the old value: it was a glitch
      stamp();
      Serial.print(F("Earlier odd reading ("));
      Serial.print(pending);
      Serial.println(F(" BPM) was a glitch - ignored."));
      publish((rLong && closeTo(rLong, published)) ? rLong : rCheck, F("Update: "));
    } else {                                        // disagrees with both: hold this one back too
      holdBack(rCheck, now);
    }
    return;
  }

  int r = rLong ? rLong : rCheck;
  if (closeTo(r, published)) publish(r, F("Update: "));   // normal: close to the current reading
  else holdBack(r, now);                               // odd: skip it and check the next one
}

// One cleaned-up heartbeat (start of a pulse).
void onBeat(unsigned long now) {
  if (haveBeat && (now - lastBeatMs) < MIN_IBI_MS) return;   // too soon after the last one
  unsigned long prev = lastBeatMs;
  bool hadBeat = haveBeat;
  lastBeatMs = now;
  haveBeat = true;

  if (state == WAITING) {
    startConfirming(now);
    stamp();
    Serial.println(F("Something detected - checking it's a steady pulse..."));
  } else if (state == CONFIRMING) {
    unsigned long gap = hadBeat ? now - prev : 0;
    bool plausible = gap >= MIN_IBI_MS && gap <= MAX_IBI_MS;
    bool steady = lastGap == 0 ||
                  (gap * 100 <= lastGap * (100 + STEADY_PCT) && gap * 100 >= lastGap * (100 - STEADY_PCT));
    if (plausible && steady) {
      addBeat(now);                                 // keep averaging while we confirm
      streakBeats++;
      lastGap = gap;
      if (streakBeats >= CONFIRM_MIN_BEATS && (now - streakStartMs) >= CONFIRM_MS) {
        int r = windowReading(now, now - streakStartMs + 1, MIN_GAPS);   // reading from everything heard so far
        if (r > 0) {                                // steady for 3 s AND a reading is ready: go live
          state = LIVE;
          lastUpdateMs = now;
          stamp();
          Serial.println(F("Finger confirmed."));
          publish(r, F("READING: "));
        }                                           // (else keep listening until there are enough beats)
      }
    } else {                                        // not regular: start counting again from this beat
      startConfirming(now);
    }
  } else {
    addBeat(now);                                   // LIVE
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(SENSOR_PIN, INPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  delay(300);
  Serial.println(F("BNTS staged heart rate test (sensor on D2, Serial Monitor at 115200 baud)"));
  Serial.println(F("A finger counts after 3 s of steady beats (reading ready at once), then updates every 3 s."));
  Serial.println();
  goWaiting();
  stamp();
  Serial.println(F("Waiting for a finger..."));
}

void loop() {
  unsigned long now = millis();

  // Once per millisecond: smooth the pin (ignores chatter) and look for the start of a pulse.
  if (now != lastTickMs) {
    lastTickMs = now;
    int target = digitalRead(SENSOR_PIN) ? 1000 : 0;
    filt += (target - filt) / 32;
    if (!level && filt > 750) {
      level = true;
      digitalWrite(LED_BUILTIN, HIGH);
      onBeat(now);
    } else if (level && filt < 250) {
      level = false;
      digitalWrite(LED_BUILTIN, LOW);
    }
  }

  // Lost the beat?
  if (state == CONFIRMING && (now - lastBeatMs) > MAX_IBI_MS) {
    stamp();
    Serial.println(F("Not a steady pulse - ignoring it."));
    goWaiting();
  } else if (state == LIVE && (now - lastBeatMs) > NO_BEAT_TIMEOUT_MS) {
    stamp();
    Serial.println(F("Finger removed."));
    goWaiting();
    stamp();
    Serial.println(F("Waiting for a finger..."));
  }

  if (state == WAITING) {
    if (now - lastNoteMs >= 10000) {
      lastNoteMs = now;
      stamp();
      Serial.print(F("Waiting for a finger...  (pin D2 is "));
      Serial.print(digitalRead(SENSOR_PIN) ? F("HIGH") : F("LOW"));
      Serial.println(F(")"));
    }
  } else if (state == LIVE) {
    if (now - lastUpdateMs >= UPDATE_MS) {
      lastUpdateMs = now;
      int rLong  = windowReading(now, AVERAGE_WINDOW_MS, MIN_GAPS);
      int rCheck = windowReading(now, UPDATE_MS, MIN_CHECK_GAPS);
      handleReading(rLong, rCheck, now);
    }
  }
}
