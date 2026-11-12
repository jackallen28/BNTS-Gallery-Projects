// StagedDetector.h — heart rate from a DIGITAL pulse sensor (switch on D), with staged, stable readings.
//
// Header-only and free of Arduino-specific calls, so the same code is unit-tested on a PC
// (see tests/test_detector.cpp). This is the logic proven in arduino/sensor_test_staged.
//
//   WAITING     No finger. bpm = 0.
//   CONFIRMING  Beats arrive. Once a steady rhythm has lasted DETECT_MS (1 s) the finger is reported as DETECTED
//               (stage 1: single noise blips and irregular beats never get this far) and every real beat is passed
//               on, with a count of how many steady beats there have been (progress), so the display can respond
//               instantly. The reading is VERIFIED (stage 2) on the 4th steady beat (3 good beat gaps) provided the
//               rhythm has lasted at least CONFIRM_MS (1.5 s): about 2.5-3 s after the finger lands at 72 BPM. Fewer
//               gaps than that would make the first number drift by several BPM, so this is as fast as is accurate.
//               While confirming, the beats are already being averaged, so the moment the heart rate is verified the
//               first reading is ready.
//   LIVE        bpm holds the current reading. It refreshes once every UPDATE_MS (3 s), never
//               faster, each time averaging the last AVERAGE_WINDOW_MS (6 s) of beats (odd
//               beats are filtered out with a median test).
//               A reading more than OUTLIER_BPM from the current one is "odd": it is held back
//               and cross-checked against the next reading, which uses only the newest 3 s of
//               beats so it is independent of the odd one:
//                 - next agrees with the old value -> the odd one was a glitch, dropped
//                 - next agrees with the odd one   -> the change is real, accepted
//                 - neither                         -> held back again; after MAX_SKIPS, start over
//   Finger removed (no beats for NO_BEAT_TIMEOUT_MS) -> WAITING, bpm = 0.
//
// The sensor's digital output chatters (flickers every few ms) at the start and end of each
// pulse, so the pin is low-pass filtered (~32 ms) and passed through a Schmitt trigger: each
// pulse becomes exactly one beat.
//
// Digital mode has no waveform, so syntheticSignal() draws a pulse shape after each real beat
// (for the display's graph). The BPM itself is always real.
#pragma once
#include <stdint.h>
#include <math.h>

namespace bnts {

class StagedDetector {
 public:
  // ---- Tuning knobs ----
  static const uint32_t DETECT_MS          = 1000;  // steady beats for this long = "finger detected" (stage 1)
  static const uint32_t CONFIRM_MS         = 1500;  // the steady rhythm must have lasted at least this long to verify
  static const uint8_t  CONFIRM_MIN_BEATS  = 4;     // ...and be at least this many beats (= 3 good gaps)
  static const uint8_t  STEADY_PCT         = 30;    // while confirming, each gap within this % of the last
  static const uint32_t UPDATE_MS          = 3000;  // reading refreshes this often (never faster)
  static const uint32_t AVERAGE_WINDOW_MS  = 6000;  // each reading averages this much recent beat history
  static const int      OUTLIER_BPM        = 15;    // further than this from the current reading = "odd"
  static const uint8_t  MAX_SKIPS          = 3;     // odd readings in a row before starting over
  static const uint32_t NO_BEAT_TIMEOUT_MS = 3000;  // no beats this long = finger removed (covers a finger being adjusted)
  static const uint32_t MIN_IBI_MS         = 300;   // fastest believable beat gap (200 BPM)
  static const uint32_t MAX_IBI_MS         = 1500;  // slowest believable beat gap (40 BPM)
  static const uint8_t  MIN_GAPS           = 3;     // good gaps needed for a reading
  static const uint8_t  MIN_CHECK_GAPS     = 2;     // ...for the quick 3 s cross-check reading
  static const uint8_t  TOLERANCE_PCT      = 25;    // a gap may differ this much from the median and count

  enum State : uint8_t { WAITING, CONFIRMING, LIVE };

  // Things worth telling a human about (the firmware prints them as "# ..." lines).
  enum Event : uint8_t {
    EV_NONE, EV_SOMETHING, EV_NOT_STEADY, EV_CONFIRMED, EV_UPDATE, EV_ODD,
    EV_GLITCH, EV_REAL, EV_NO_CLEAR, EV_RESTART, EV_REMOVED
  };

  uint16_t bpm = 0;         // current reading; 0 unless LIVE
  uint16_t amplitude = 0;   // 0 = nothing heard, 100 = beats are being heard (display uses this as a hint)
  uint8_t  stage = 0;       // 0 = no finger, 1 = finger detected for 1 s (verifying), 2 = heart rate verified (LIVE)
  uint8_t  progress = 0;    // steady beats counted so far while verifying (0..CONFIRM_MIN_BEATS); full once verified

  State state() const { return state_; }

