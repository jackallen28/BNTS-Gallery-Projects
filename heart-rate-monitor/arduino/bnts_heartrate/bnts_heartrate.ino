/*
  BNTS Heart Rate — Arduino firmware

  Sensor : DFRobot Gravity analog heart rate sensor (SEN0213)
  Wiring : sensor signal -> A0,  VCC -> 5V (or 3.3V),  GND -> GND
  Output : USB serial, 115200 baud, 50 lines per second:

      H,<signal>,<bpm>,<beat>,<amplitude>

      signal     filtered sensor value 0..1023 (this is the live pulse waveform)
      bpm        beats per minute, or 0 when no pulse is detected
      beat       1 on the line where a heartbeat was just detected, else 0
      amplitude  size of the pulse wave; ~0-10 = nothing on the sensor

  The Raspberry Pi app reads these lines. Lines starting with '#' are comments.
  Detection logic lives in HeartDetector.h (all tuning knobs are at the top).
*/

#include "HeartDetector.h"

const uint8_t  SENSOR_PIN   = A0;
const long     BAUD         = 115200;
const uint16_t SAMPLE_US    = 5000;  // 200 samples/s
const uint8_t  SEND_EVERY_N = 4;     // -> 50 lines/s to the Pi

bnts::HeartDetector detector;
uint32_t nextSampleUs;
uint8_t  sampleCount = 0;
bool     beatLatch = false;

void setup() {
  Serial.begin(BAUD);
  pinMode(SENSOR_PIN, INPUT);
  Serial.println(F("# BNTS heart rate v1"));
  nextSampleUs = micros() + SAMPLE_US;
}

void loop() {
  uint32_t now = micros();
  if ((int32_t)(now - nextSampleUs) < 0) return;  // not time yet
  nextSampleUs += SAMPLE_US;
  if ((int32_t)(now - nextSampleUs) > 0) nextSampleUs = now + SAMPLE_US;  // fell behind; resync

  detector.update(analogRead(SENSOR_PIN), millis());
  if (detector.takeBeat()) beatLatch = true;  // hold until the next line goes out

  if (++sampleCount >= SEND_EVERY_N) {
    sampleCount = 0;
    Serial.print(F("H,"));
    Serial.print(detector.signal);
    Serial.print(',');
    Serial.print(detector.bpm);
    Serial.print(',');
    Serial.print(beatLatch ? 1 : 0);
    Serial.print(',');
    Serial.println(detector.amplitude);
    beatLatch = false;
  }
}
