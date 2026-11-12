// AnalogBeats.h — beats from an ANALOG optical (PPG) pulse sensor (switch on A). No heart-rate filtering: every
// pulse that crosses the threshold is a beat; the Pi works out the average heart rate from the beat times.
//
// Turning a waveform into beats needs a threshold, so:
//   1. the raw ADC value is lightly smoothed,
//   2. a fast-attack / slow-decay envelope tracks the signal's recent high and low,
//   3. a beat is a rise above 60% of that range (re-armed once the signal falls below 40%), provided the pulse is at
//      least MIN_AMPLITUDE counts tall (otherwise it is just noise from an empty sensor).
//
// Header-only and free of Arduino-specific calls so it can be unit-tested on a PC (tests/test_detector.cpp).
#pragma once
#include <stdint.h>

namespace bnts {

static const float    ANALOG_FILTER_ALPHA   = 0.30f;  // 0..1, higher = less smoothing
static const float    ANALOG_ENVELOPE_DECAY = 0.003f; // per sample; about 1.7 s at 200 Hz
static const uint16_t ANALOG_MIN_AMPLITUDE  = 20;     // ADC counts; raise if an empty sensor produces phantom beats

class AnalogBeats {
 public:
  uint16_t signal = 0;     // smoothed signal 0..1023 (the graph on the display)

  // Feed one ADC sample (0..1023) with a millisecond timestamp.
  void update(uint16_t raw, uint32_t nowMs) {
    if (!started_) { filt_ = raw; envMax_ = envMin_ = filt_; started_ = true; }
    filt_ += ((float)raw - filt_) * ANALOG_FILTER_ALPHA;
    signal = (uint16_t)(filt_ + 0.5f);

    float step = (envMax_ - envMin_) * ANALOG_ENVELOPE_DECAY;        // jumps to new peaks, relaxes slowly
    envMax_ = (filt_ > envMax_ - step) ? filt_ : envMax_ - step;
    envMin_ = (filt_ < envMin_ + step) ? filt_ : envMin_ + step;
    float range = envMax_ - envMin_;

    const bool railed = (raw <= 3 || raw >= 1020);                    // sensor pinned = nothing useful
    const float hi = envMin_ + range * 0.60f, lo = envMin_ + range * 0.40f;
    if (!armed_ && filt_ < lo) armed_ = true;
    if (armed_ && !railed && range >= ANALOG_MIN_AMPLITUDE && filt_ > hi) {
      armed_ = false;
      lastBeatMs_ = nowMs;
      pending_ = true;
    }
  }

  bool takeBeat() { bool b = pending_; pending_ = false; return b; }  // true once per beat
  uint32_t lastBeatMs() const { return lastBeatMs_; }

 private:
  float filt_ = 0, envMax_ = 0, envMin_ = 0;
  bool started_ = false, armed_ = false, pending_ = false;
  uint32_t lastBeatMs_ = 0;
};

}  // namespace bnts
