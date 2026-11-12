// Host-side tests for the firmware's beat code. Build & run:
//   g++ -O1 -std=c++11 -o /tmp/test_detector tests/test_detector.cpp && /tmp/test_detector
//
// The firmware does no filtering of the heart rate: every pulse is a beat, and the time of every beat is reported.
// These tests check that each pulse is exactly one beat (including through the sensor's few-ms flicker on its edges), that
// the beat TIMES are accurate, and that nothing is reported when there is no pulse.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../arduino/bnts_heartrate/AnalogBeats.h"
#include "../arduino/bnts_heartrate/BeatSensor.h"
#include "../arduino/bnts_heartrate/StatusLed.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } else { printf("  ok:   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---------------------------------------------------------------------------------------------- digital
// A digital pulse train like the real sensor: pulses of `width` ms every `period` ms, with 5 ms flicker on both edges.
static int pulseTrain(uint32_t t, uint32_t t0, double period, uint32_t width) {
  if (t < t0) return 0;
  uint32_t ph = (uint32_t)fmod((double)(t - t0), period);
  auto flicker = [&](uint32_t d, int hi) { return ((d / 5) % 2 == 0) ? hi : !hi; };
  if (ph < 15) return flicker(ph, 1);
  if (ph < width) return 1;
  if (ph < width + 15) return flicker(ph - width, 0);
  return 0;
}

struct DigitalRun { std::vector<uint32_t> beats; int flags = 0; };

// Run `seconds` of a pulse train (start at t0) through BeatSensor; returns every reported beat time.
static DigitalRun runDigital(double seconds, double bpm, uint32_t width, uint32_t t0 = 500) {
  bnts::BeatSensor s; DigitalRun r; double period = 60000.0 / bpm; uint32_t last = 0;
  for (uint32_t t = 0; t < (uint32_t)(seconds * 1000); t++) {
    s.update(pulseTrain(t, t0, period, width), t);
    if (s.takeBeat()) { r.flags++; if (s.lastBeatMs() != last) { r.beats.push_back(s.lastBeatMs()); last = s.lastBeatMs(); } }
  }
  return r;
}
static int pulsesIn(double seconds, double bpm, uint32_t t0 = 500) {
  double period = 60000.0 / bpm; int n = 0;
  for (double s = t0; s < seconds * 1000 - 60; s += period) n++;    // a pulse is reported ~45 ms after it starts
  return n;
}
// The average the display computes: 60000 * (beats - 1) / (last beat - first beat).
static double averageBpm(const std::vector<uint32_t>& b) { return 60000.0 * (b.size() - 1) / (double)(b.back() - b.front()); }

static void digitalTests() {
  printf("1. nothing on the sensor: no beats\n");
  { DigitalRun r = runDigital(10, 1, 0); r.beats.clear(); bnts::BeatSensor s; int f = 0;
    for (uint32_t t = 0; t < 10000; t++) { s.update(false, t); if (s.takeBeat()) f++; }
    CHECK(f == 0, "0 beats from a pin that stays low (%d)", f); }

  printf("2. every pulse is exactly one beat, at any heart rate (flicker on each edge)\n");
  for (double bpm : {40.0, 55.0, 72.0, 90.0, 120.0, 160.0, 200.0}) {
    uint32_t width = (uint32_t)std::min(250.0, 60000.0 / bpm * 0.4);
    DigitalRun r = runDigital(20, bpm, width); int want = pulsesIn(20, bpm);
    CHECK((int)r.beats.size() == want && r.flags == want, "%3.0f BPM: %d beats for %d pulses", bpm, (int)r.beats.size(), want);
  }

  printf("3. the beat times are accurate: average rate from the beat times matches the true rate\n");
  for (double bpm : {48.0, 63.0, 72.0, 88.0, 110.0, 150.0}) {
    uint32_t width = (uint32_t)std::min(250.0, 60000.0 / bpm * 0.4);
    DigitalRun r = runDigital(12, bpm, width); double avg = averageBpm(r.beats);
    CHECK(std::fabs(avg - bpm) < 0.3, "true %5.1f BPM -> measured %6.2f BPM", bpm, avg);
  }

  printf("4. a short pulse (80 ms) is still a beat; the sensor's flicker is not\n");
  { DigitalRun r = runDigital(10, 70, 80); int want = pulsesIn(10, 70); CHECK((int)r.beats.size() == want, "%d beats for %d pulses of 80 ms", (int)r.beats.size(), want); }

  printf("5. pure random flicker for 20 s: no beats\n");
  { bnts::BeatSensor s; int f = 0; srand(5); for (uint32_t t = 0; t < 20000; t++) { s.update(rand() & 1, t); if (s.takeBeat()) f++; }
    CHECK(f == 0, "0 beats (%d)", f); }

  printf("6. heart-rate variation is reported, not hidden: beat gaps that vary +-12%% come through as they are\n");
  { bnts::BeatSensor s; std::vector<double> truth; double t = 600; srand(9); std::vector<uint32_t> got; uint32_t last = 0;
    std::vector<double> starts; for (int i = 0; i < 14; i++) { starts.push_back(t); t += 833 * (1 + 0.12 * (2 * rand() / (double)RAND_MAX - 1)); }
    for (uint32_t ms = 0; ms < 12000; ms++) {
      int pin = 0;
      for (double st : starts) { double d = ms - st; if (d >= 0 && d < 260) pin = (d < 15) ? (((int)(d / 5) % 2 == 0) ? 1 : 0) : (d < 250 ? 1 : (((int)((d - 250) / 5) % 2 == 0) ? 0 : 1)); }
      s.update(pin, ms); if (s.takeBeat() && s.lastBeatMs() != last) { got.push_back(s.lastBeatMs()); last = s.lastBeatMs(); }
    }
    bool same = got.size() == starts.size() - (starts.back() > 11800 ? 1 : 0) || got.size() == starts.size(); double worst = 0;
    for (size_t i = 1; i < got.size() && i < starts.size(); i++) worst = std::max(worst, std::fabs((double)(got[i] - got[i - 1]) - (starts[i] - starts[i - 1])));
    CHECK(same && worst < 3.0, "%d beats; every gap between beats reproduced to within %.1f ms", (int)got.size(), worst); }

  printf("7. synthetic waveform for the display graph\n");
  { bnts::BeatSensor s; uint32_t beatAt = 0; bool seen = false; int peak = 0; uint32_t peakAt = 0; uint16_t late = 0;
    for (uint32_t t = 0; t < 6000; t++) {
      s.update(pulseTrain(t, 500, 833, 250), t);
      if (s.takeBeat()) { beatAt = t; seen = true; }
      if (seen && t > 3000) { int v = s.syntheticSignal(t); uint32_t since = t - beatAt; if (since < 400 && v > peak) { peak = v; peakAt = since; } if (since == 700) late = v; } }
    CHECK(peak > 590 && peakAt > 80 && peakAt < 160, "pulse peaks at %d, %u ms after the beat", peak, peakAt);
    CHECK(late == 512, "flat (512) between beats: %d", late);
    bnts::BeatSensor idle; CHECK(idle.syntheticSignal(5000) == 512, "flat line (512) before any beat"); }
}

