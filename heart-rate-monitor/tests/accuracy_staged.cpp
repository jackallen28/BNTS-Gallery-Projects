// How accurate is the FIRST reading (4th steady beat) when beat gaps vary like a real heart?
//   g++ -O2 -std=c++11 -o /tmp/accuracy tests/accuracy_staged.cpp && /tmp/accuracy [variation, e.g. 0.08]
// Exits non-zero if 95% of first readings are not within 2 BPM of the beats they used (variation up to 8%), or if
// verification is slower than 3.5 s for 90% of trials.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../arduino/bnts_heartrate/StagedDetector.h"

int main(int argc, char** argv) {
  double jitter = argc > 1 ? atof(argv[1]) : 0.08;
  srand(11);
  std::vector<double> errs, times; int failed = 0, trials = 400;
  for (int k = 0; k < trials; k++) {
    double meanBpm = 55 + (rand() / (double)RAND_MAX) * 60;           // 55..115 BPM
    double meanGap = 60000.0 / meanBpm;
    // beat times with per-beat variation
    std::vector<double> beat; double t = 400 + rand() % 600;
    for (int i = 0; i < 30; i++) { beat.push_back(t); t += meanGap * (1 + jitter * (2 * rand() / (double)RAND_MAX - 1)); }
    bnts::StagedDetector det; int firstReading = 0; uint32_t liveAt = 0; size_t used = 0;
    for (uint32_t ms = 0; ms < 12000; ms++) {
      int pin = 0;
      for (size_t i = 0; i < beat.size(); i++) {
        double d = ms - beat[i]; if (d < 0) break;
        double width = std::min(250.0, meanGap * 0.4);
        if (d < width + 15) {
          auto ch = [&](double dd, int hi) { return ((int)(dd / 5) % 2 == 0) ? hi : !hi; };
          pin = d < 15 ? ch(d, 1) : (d < width ? 1 : ch(d - width, 0));
        }
      }
      det.update(pin, ms);
      if (!firstReading && det.bpm > 0) { firstReading = det.bpm; liveAt = ms; for (size_t i = 0; i < beat.size(); i++) if (beat[i] <= ms) used = i + 1; }
    }
    if (!firstReading) { failed++; continue; }
    // the true rate over exactly the beats the reading could have used
    double sum = 0; size_t n = 0; for (size_t i = 1; i < used; i++) { sum += beat[i] - beat[i - 1]; n++; }
    double trueBpm = 60000.0 / (sum / n);
    errs.push_back(fabs(firstReading - trueBpm)); times.push_back((liveAt - beat[0]) / 1000.0);
  }
  std::sort(errs.begin(), errs.end()); std::sort(times.begin(), times.end());
  auto pct = [&](std::vector<double>& v, double p) { return v[(size_t)(p * (v.size() - 1))]; };
  printf("gap variation +-%.0f%%, %d trials, %d never verified\n", jitter * 100, trials, failed);
  printf("  first-reading error vs the true rate of the beats used:  median %.1f BPM, 90%% within %.1f, 95%% within %.1f, worst %.1f\n",
         pct(errs, .5), pct(errs, .9), pct(errs, .95), errs.back());
  printf("  time from first beat to verified:                         median %.1f s, 90%% within %.1f s, worst %.1f s\n",
         pct(times, .5), pct(times, .9), times.back());
  bool good = failed == 0 && (jitter > 0.0801 || (pct(errs, .95) <= 2.0 && pct(times, .9) <= 3.5));
  printf(good ? "PASSED\n" : "FAILED\n");
  return good ? 0 : 1;
}
