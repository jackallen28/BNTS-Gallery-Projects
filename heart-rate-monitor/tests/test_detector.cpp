// Host-side test for HeartDetector.  Build & run:
//   g++ -O1 -std=c++11 -o /tmp/test_detector tests/test_detector.cpp && /tmp/test_detector
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "../arduino/bnts_heartrate/HeartDetector.h"

static double gauss(double x, double mu, double sd) { double d = (x - mu) / sd; return std::exp(-d * d); }

// Synthetic PPG pulse: systolic peak + smaller dicrotic bump. phase in [0,1).
static double ppg(double p) { return gauss(p, 0.18, 0.08) + 0.35 * gauss(p, 0.50, 0.09); }

struct Result { int bpmAtEnd; int beats; };

// Run `seconds` of signal at `bpm` (bpm<=0 => no finger, noise only). Returns detector state.
static Result run(bnts::HeartDetector& d, uint32_t& t, double& phase, double seconds, double bpm,
                  double amp, double noise, int baseline = 512, bool railed = false) {
  int beats = 0;
  uint32_t end = t + (uint32_t)(seconds * 1000);
  for (; t < end; t += 5) {
    double v = baseline + ((double)rand() / RAND_MAX - 0.5) * 2 * noise;
    if (bpm > 0) {
      phase += (bpm / 60.0) * 0.005;
      if (phase >= 1) phase -= 1;
      v += amp * ppg(phase);
    }
    int raw = (int)v;
    if (railed) raw = 1023;
    if (raw < 0) raw = 0; if (raw > 1023) raw = 1023;
    d.update(raw, t);
    if (d.takeBeat()) beats++;
  }
  return {d.bpm, beats};
}

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } else { printf("  ok:   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main() {
  srand(7);
  bnts::HeartDetector d; uint32_t t = 0; double ph = 0;

  printf("1. no finger (ambient noise only) for 10 s\n");
  Result r = run(d, t, ph, 10, 0, 0, 4);
  CHECK(r.bpmAtEnd == 0 && r.beats == 0, "bpm=%d beats=%d (expect 0/0)", r.bpmAtEnd, r.beats);

  printf("2. finger on, 72 BPM, amplitude 80, noise 6\n");
  r = run(d, t, ph, 4, 72, 80, 6);
  printf("     (after 4 s) bpm=%d\n", r.bpmAtEnd);
  r = run(d, t, ph, 16, 72, 80, 6);
  CHECK(std::abs(r.bpmAtEnd - 72) <= 2, "bpm=%d (expect 72 +-2)", r.bpmAtEnd);
  CHECK(r.beats >= 18 && r.beats <= 21, "beats in 16 s = %d (expect ~19)", r.beats);

  printf("3. rate rises to 118 BPM\n");
  r = run(d, t, ph, 14, 118, 80, 6);
  CHECK(std::abs(r.bpmAtEnd - 118) <= 3, "bpm=%d (expect 118 +-3)", r.bpmAtEnd);

  printf("4. rate drops to 52 BPM\n");
  r = run(d, t, ph, 20, 52, 80, 6);
  CHECK(std::abs(r.bpmAtEnd - 52) <= 3, "bpm=%d (expect 52 +-3)", r.bpmAtEnd);

  printf("5. finger removed -> bpm must return to 0 within 4 s\n");
  r = run(d, t, ph, 4, 0, 0, 4);
  CHECK(r.bpmAtEnd == 0, "bpm=%d (expect 0)", r.bpmAtEnd);

  printf("6. finger back on at a different baseline (380), small amplitude 35, 90 BPM\n");
  r = run(d, t, ph, 15, 90, 35, 3, 380);
  CHECK(std::abs(r.bpmAtEnd - 90) <= 3, "bpm=%d (expect 90 +-3)", r.bpmAtEnd);

  printf("7. sensor pinned at 1023 for 8 s -> no reading\n");
  r = run(d, t, ph, 8, 0, 0, 0, 512, true);
  CHECK(r.bpmAtEnd == 0, "bpm=%d (expect 0)", r.bpmAtEnd);

  // ---------------------------------------------------------------- digital mode
  printf("\n--- DIGITAL mode (1 kHz pin samples, chatter on pulse edges like the real sensor) ---\n");
  bnts::HeartDetector dd; uint32_t td = 0;
  struct Scenario { int kind; double bpm; int widthMs; };   // kind: 0 idle low, 1 pulses, 2 random flicker
  auto runD = [&](double seconds, Scenario sc, int* beatsOut) {
    int beats = 0; uint32_t end = td + (uint32_t)(seconds * 1000); uint32_t t0 = td;
    for (; td < end; td++) {
      int pin = 0;
      if (sc.kind == 1) {
        uint32_t period = (uint32_t)(60000.0 / sc.bpm), ph = (td - t0) % period;
        auto chatter = [&](uint32_t d, int toHigh) { return ((d / 5) % 2 == 0) ? toHigh : !toHigh; };
        if (ph < 15) pin = chatter(ph, 1);
        else if (ph < (uint32_t)sc.widthMs) pin = 1;
        else if (ph < (uint32_t)sc.widthMs + 15) pin = chatter(ph - sc.widthMs, 0);
        else pin = 0;
      } else if (sc.kind == 2) pin = rand() & 1;
      dd.updateDigital(pin, td);
      if (dd.takeBeat()) beats++;
    }
    if (beatsOut) *beatsOut = beats;
  };
  int nb = 0;

  printf("8. idle (pin low) for 5 s\n");
  runD(5, {0, 0, 0}, &nb);
  CHECK(dd.bpm == 0 && nb == 0, "bpm=%d beats=%d (expect 0/0)", dd.bpm, nb);

  printf("9. finger on: 72 BPM, 250 ms pulses with edge chatter\n");
  runD(12, {1, 72, 250}, &nb);
  CHECK(std::abs(dd.bpm - 72) <= 2, "bpm=%d (expect 72 +-2)", dd.bpm);
  CHECK(nb >= 12 && nb <= 15, "beats flagged = %d (expect ~13, chatter must not double-count)", nb);

  printf("10. synthetic waveform for the display graph\n");
  {
    // find the moment right after a beat: restart a clean detector and read the shape around a beat
    bnts::HeartDetector sd; uint32_t ts = 0; uint32_t beatAt = 0; int peak = 0, atPeakMs = 0; uint16_t late = 0; bool seen = false;
    for (; ts < 12000; ts++) {
      uint32_t ph = ts % 833; int pin = ph < 250;
      sd.updateDigital(pin, ts);
      if (sd.takeBeat()) { beatAt = ts; seen = true; }
      if (seen && sd.bpm > 0 && ts > 9000) {
        int v = sd.syntheticSignal(ts); uint32_t since = ts - beatAt;
        if (since < 400 && v > peak) { peak = v; atPeakMs = since; }
        if (since == 700) late = v;
      }
    }
    CHECK(peak > 590 && atPeakMs > 80 && atPeakMs < 160, "pulse peaks at %d, %d ms after the beat (expect >590, 80-160 ms)", peak, atPeakMs);
    CHECK(late == 512, "back to baseline (512) between beats: %d", late);
    bnts::HeartDetector blank;
    CHECK(blank.syntheticSignal(5000) == 512, "flat line (512) when there is no reading");
  }

  printf("11. rate rises to 100 BPM, 200 ms pulses\n");
  runD(14, {1, 100, 200}, &nb);
  CHECK(std::abs(dd.bpm - 100) <= 3, "bpm=%d (expect 100 +-3)", dd.bpm);

  printf("12. finger removed -> bpm must return to 0 within 4 s\n");
  runD(4, {0, 0, 0}, &nb);
  CHECK(dd.bpm == 0, "bpm=%d (expect 0)", dd.bpm);

  printf("13. pure random flicker (no finger) for 15 s -> no reading\n");
  runD(15, {2, 0, 0}, &nb);
  CHECK(dd.bpm == 0, "bpm=%d (expect 0)", dd.bpm);

  printf("14. slow rate: 52 BPM, 300 ms pulses\n");
  runD(20, {1, 52, 300}, &nb);
  CHECK(std::abs(dd.bpm - 52) <= 3, "bpm=%d (expect 52 +-3)", dd.bpm);

  printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
