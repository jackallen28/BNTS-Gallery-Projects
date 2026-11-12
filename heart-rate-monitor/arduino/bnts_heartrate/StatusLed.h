// StatusLed.h — blink pattern for the Arduino's on-board LED (pin 13), so you can SEE what the Arduino thinks.
//
//   LED off            no finger detected (no reading)
//   LED flashing       finger detected and a reading is available: 0.5 s on, 0.5 s off
//
// If the LED flashes but the Pi display stays on standby, the Arduino is fine and the problem is between the
// Arduino and the Pi (run ./check-arduino.sh). If the LED never flashes, the Arduino isn't detecting the finger:
// check the sensor wiring, the pin (D2 for digital mode) and the sensor's A/D switch.
//
// Header-only and free of Arduino calls so it can be unit-tested on a PC (tests/test_detector.cpp).
#pragma once
#include <stdint.h>

namespace bnts {

class StatusLed {
 public:
  static const uint32_t HALF_PERIOD_MS = 500;

  // Returns true if the LED should be ON right now. The blink always starts with "on" the moment a finger is detected.
  bool update(bool fingerDetected, uint32_t nowMs) {
    if (!fingerDetected) {
      active_ = false;
      return false;
    }
    if (!active_) {
      active_ = true;
      startMs_ = nowMs;
    }
    return ((nowMs - startMs_) / HALF_PERIOD_MS) % 2 == 0;
  }

 private:
  bool active_ = false;
  uint32_t startMs_ = 0;
};

}  // namespace bnts
