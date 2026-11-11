/*
  BNTS Heart Rate — Arduino firmware

  Sensor : DFRobot Gravity heart rate sensor (SEN0213)
  Wiring : sensor signal -> A0,  VCC -> 5V (or 3.3V),  GND -> GND

  SENSOR MODE — set this to match the little A/D switch on the sensor:
      SENSOR_MODE_DIGITAL 1   switch on D: the sensor gives one pulse per heartbeat
                              (the graph on the display is then a drawn pulse shape; BPM is real)
      SENSOR_MODE_DIGITAL 0   switch on A: the sensor gives the real pulse waveform

  Output : USB serial, 115200 baud, 50 lines per second:

      H,<signal>,<bpm>,<beat>,<amplitude>

      signal     the pulse waveform 0..1023 (this is the live graph)
      bpm        beats per minute, or 0 when no pulse is detected
      beat       1 on the line where a heartbeat was just detected, else 0
      amplitude  analog: size of the pulse wave (~0-10 = nothing on the sensor)
                 digital: 100 once beats are being heard, else 0

  The Raspberry Pi app reads these lines. Lines starting with '#' are comments.
  Detection logic lives in HeartDetector.h (all tuning knobs are at the top).
*/

#include "HeartDetector.h"

#define SENSOR_MODE_DIGITAL 1     // 1 = switch on D (digital),  0 = switch on A (analog)

const uint8_t  SENSOR_PIN   = A0;
const long     BAUD         = 115200;

#if SENSOR_MODE_DIGITAL
const uint16_t SAMPLE_US    = 1000;  // 1000 samples/s: needed to see through the sensor's chatter
const uint8_t  SEND_EVERY_N = 20;    // -> 50 lines/s to the Pi
#else
const uint16_t SAMPLE_US    = 5000;  // 200 samples/s
const uint8_t  SEND_EVERY_N = 4;     // -> 50 lines/s to the Pi
#endif

bnts::HeartDetector detector;
uint32_t nextSampleUs;
uint8_t  sampleCount = 0;
bool     beatLatch = false;

void setup() {
  Serial.begin(BAUD);
  pinMode(SENSOR_PIN, INPUT);
#if SENSOR_MODE_DIGITAL
  Serial.println(F("# BNTS heart rate v1 (digital sensor mode)"));
#else
  Serial.println(F("# BNTS heart rate v1 (analog sensor mode)"));
#endif
  nextSampleUs = micros() + SAMPLE_US;
}

void loop() {
  uint32_t now = micros();
  if ((int32_t)(now - nextSampleUs) < 0) return;  // not time yet
  nextSampleUs += SAMPLE_US;
  if ((int32_t)(now - nextSampleUs) > 0) nextSampleUs = now + SAMPLE_US;  // fell behind; resync

#if SENSOR_MODE_DIGITAL
  detector.updateDigital(digitalRead(SENSOR_PIN), millis());
#else
  detector.update(analogRead(SENSOR_PIN), millis());
#endif
  if (detector.takeBeat()) beatLatch = true;  // hold until the next line goes out

  if (++sampleCount >= SEND_EVERY_N) {
    sampleCount = 0;
#if SENSOR_MODE_DIGITAL
    const uint16_t sig = detector.syntheticSignal(millis());
#else
    const uint16_t sig = detector.signal;
#endif
    Serial.print(F("H,"));
    Serial.print(sig);
    Serial.print(',');
    Serial.print(detector.bpm);
    Serial.print(',');
    Serial.print(beatLatch ? 1 : 0);
    Serial.print(',');
    Serial.println(detector.amplitude);
    beatLatch = false;
  }
}
