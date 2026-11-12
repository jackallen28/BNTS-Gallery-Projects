/*
  BNTS Heart Rate — Arduino firmware

  Sensor : DFRobot Gravity heart rate sensor (SEN0213)

  What it does: it reports every beat the sensor produces, with the exact time of each beat. It does NOT filter or judge
  the heart rate. The Raspberry Pi starts a measurement when the screen is pressed and works out the average heart rate
  from the beat times it receives.

  SENSOR MODE — set this to match the little A/D switch on the sensor:

      SENSOR_MODE_DIGITAL 1   switch on D.   Wiring: signal -> D2,  VCC -> 5V,  GND -> GND
                              One pulse per heartbeat. (The only processing is smoothing away the sensor's few-millisecond
                              flicker at the start and end of each pulse, so that each pulse counts as exactly one beat.)
                              The graph on the display is a drawn pulse shape triggered by each real beat.

      SENSOR_MODE_DIGITAL 0   switch on A.   Wiring: signal -> A0,  VCC -> 5V,  GND -> GND
                              The real pulse waveform on the graph; a beat is each rise above a threshold.

  Output : USB serial, 115200 baud, 50 lines per second:

      H,<signal>,<beat>,<beatMs>

      signal    the pulse waveform 0..1023 (the live graph)
      beat      1 on the line where a beat was just detected, else 0
      beatMs    the Arduino's millisecond clock at the most recent beat (it changes with every new beat; the Pi uses these
                times, not the time the lines arrive, so serial and USB delays can't affect the average)

  LED (pin 13, the little LED on the board): flashes briefly with every beat the Arduino detects. Put a finger on the sensor:
  if the LED flashes in time with your pulse the Arduino is working; if it never flashes, the Arduino isn't detecting a pulse
  (check the wiring, the pin and the A/D switch). If it flashes but the Pi doesn't react, run ./check-arduino.sh on the Pi.
*/

#include "AnalogBeats.h"
#include "BeatSensor.h"
#include "StatusLed.h"

#define SENSOR_MODE_DIGITAL 1     // 1 = switch on D (digital),  0 = switch on A (analog)

const long BAUD = 115200;

#if SENSOR_MODE_DIGITAL
const uint8_t  SENSOR_PIN   = 2;
const uint16_t SAMPLE_US    = 1000;  // 1000 samples/s: needed to see through the sensor's flicker
const uint8_t  SEND_EVERY_N = 20;    // -> 50 lines/s to the Pi
bnts::BeatSensor sensor;
#else
const uint8_t  SENSOR_PIN   = A0;
const uint16_t SAMPLE_US    = 5000;  // 200 samples/s
const uint8_t  SEND_EVERY_N = 4;     // -> 50 lines/s to the Pi
bnts::AnalogBeats sensor;
#endif

bnts::StatusLed statusLed;
uint32_t nextSampleUs;
uint8_t  sampleCount = 0;
bool     beatLatch = false;

void setup() {
  Serial.begin(BAUD);
  pinMode(SENSOR_PIN, INPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
#if SENSOR_MODE_DIGITAL
  Serial.println(F("# BNTS heart rate v3 (digital sensor mode, pin D2): reports every beat"));
#else
  Serial.println(F("# BNTS heart rate v3 (analog sensor mode, pin A0): reports every beat"));
#endif
  nextSampleUs = micros() + SAMPLE_US;
}

void loop() {
  uint32_t now = micros();
  if ((int32_t)(now - nextSampleUs) < 0) return;  // not time yet
  nextSampleUs += SAMPLE_US;
  if ((int32_t)(now - nextSampleUs) > 0) nextSampleUs = now + SAMPLE_US;  // fell behind; resync

#if SENSOR_MODE_DIGITAL
  sensor.update(digitalRead(SENSOR_PIN), millis());
#else
  sensor.update(analogRead(SENSOR_PIN), millis());
#endif
  bool beat = sensor.takeBeat();
  if (beat) beatLatch = true;                     // hold until the next line goes out
  digitalWrite(LED_BUILTIN, statusLed.update(beat, millis()) ? HIGH : LOW);   // flash with every beat

  if (++sampleCount >= SEND_EVERY_N) {
    sampleCount = 0;
#if SENSOR_MODE_DIGITAL
    const uint16_t sig = sensor.syntheticSignal(millis());
#else
    const uint16_t sig = sensor.signal;
#endif
    Serial.print(F("H,"));
    Serial.print(sig);
    Serial.print(',');
    Serial.print(beatLatch ? 1 : 0);
    Serial.print(',');
    Serial.println(sensor.lastBeatMs());
    beatLatch = false;
  }
}
