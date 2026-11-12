// StatusLed.h — the Arduino's on-board LED (pin 13) blinks briefly with every beat the Arduino detects, so you can SEE
// whether the sensor is working: put a finger on it and the LED should flash in time with your pulse.
//
//   LED flashes with your pulse   the Arduino is detecting beats. If the Pi still doesn't react, the problem is
//                                 between the Arduino and the Pi (run ./check-arduino.sh or bnts-monitor).
//   LED stays off                 the Arduino isn't detecting a pulse: check the sensor wiring, that the signal is on D2,
//                                 the sensor's A/D switch, and that the finger is on the sensor window.
//
// Header-only and free of Arduino calls so it can be unit-tested on a PC (tests/test_detector.cpp).
#pragma once
#include <stdint.h>

namespace bnts {

class StatusLed {
 public:
  static const uint32_t ON_MS = 120;   // how long the LED stays on after each beat

  // Returns true if the LED should be ON right now. Call every millisecond; pass beatNow = true on the call a beat arrives.
  bool update(bool beatNow, uint32_t nowMs) {
    if (beatNow) { lastBeatMs_ = nowMs; active_ = true; }
    return active_ && (nowMs - lastBeatMs_) < ON_MS;
  }

 private:
  bool active_ = false;
  uint32_t lastBeatMs_ = 0;
};

}  // namespace bnts
