/*
  DFRobot Gravity heart rate sensor - ANALOG mode test using DFRobot's own library

  Needs the "DFRobot_Heartrate" library:
    Arduino IDE > Sketch > Include Library > Manage Libraries... > search "DFRobot_Heartrate" > Install
    (or download it from github.com/DFRobot/DFRobot_Heartrate)

  Wiring:  sensor signal -> HEARTRATE_PIN (A0 below),  VCC -> 5V,  GND -> GND
  The little switch on the sensor must be on  A  (analog) for ANALOG_MODE.
  Serial Monitor: 115200 baud.

  You get two kinds of output:
    "Heart Rate: 72"   printed by the DFRobot library each time it computes a rate
    A "signal" line once a second, so you can SEE whether anything is coming from the sensor:
        signal: now 512   low 480   high 560   swing 80
      swing = how much the reading moved in the last second.
      Under ~20 means no pulse is coming through; over ~20 means there's a pulse to detect.
*/

#include "DFRobot_Heartrate.h"

#define HEARTRATE_PIN A0      // the analog pin your signal wire is on (your sample used A1: match your wiring!)

DFRobot_Heartrate heartrate(ANALOG_MODE);   // use DIGITAL_MODE only if the switch is on D

unsigned long lastReportMs = 0;
unsigned long lastRateMs = 0;
uint8_t lastRate = 0;
int rawLow = 1023, rawHigh = 0;

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println(F("DFRobot heart rate - analog mode test  (Serial Monitor must be at 115200 baud)"));
  Serial.println(F("Rest a fingertip lightly on the sensor and hold still."));
  lastReportMs = millis();
}

void loop() {
  heartrate.getValue(HEARTRATE_PIN);           // feed the library a sample
  uint8_t rate = heartrate.getRate();          // library's computed rate (0 = none right now)

  int raw = analogRead(HEARTRATE_PIN);         // our own look at the signal, for diagnostics
  if (raw < rawLow)  rawLow = raw;
  if (raw > rawHigh) rawHigh = raw;

  if (rate) {
    lastRate = rate;
    lastRateMs = millis();
    Serial.print(F("Heart Rate: "));
    Serial.println(rate);
  }

  unsigned long now = millis();
  if (now - lastReportMs >= 1000) {
    lastReportMs = now;
    int swing = rawHigh - rawLow;
    Serial.print(F("signal: now "));
    Serial.print(raw);
    Serial.print(F("   low "));
    Serial.print(rawLow);
    Serial.print(F("   high "));
    Serial.print(rawHigh);
    Serial.print(F("   swing "));
    Serial.print(swing);
    if (lastRate && now - lastRateMs < 10000) {
      Serial.print(F("   last rate "));
      Serial.print(lastRate);
      Serial.print(F(" BPM"));
    }
    Serial.println();

    if (rawHigh <= 3)         Serial.println(F("  ! Signal is stuck at 0: check power/GND, the signal wire and pin (A0 vs A1), and that the switch is on A."));
    else if (rawLow >= 1020)  Serial.println(F("  ! Signal is stuck at maximum: check the signal wire and that the switch is on A."));
    else if (swing < 20)      Serial.println(F("  . No pulse coming through yet (swing under 20): rest a fingertip lightly on the sensor."));

    rawLow = 1023;
    rawHigh = 0;
  }

  delay(20);
}