  // Call about once per millisecond with the sensor pin level and a millisecond timestamp.
  void update(bool pinHigh, uint32_t now) {
    filt_ += ((pinHigh ? 1000 : 0) - filt_) / 32;       // smooth away the chatter
    if (!level_ && filt_ > 750) {
      level_ = true;
      onBeat(now);
    } else if (level_ && filt_ < 250) {
      level_ = false;
    }

    if (state_ == CONFIRMING && (now - lastBeatMs_) > MAX_IBI_MS) {
      push(EV_NOT_STEADY, 0, 0);
      goWaiting();
    } else if (state_ == LIVE && (now - lastBeatMs_) > NO_BEAT_TIMEOUT_MS) {
      push(EV_REMOVED, 0, 0);
      goWaiting();
    }

    if (state_ == LIVE && (now - lastUpdateMs_) >= UPDATE_MS) {
      lastUpdateMs_ = now;
      int rLong  = windowReading(now, AVERAGE_WINDOW_MS, MIN_GAPS);
      int rCheck = windowReading(now, UPDATE_MS, MIN_CHECK_GAPS);
      handleReading(rLong, rCheck, now);
    }

    if (state_ == LIVE) stage = 2;
    else if (state_ == CONFIRMING && streakBeats_ >= 2 && (now - streakStartMs_) >= DETECT_MS) stage = 1;
    else stage = 0;
    progress = (state_ == LIVE) ? CONFIRM_MIN_BEATS
             : (stage >= 1 ? (streakBeats_ < CONFIRM_MIN_BEATS ? streakBeats_ : CONFIRM_MIN_BEATS) : 0);
    bpm = (state_ == LIVE) ? (uint16_t)published_ : 0;
    amplitude = (state_ == WAITING) ? 0 : 100;
  }

  // True once per real beat (clears itself when read): while LIVE, and while verifying once a rhythm has started
  // (two or more steady beats), so the display can respond to the very first beats of a real pulse.
  bool takeBeat() {
    bool b = beatPending_ && (state_ == LIVE || (state_ == CONFIRMING && streakBeats_ >= 2));
    beatPending_ = false;
    return b;
  }

  // A pulse-shaped waveform centred on 512, drawn after each beat, for the display graph.
  uint16_t syntheticSignal(uint32_t now) const {
    if (state_ != LIVE) return 512;
    float t = (float)(now - lastBeatMs_) / 1000.0f;
    if (t > 0.8f) return 512;
    float a = (t - 0.12f) / 0.06f, b = (t - 0.34f) / 0.07f;
    return (uint16_t)(512.0f + 100.0f * expf(-a * a) + 35.0f * expf(-b * b));
  }

  // Pop the next event (returns false when there are none). a and b are event-specific numbers.
  bool takeEvent(Event& e, int& a, int& b) {
    if (qCount_ == 0) return false;
    e = qEv_[qHead_]; a = qA_[qHead_]; b = qB_[qHead_];
    qHead_ = (qHead_ + 1) % QSIZE;
    qCount_--;
    return true;
  }

 private:
  static const uint8_t MAX_BEATS = 24;
  static const uint8_t QSIZE = 6;

  void push(Event e, int a, int b) {
    if (qCount_ == QSIZE) return;                        // never block; drop if nobody is listening
    uint8_t tail = (qHead_ + qCount_) % QSIZE;
    qEv_[tail] = e; qA_[tail] = a; qB_[tail] = b;
    qCount_++;
  }

  void addBeat(uint32_t t) {
    if (nBeats_ == MAX_BEATS) {                          // buffer full: drop the oldest
      for (uint8_t i = 1; i < MAX_BEATS; i++) beats_[i - 1] = beats_[i];
      nBeats_--;
    }
    beats_[nBeats_++] = t;
  }

  // Average BPM from the beats in the last windowMs, or 0 if they aren't good enough.
  int windowReading(uint32_t now, uint32_t windowMs, uint8_t minGaps) const {
    uint32_t cutoff = now > windowMs ? now - windowMs : 0;
    uint32_t gaps[MAX_BEATS];
    uint8_t n = 0;
    for (uint8_t i = 1; i < nBeats_; i++) {
      if (beats_[i - 1] < cutoff) continue;
      uint32_t d = beats_[i] - beats_[i - 1];
      if (d >= MIN_IBI_MS && d <= MAX_IBI_MS) gaps[n++] = d;
    }
    if (n < minGaps) return 0;

    uint32_t sorted[MAX_BEATS];
    for (uint8_t i = 0; i < n; i++) {                    // insertion sort to find the median
      uint32_t v = gaps[i];
      int8_t j = (int8_t)i - 1;
      while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; j--; }
      sorted[j + 1] = v;
    }
    uint32_t med = sorted[n / 2];
    uint32_t sum = 0;
    uint8_t kept = 0;
    for (uint8_t i = 0; i < n; i++) {                    // odd gaps (far from the median) are left out
      int32_t dev = (int32_t)gaps[i] - (int32_t)med;
      if (dev < 0) dev = -dev;
      if ((uint32_t)dev * 100 <= med * TOLERANCE_PCT) { sum += gaps[i]; kept++; }
    }
    if (kept < minGaps) return 0;
    return (int)((60000UL * kept + sum / 2) / sum);
  }

