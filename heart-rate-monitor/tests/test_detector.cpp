// Host-side test for HeartDetector.  Build & run:
//   g++ -O1 -std=c++11 -o /tmp/test_detector tests/test_detector.cpp && /tmp/test_detector
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include "../arduino/bnts_heartrate/HeartDetector.h"
#include "../arduino/bnts_heartrate/StagedDetector.h"

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

  // ---------------------------------------------------------------- digital mode (staged)
  printf("\n--- DIGITAL mode: StagedDetector (1 kHz pin samples, chatter on pulse edges like the real sensor) ---\n");
  using SD = bnts::StagedDetector;

  struct Seg { double fromS, toS; int kind; double bpm; int widthMs; };   // kind 0 low, 1 pulses, 2 random flicker
  struct Trace { std::vector<int> bpmAt; std::vector<uint32_t> updateTimes; int beats = 0; uint32_t liveAtMs = 0; };

  auto simulate = [&](const std::vector<Seg>& segs, double totalS) {
    SD det; Trace tr;
    for (uint32_t t = 0; t < (uint32_t)(totalS * 1000); t++) {
      int pin = 0;
      for (const Seg& sg : segs) {
        if (t < (uint32_t)(sg.fromS * 1000) || t >= (uint32_t)(sg.toS * 1000)) continue;
        if (sg.kind == 1) {
          uint32_t t0 = (uint32_t)(sg.fromS * 1000), period = (uint32_t)(60000.0 / sg.bpm), ph = (t - t0) % period;
          auto ch = [&](uint32_t d, int hi) { return ((d / 5) % 2 == 0) ? hi : !hi; };
          if (ph < 15) pin = ch(ph, 1); else if (ph < (uint32_t)sg.widthMs) pin = 1;
          else if (ph < (uint32_t)sg.widthMs + 15) pin = ch(ph - sg.widthMs, 0); else pin = 0;
        } else if (sg.kind == 2) pin = rand() & 1;
      }
      det.update(pin, t);
      if (det.takeBeat()) tr.beats++;
      SD::Event e; int a, b;
      while (det.takeEvent(e, a, b)) if (e == SD::EV_CONFIRMED || e == SD::EV_UPDATE || e == SD::EV_REAL) tr.updateTimes.push_back(t);
      if (det.bpm > 0 && tr.liveAtMs == 0) tr.liveAtMs = t;
      tr.bpmAt.push_back(det.bpm);
    }
    return tr;
  };
  auto maxBpm = [](const Trace& tr, double fromS, double toS) {
    int m = 0; for (size_t i = (size_t)(fromS * 1000); i < tr.bpmAt.size() && i < (size_t)(toS * 1000); i++) m = std::max(m, tr.bpmAt[i]); return m; };

  printf("8. nothing on the sensor for 8 s\n");
  { Trace tr = simulate({}, 8); CHECK(maxBpm(tr, 0, 8) == 0 && tr.beats == 0, "bpm stays 0, no beats flagged"); }

  printf("9. a brief 2.2 s touch must NOT be treated as a finger\n");
  { Trace tr = simulate({{2, 4.2, 1, 72, 250}}, 12); CHECK(maxBpm(tr, 0, 12) == 0, "bpm stayed 0 (max seen %d)", maxBpm(tr, 0, 12)); }

  printf("10. finger on at 72 BPM: confirmed after ~3 s with the reading ready at once\n");
  { Trace tr = simulate({{2, 30, 1, 72, 250}}, 30);
    double confirmAfter = (tr.liveAtMs - 2000) / 1000.0;
    CHECK(confirmAfter >= 2.9 && confirmAfter <= 4.8, "first reading %.1f s after the finger arrived (expect 3-4.8)", confirmAfter);
    CHECK(std::abs(tr.bpmAt[tr.liveAtMs] - 72) <= 2, "first reading = %d BPM (expect 72 +-2)", tr.bpmAt[tr.liveAtMs]);
    CHECK(std::abs(tr.bpmAt[29000] - 72) <= 2, "still %d BPM after 28 s", tr.bpmAt[29000]);
    CHECK(tr.beats >= 30 && tr.beats <= 36, "beat flags = %d in 28 s (expect ~34; chatter must not double count)", tr.beats);
    bool spaced = true; for (size_t i = 1; i < tr.updateTimes.size(); i++) if (tr.updateTimes[i] - tr.updateTimes[i - 1] < 2990) spaced = false;
    CHECK(spaced && tr.updateTimes.size() >= 8, "%d updates, never closer than 3 s apart", (int)tr.updateTimes.size()); }

  printf("11. a 3.3 s glitch at 112 BPM is dropped (tried at 5 different positions)\n");
  { bool ok = true; int worst = 0;
    for (double off : {29.0, 29.6, 30.2, 30.8, 31.4}) {
      Trace tr = simulate({{2, off, 1, 72, 250}, {off, off + 3.3, 1, 112, 180}, {off + 3.3, 60, 1, 72, 250}}, 60);
      int m = maxBpm(tr, 20, 60); worst = std::max(worst, m); if (m > 72 + SD::OUTLIER_BPM + 3) ok = false; }
    CHECK(ok, "highest reading shown while glitching = %d BPM (expect <= 90: drift within the 15 BPM tolerance is fine, 112 never shown)", worst); }

  printf("12. a sustained change to 100 BPM is accepted within 12 s\n");
  { Trace tr = simulate({{2, 30, 1, 72, 250}, {30, 70, 1, 100, 200}}, 70);
    int at = 0; for (int s = 30; s < 70; s++) if (std::abs(tr.bpmAt[s * 1000] - 100) <= 3) { at = s; break; }
    CHECK(at > 0 && at <= 42, "showing ~100 BPM from t=%d s (change began at 30 s)", at);
    CHECK(std::abs(tr.bpmAt[69000] - 100) <= 3, "final reading %d BPM", tr.bpmAt[69000]); }

  printf("13. finger removed: bpm back to 0 within 5 s\n");
  { Trace tr = simulate({{2, 25, 1, 72, 250}}, 31);
    CHECK(tr.bpmAt[24500] > 0 && tr.bpmAt[30500] == 0, "bpm %d just before removal, %d five seconds later", tr.bpmAt[24500], tr.bpmAt[30500]);
    CHECK(tr.bpmAt[26300] > 0, "reading still held 1.3+ s after the finger left (%d BPM): the 3 s adjusting window", tr.bpmAt[26300]);
    CHECK(tr.bpmAt[28800] == 0, "reading cleared once 3 s have passed without a beat (%d)", tr.bpmAt[28800]); }

  printf("13b. finger lifted and put back within 2 s (adjusting) must not drop the reading\n");
  { Trace tr = simulate({{2, 20, 1, 72, 250}, {22, 45, 1, 72, 250}}, 45);
    int lowest = 999; for (int ms = 12000; ms < 45000; ms++) lowest = std::min(lowest, tr.bpmAt[ms]);
    CHECK(lowest > 0, "bpm never dropped to 0 during the adjustment (lowest %d)", lowest);
    CHECK(std::abs(tr.bpmAt[44000] - 72) <= 3, "still %d BPM afterwards", tr.bpmAt[44000]); }

  printf("14. slow pulse, 50 BPM (300 ms pulses)\n");
  { Trace tr = simulate({{2, 40, 1, 50, 300}}, 40);
    double confirmAfter = (tr.liveAtMs - 2000) / 1000.0;
    CHECK(tr.liveAtMs > 0 && confirmAfter <= 6.0, "confirmed %.1f s after the finger arrived", confirmAfter);
    CHECK(std::abs(tr.bpmAt[39000] - 50) <= 2, "reading %d BPM (expect 50 +-2)", tr.bpmAt[39000]); }

  printf("15. pure random flicker (no finger) for 20 s -> never a reading\n");
  { Trace tr = simulate({{1, 21, 2, 0, 0}}, 22); CHECK(maxBpm(tr, 0, 22) == 0, "bpm stayed 0 (max %d)", maxBpm(tr, 0, 22)); }

  printf("16. synthetic waveform for the display graph\n");
  { SD det; uint32_t beatAt = 0; bool seen = false; int peak = 0; uint32_t peakAt = 0; uint16_t late = 0;
    for (uint32_t t = 0; t < 14000; t++) {
      uint32_t ph = t % 833; det.update(ph < 250, t);
      if (det.takeBeat()) { beatAt = t; seen = true; }
      if (seen && det.bpm > 0 && t > 9000) {
        int v = det.syntheticSignal(t); uint32_t since = t - beatAt;
        if (since < 400 && v > peak) { peak = v; peakAt = since; }
        if (since == 700) late = v; } }
    CHECK(peak > 590 && peakAt > 80 && peakAt < 160, "pulse peaks at %d, %u ms after the beat (expect >590, 80-160 ms)", peak, peakAt);
    CHECK(late == 512, "back to baseline (512) between beats: %d", late);
    SD idle; CHECK(idle.syntheticSignal(5000) == 512 && idle.amplitude == 0, "flat line and amplitude 0 when idle"); }

  printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
