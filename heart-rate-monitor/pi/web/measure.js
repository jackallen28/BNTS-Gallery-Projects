/* The measuring maths, kept apart from the display so it can be tested (tests/test_measure.js).
 *
 * No filtering: every beat the Arduino reports is used. `beatMs` is a list of beat times from the Arduino's own
 * millisecond clock, oldest first. That clock is 32 bits and wraps after about 49 days, so differences use
 * unsigned arithmetic.
 *
 * The average heart rate is the true average over the whole measurement:
 *     60000 x (number of beats - 1) / (time from the first beat to the last beat)
 * Unlike counting the beats in a fixed window it has no "+-1 beat" error, and because the beat times come from the
 * Arduino's clock, delays on the USB cable or in the browser cannot affect it.
 */
(function (root) {
  const gap = (later, earlier) => (later - earlier) >>> 0;

  // Returns null if there are fewer than 3 beats (2 gaps), otherwise the numbers the display shows.
  function summarize(beatMs) {
    if (!beatMs || beatMs.length < 3) return null;
    const n = beatMs.length;
    const spanMs = gap(beatMs[n - 1], beatMs[0]);
    if (spanMs <= 0) return null;
    const rates = [];                                   // the heart rate at each beat, from the gap before it
    for (let i = 1; i < n; i++) {
      const g = gap(beatMs[i], beatMs[i - 1]);
      if (g > 0) rates.push(60000 / g);
    }
    if (!rates.length) return null;
    return {
      beats: n,
      spanMs,
      avg: 60000 * (n - 1) / spanMs,
      rates,
      min: Math.min.apply(null, rates),
      max: Math.max.apply(null, rates),
    };
  }

  const api = { summarize };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  root.BNTSMeasure = api;
})(typeof window !== 'undefined' ? window : globalThis);