  void goWaiting() {
    state_ = WAITING;
    haveBeat_ = false;
    nBeats_ = 0;
    published_ = pending_ = 0;
    skips_ = noReading_ = 0;
    streakBeats_ = 0;
  }

  // Start (or restart) confirming a finger from the beat at beatMs.
  void startConfirming(uint32_t beatMs) {
    state_ = CONFIRMING;
    nBeats_ = 0;
    addBeat(beatMs);
    streakStartMs_ = beatMs;
    streakBeats_ = 1;
    lastGap_ = 0;
    published_ = pending_ = 0;
    skips_ = noReading_ = 0;
  }

  static bool closeTo(int a, int b) {
    int d = a - b;
    if (d < 0) d = -d;
    return d <= OUTLIER_BPM;
  }

  void holdBack(int r) {
    pending_ = r;
    skips_++;
    push(EV_ODD, r, published_);
    if (skips_ >= MAX_SKIPS) {
      push(EV_RESTART, 0, 0);
      startConfirming(lastBeatMs_);
    }
  }

  void publish(int r) {
    published_ = r;
    pending_ = 0;
    skips_ = 0;
  }

  // rLong = average of the last 6 s of beats; rCheck = average of the newest 3 s only. 0 = no clear reading.
  void handleReading(int rLong, int rCheck, uint32_t now) {
    (void)now;
    if (rLong == 0 && rCheck == 0) {
      noReading_++;
      push(EV_NO_CLEAR, published_, 0);
      if (noReading_ >= MAX_SKIPS) {
        push(EV_RESTART, 0, 0);
        startConfirming(lastBeatMs_);
      }
      return;
    }
    noReading_ = 0;

    if (pending_) {                                      // cross-checking an earlier odd reading
      if (rCheck == 0) {                                 // nothing usable from the newest beats: wait a round
        push(EV_NO_CLEAR, published_, 0);
        return;
      }
      if (closeTo(rCheck, pending_)) {                   // fresh data agrees with the odd reading: real
        publish(rCheck);
        push(EV_REAL, rCheck, 0);
      } else if (closeTo(rCheck, published_)) {          // fresh data agrees with the old value: glitch
        push(EV_GLITCH, pending_, 0);
        int r = (rLong && closeTo(rLong, published_)) ? rLong : rCheck;
        publish(r);
        push(EV_UPDATE, r, 0);
      } else {                                           // disagrees with both: hold this one back too
        holdBack(rCheck);
      }
      return;
    }

    int r = rLong ? rLong : rCheck;
    if (closeTo(r, published_)) {                        // normal: close to the current reading
      publish(r);
      push(EV_UPDATE, r, 0);
    } else {
      holdBack(r);                                       // odd: skip it and check the next one
    }
  }

  // One cleaned-up heartbeat (start of a pulse).
  void onBeat(uint32_t now) {
    if (haveBeat_ && (now - lastBeatMs_) < MIN_IBI_MS) return;   // too soon after the last one
    uint32_t prev = lastBeatMs_;
    bool hadBeat = haveBeat_;
    lastBeatMs_ = now;
    haveBeat_ = true;

    if (state_ == WAITING) {
      startConfirming(now);
      push(EV_SOMETHING, 0, 0);
    } else if (state_ == CONFIRMING) {
      uint32_t gap = hadBeat ? now - prev : 0;
      bool plausible = gap >= MIN_IBI_MS && gap <= MAX_IBI_MS;
      bool steady = lastGap_ == 0 ||
                    (gap * 100 <= lastGap_ * (100 + STEADY_PCT) && gap * 100 >= lastGap_ * (100 - STEADY_PCT));
      if (plausible && steady) {
        addBeat(now);                                    // keep averaging while we confirm
        streakBeats_++;
        lastGap_ = gap;
        if (streakBeats_ >= CONFIRM_MIN_BEATS && (now - streakStartMs_) >= CONFIRM_MS) {
          int r = windowReading(now, now - streakStartMs_ + 1, MIN_GAPS);   // everything heard so far
          if (r > 0) {                                   // steady long enough AND a reading is ready: go live
            state_ = LIVE;
            lastUpdateMs_ = now;
            publish(r);
            push(EV_CONFIRMED, r, 0);
          }                                              // (else keep listening until there are enough beats)
        }
      } else {                                           // not regular: start counting again from this beat
        startConfirming(now);
      }
    } else {
      addBeat(now);                                      // LIVE
    }
    beatPending_ = true;
  }

  State state_ = WAITING;
  int filt_ = 0;
  bool level_ = false;

  uint32_t lastBeatMs_ = 0;
  bool haveBeat_ = false, beatPending_ = false;
  uint32_t beats_[MAX_BEATS];
  uint8_t nBeats_ = 0;

  uint32_t streakStartMs_ = 0, lastGap_ = 0;
  uint8_t streakBeats_ = 0;

  uint32_t lastUpdateMs_ = 0;
  int published_ = 0, pending_ = 0;
  uint8_t skips_ = 0, noReading_ = 0;

  Event qEv_[QSIZE];
  int qA_[QSIZE], qB_[QSIZE];
  uint8_t qHead_ = 0, qCount_ = 0;
};

}  // namespace bnts