// ---------------------------------------------------------------------------------------------- analog
static double gauss(double x, double mu, double sd) { double d = (x - mu) / sd; return std::exp(-d * d); }
static double ppg(double p) { return gauss(p, 0.18, 0.08) + 0.35 * gauss(p, 0.50, 0.09); }

static std::vector<uint32_t> runAnalog(bnts::AnalogBeats& a, uint32_t& t, double& phase, double seconds, double bpm, double amp, double noise, int baseline = 512, bool railed = false) {
  std::vector<uint32_t> beats; uint32_t end = t + (uint32_t)(seconds * 1000);
  for (; t < end; t += 5) {
    double v = baseline + ((double)rand() / RAND_MAX - 0.5) * 2 * noise;
    if (bpm > 0) { phase += (bpm / 60.0) * 0.005; if (phase >= 1) phase -= 1; v += amp * ppg(phase); }
    int raw = (int)v; if (railed) raw = 1023; if (raw < 0) raw = 0; if (raw > 1023) raw = 1023;
    a.update(raw, t); if (a.takeBeat()) beats.push_back(a.lastBeatMs());
  }
  return beats;
}

static void analogTests() {
  srand(7); bnts::AnalogBeats a; uint32_t t = 0; double ph = 0;
  printf("8. analog: an empty sensor (noise only) gives no beats\n");
  { auto b = runAnalog(a, t, ph, 10, 0, 0, 4); CHECK(b.empty(), "%d beats", (int)b.size()); }
  printf("9. analog: one beat per pulse at 72, 118 and 52 BPM, and the beat times give the right average\n");
  for (double bpm : {72.0, 118.0, 52.0}) {
    runAnalog(a, t, ph, 3, bpm, 80, 6);                               // let the threshold settle
    auto b = runAnalog(a, t, ph, 15, bpm, 80, 6); int want = (int)std::round(15.0 * bpm / 60.0);
    double avg = b.size() > 1 ? 60000.0 * (b.size() - 1) / (double)(b.back() - b.front()) : 0;
    CHECK(std::abs((int)b.size() - want) <= 1 && std::fabs(avg - bpm) < 1.5, "%3.0f BPM: %d beats (expect ~%d), average from beat times %.1f", bpm, (int)b.size(), want, avg);
  }
  printf("10. analog: a small signal at a different baseline still works\n");
  { runAnalog(a, t, ph, 3, 90, 35, 3, 380); auto b = runAnalog(a, t, ph, 15, 90, 35, 3, 380); CHECK(b.size() >= 21 && b.size() <= 24, "%d beats in 15 s at 90 BPM", (int)b.size()); }
  printf("11. analog: a sensor pinned at 1023 gives no beats\n");
  { auto b = runAnalog(a, t, ph, 8, 0, 0, 0, 512, true); CHECK(b.empty(), "%d beats", (int)b.size()); }
}

// ---------------------------------------------------------------------------------------------- LED
static void ledTests() {
  printf("12. the LED flashes briefly with each beat and is off otherwise\n");
  bnts::StatusLed led; bool ok = true;
  for (uint32_t t = 0; t < 3000; t++) if (led.update(false, t)) ok = false;
  CHECK(ok, "off for 3 s with no beats");
  std::vector<uint32_t> beats = {3100, 3900, 4700}; int onMs = 0, mism = 0;
  for (uint32_t t = 3000; t < 5500; t++) {
    bool beat = std::find(beats.begin(), beats.end(), t) != beats.end();
    bool on = led.update(beat, t); bool expect = false;
    for (uint32_t b : beats) if (t >= b && t < b + bnts::StatusLed::ON_MS) expect = true;
    if (on) onMs++; if (on != expect) mism++;
  }
  CHECK(mism == 0, "on for exactly %u ms after each beat (mismatches %d)", (unsigned)bnts::StatusLed::ON_MS, mism);
  CHECK(onMs == 3 * (int)bnts::StatusLed::ON_MS, "3 beats -> 3 flashes (%d ms lit in total)", onMs);
}

int main() {
  digitalTests(); analogTests(); ledTests();
  printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
