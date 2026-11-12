// HeartDetector.h — beat detection for an ANALOG optical (PPG) pulse sensor (switch on A).
//
// (Digital sensors use StagedDetector.h instead.)
//
// Header-only and free of Arduino-specific calls, so the same code is unit-tested
// on a PC (see tests/test_detector.cpp).
//
//   1. The raw ADC value is lightly low-pass filtered to remove jitter.
//   2. A fast-attack / slow-decay envelope tracks the signal's recent max and min.
//      The gap between them is the pulse "amplitude" — it is big with a finger on
//      the sensor and tiny with nothing on it.
//   3. A beat is a rising crossing of 60% of that range (re-armed once the signal
//      falls below 40%), provided the amplitude is big enough.
//   4. Beat-to-beat gaps (IBI) go in a small ring. Outliers are rejected, BPM is the
//      average of the ring, and is only reported after MIN_BEATS_FOR_BPM good gaps.
//   5. If no beat arrives for NO_BEAT_TIMEOUT_MS, everything resets and BPM = 0.
#pragma once
#include <stdint.h>
#include <math.h>

namespace bnts {

// ---- Tuning knobs ---------------------------------------------------------
static const float    FILTER_ALPHA        = 0.30f;  // 0..1, higher = less smoothing
static const float    ENVELOPE_DECAY      = 0.003f; // per sample; ~1.7 s at 200 Hz
static const uint16_t MIN_AMPLITUDE       = 20;     // ADC counts; raise if noise gives phantom beats
static const uint16_t MIN_IBI_MS          = 300;    // 200 BPM ceiling
static const uint16_t MAX_IBI_MS          = 1500;   // 40 BPM floor
static const uint16_t NO_BEAT_TIMEOUT_MS  = 3000;   // finger removed -> BPM drops to 0
static const uint8_t  MIN_BEATS_FOR_BPM   = 3;      // good gaps needed before reporting
static const float    OUTLIER_TOLERANCE   = 0.35f;  // gap may differ 35% from the average
static const uint8_t  MAX_OUTLIERS_IN_ROW = 3;      // then assume the rate really changed
// ----------------------------------------------------------------------------

class HeartDetector {
 public:
  uint16_t bpm = 0;        // 0 = no valid reading
  uint16_t amplitude = 0;  // pulse size in ADC counts
  uint16_t signal = 0;     // filtered signal 0..1023
  uint8_t  stage = 0;      // 0 = no reading, 2 = reading available (analog mode has no separate "detected" stage)
  uint8_t  progress = 0;   // always 0 in analog mode

  // Feed one ADC sample (0..1023) with a millisecond timestamp.
  void update(uint16_t raw, uint32_t nowMs) {
    if (!started_) {
      filt_ = raw;
      envMax_ = envMin_ = filt_;
      started_ = true;
    }
    filt_ += ((float)raw - filt_) * FILTER_ALPHA;
    signal = (uint16_t)(filt_ + 0.5f);

    // Envelope: jumps to new extremes instantly, relaxes toward the middle slowly.
    float step = (envMax_ - envMin_) * ENVELOPE_DECAY;
    envMax_ = (filt_ > envMax_ - step) ? filt_ : envMax_ - step;
    envMin_ = (filt_ < envMin_ + step) ? filt_ : envMin_ + step;
    float range = envMax_ - envMin_;
    amplitude = (range > 1023.0f) ? 1023 : (uint16_t)range;

    const bool railed = (raw <= 3 || raw >= 1020);  // sensor pinned = nothing useful
    const float hi = envMin_ + range * 0.60f;
    const float lo = envMin_ + range * 0.40f;

    if (!armed_ && filt_ < lo) armed_ = true;
    if (armed_ && !railed && range >= MIN_AMPLITUDE && filt_ > hi) {
      if (tryBeat(nowMs)) armed_ = false;
    }
    checkTimeout(nowMs);
    stage = bpm > 0 ? 2 : 0;
  }

  // True once per detected beat (clears itself when read).
  bool takeBeat() {
    bool b = beatPending_ && bpm > 0;
    beatPending_ = false;
    return b;
  }

 private:
  // Register a beat at nowMs if enough time has passed since the last one.
  bool tryBeat(uint32_t nowMs) {
    uint32_t sinceBeat = nowMs - lastBeatMs_;
    uint32_t refractory = MIN_IBI_MS;
    if (bpm > 0) {  // don't re-trigger on the dicrotic notch of the previous beat
      uint32_t r = (uint32_t)(60000UL / bpm) * 6 / 10;
      if (r > refractory) refractory = r;
    }
    if (haveBeat_ && sinceBeat < refractory) return false;
    if (haveBeat_) acceptInterval(sinceBeat);
    haveBeat_ = true;
    lastBeatMs_ = nowMs;
    beatPending_ = true;
    return true;
  }

  void checkTimeout(uint32_t nowMs) {
    if (haveBeat_ && (nowMs - lastBeatMs_) > NO_BEAT_TIMEOUT_MS) reset();
  }

  void reset() {
    haveBeat_ = false;
    count_ = 0;
    head_ = 0;
    outliers_ = 0;
    bpm = 0;
    envMax_ = envMin_ = filt_;
    armed_ = false;
  }

  void acceptInterval(uint32_t ibi) {
    if (ibi < MIN_IBI_MS || ibi > MAX_IBI_MS) {
      if (++outliers_ >= MAX_OUTLIERS_IN_ROW) { count_ = 0; head_ = 0; bpm = 0; outliers_ = 0; }
      return;
    }
    if (count_ >= 2) {
      float avg = average();
      float dev = ((float)ibi - avg) / avg;
      if (dev < 0) dev = -dev;
      if (dev > OUTLIER_TOLERANCE) {
        if (++outliers_ >= MAX_OUTLIERS_IN_ROW) {  // rate genuinely changed: start over
          count_ = 0; head_ = 0; bpm = 0; outliers_ = 0;
          ring_[head_++] = ibi; count_ = 1;
        }
        return;
      }
    }
    outliers_ = 0;
    ring_[head_] = ibi;
    head_ = (head_ + 1) % RING;
    if (count_ < RING) count_++;
    if (count_ >= MIN_BEATS_FOR_BPM) {
      float b = 60000.0f / average();
      bpm = (uint16_t)(b + 0.5f);
    }
  }

  float average() const {
    float s = 0;
    for (uint8_t i = 0; i < count_; i++) s += ring_[i];
    return s / count_;
  }

  static const uint8_t RING = 6;
  uint32_t ring_[RING] = {0};
  uint8_t count_ = 0, head_ = 0, outliers_ = 0;
  uint32_t lastBeatMs_ = 0;
  float filt_ = 0, envMax_ = 0, envMin_ = 0;
  bool started_ = false, armed_ = false, haveBeat_ = false, beatPending_ = false;
};

}  // namespace bnts
