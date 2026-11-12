// BeatSensor.h — beats from a DIGITAL pulse sensor (switch on D). No filtering of the heart rate: every pulse is a beat.
//
// The sensor's digital output chatters (flickers on and off every few milliseconds) at the start and end of each
// pulse. Counting that raw would give 5-20 "beats" per heartbeat, so the pin is smoothed over about 30 ms and passed
// through a Schmitt trigger. That is the only processing: each pulse becomes exactly one beat, and the time of every beat
// (the Arduino's own millisecond clock) is reported to the Pi, which works out the average heart rate.
//
// Header-only and free of Arduino-specific calls so it can be unit-tested on a PC (tests/test_detector.cpp).
#pragma once
#include <stdint.h>
#include <math.h>

namespace bnts {

class BeatSensor {
 public:
  static const int DIV  = 32;    // smoothing: the pin is averaged over about 32 ms at 1 kHz
  static const int HIGH_AT = 750;  // smoothed level (0..1000) at which a pulse starts
  static const int LOW_AT  = 250;  // ...and ends

  // Call about once per millisecond with the sensor pin level and a millisecond timestamp.
  void update(bool pinHigh, uint32_t now) {
    filt_ += ((pinHigh ? 1000 : 0) - filt_) / DIV;
    if (!level_ && filt_ > HIGH_AT) {
      level_ = true;
      lastBeatMs_ = now;
      haveBeat_ = true;
      pending_ = true;
    } else if (level_ && filt_ < LOW_AT) {
      level_ = false;
    }
  }

  // True once per beat (clears itself when read).
  bool takeBeat() {
    bool b = pending_;
    pending_ = false;
    return b;
  }

  uint32_t lastBeatMs() const { return lastBeatMs_; }   // time of the most recent beat (0 until the first one)

  // A pulse-shaped waveform centred on 512, drawn after each beat, for the display's graph (digital mode has no real waveform).
  uint16_t syntheticSignal(uint32_t now) const {
    if (!haveBeat_) return 512;
    float t = (float)(now - lastBeatMs_) / 1000.0f;
    if (t > 0.8f) return 512;
    float a = (t - 0.12f) / 0.06f, b = (t - 0.34f) / 0.07f;
    return (uint16_t)(512.0f + 100.0f * expf(-a * a) + 35.0f * expf(-b * b));
  }

 private:
  int filt_ = 0;
  bool level_ = false, haveBeat_ = false, pending_ = false;
  uint32_t lastBeatMs_ = 0;
};

}  // namespace bnts
