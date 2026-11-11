/*
  BNTS Heart Rate — Arduino firmware

  Sensor : DFRobot Gravity heart rate sensor (SEN0213)

  SENSOR MODE — set this to match the little A/D switch on the sensor:

      SENSOR_MODE_DIGITAL 1   switch on D.   Wiring: signal -> D2,  VCC -> 5V,  GND -> GND
                              One pulse per heartbeat. A finger is confirmed after 3 s of steady
                              beats (the first reading is ready at that moment), then the reading
                              refreshes every 3 s. Odd readings are skipped and cross-checked.
                              (The graph on the display is a drawn pulse shape; the BPM is real.)

      SENSOR_MODE_DIGITAL 0   switch on A.   Wiring: signal -> A0,  VCC -> 5V,  GND -> GND
                              The real pulse waveform on the graph.

  Output : USB serial, 115200 baud, 50 lines per second:

      H,<signal>,<bpm>,<beat>,<amplitude>

      signal     the pulse waveform 0..1023 (this is the live graph)
      bpm        beats per minute, or 0 when there is no reading yet / no finger
      beat       1 on the line where a heartbeat was just detected, else 0
      amplitude  analog: size of the pulse wave (~0-10 = nothing on the sensor)
                 digital: 0 = nothing heard, 100 = beats are being heard

  Lines starting with '#' are human-readable status messages (the Raspberry Pi ignores them), e.g.
      # [ 12 s] Finger confirmed. READING: 72 BPM
  Open the Serial Monitor at 115200 baud to read them. Set STATUS_MESSAGES to 0 to silence them.

  Detection logic lives in StagedDetector.h (digital) and HeartDetector.h (analog); every tuning knob is
  at the top of those files.
*/

#include "HeartDetector.h"
#include "StagedDetector.h"

#define SENSOR_MODE_DIGITAL 1     // 1 = switch on D (digital),  0 = switch on A (analog)
#define STATUS_MESSAGES     1     // 1 = print "# ..." status lines (digital mode)

const long BAUD = 115200;

#if SENSOR_MODE_DIGITAL
const uint8_t  SENSOR_PIN   = 2;
const uint16_t SAMPLE_US    = 1000;  // 1000 samples/s: needed to see through the sensor's chatter
const uint8_t  SEND_EVERY_N = 20;    // -> 50 lines/s to the Pi
bnts::StagedDetector detector;
#else
const uint8_t  SENSOR_PIN   = A0;
const uint16_t SAMPLE_US    = 5000;  // 200 samples/s
const uint8_t  SEND_EVERY_N = 4;     // -> 50 lines/s to the Pi
bnts::HeartDetector detector;
#endif

uint32_t nextSampleUs;
uint8_t  sampleCount = 0;
bool     beatLatch = false;

#if SENSOR_MODE_DIGITAL && STATUS_MESSAGES
void printStatusMessages() {
  bnts::StagedDetector::Event e;
  int a, b;
  while (detector.takeEvent(e, a, b)) {
    Serial.print(F("# ["));
    Serial.print(millis() / 1000);
    Serial.print(F(" s] "));
    switch (e) {
      case bnts::StagedDetector::EV_SOMETHING:
        Serial.println(F("Something detected - checking it's a steady pulse...")); break;
      case bnts::StagedDetector::EV_NOT_STEADY:
        Serial.println(F("Not a steady pulse - ignoring it.")); break;
      case bnts::StagedDetector::EV_CONFIRMED:
        Serial.print(F("Finger confirmed. READING: ")); Serial.print(a); Serial.println(F(" BPM")); break;
      case bnts::StagedDetector::EV_UPDATE:
        Serial.print(F("Update: ")); Serial.print(a); Serial.println(F(" BPM")); break;
      case bnts::StagedDetector::EV_ODD:
        Serial.print(F("Odd reading (")); Serial.print(a);
        Serial.print(F(" BPM) - skipped, checking the next one. Still showing ")); Serial.print(b);
        Serial.println(F(" BPM")); break;
      case bnts::StagedDetector::EV_GLITCH:
        Serial.print(F("Earlier odd reading (")); Serial.print(a); Serial.println(F(" BPM) was a glitch - ignored.")); break;
      case bnts::StagedDetector::EV_REAL:
        Serial.print(F("Two readings agree - the change is real. Update: ")); Serial.print(a); Serial.println(F(" BPM")); break;
      case bnts::StagedDetector::EV_NO_CLEAR:
        Serial.print(F("No clear reading this time - keeping ")); Serial.print(a); Serial.println(F(" BPM")); break;
      case bnts::StagedDetector::EV_RESTART:
        Serial.println(F("Readings unsteady - checking the finger again.")); break;
      case bnts::StagedDetector::EV_REMOVED:
        Serial.println(F("Finger removed.")); break;
      default: Serial.println(); break;
    }
  }
}
#endif

void setup() {
  Serial.begin(BAUD);
  pinMode(SENSOR_PIN, INPUT);
#if SENSOR_MODE_DIGITAL
  Serial.println(F("# BNTS heart rate v2 (digital sensor mode, pin D2)"));
#else
  Serial.println(F("# BNTS heart rate v2 (analog sensor mode, pin A0)"));
#endif
  nextSampleUs = micros() + SAMPLE_US;
}

void loop() {
  uint32_t now = micros();
  if ((int32_t)(now - nextSampleUs) < 0) return;  // not time yet
  nextSampleUs += SAMPLE_US;
  if ((int32_t)(now - nextSampleUs) > 0) nextSampleUs = now + SAMPLE_US;  // fell behind; resync

#if SENSOR_MODE_DIGITAL
  detector.update(digitalRead(SENSOR_PIN), millis());
#if STATUS_MESSAGES
  printStatusMessages();
#endif
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
