// Tests for the measuring maths in pi/web/measure.js.   node tests/test_measure.js
const { summarize } = require('../pi/web/measure.js');
let fails = 0;
const check = (ok, msg) => { if (!ok) fails++; console.log((ok ? '  ok:   ' : '  FAIL: ') + msg); };
const near = (a, b, tol) => Math.abs(a - b) <= tol;

console.log('1. a perfectly steady heart at several rates');
for (const bpm of [48, 60, 72, 100, 140, 190]) {
  const gap = 60000 / bpm, beats = Array.from({ length: 14 }, (_, i) => Math.round(5000 + i * gap));
  const r = summarize(beats);
  check(near(r.avg, bpm, 0.15) && r.beats === 14, `${bpm} BPM -> ${r.avg.toFixed(2)} BPM from ${r.beats} beats`);
}

console.log('2. no filtering: an uneven heart is averaged exactly as it beat');
{
  const gaps = [800, 870, 910, 840, 760, 700, 780, 860, 930, 880, 810];          // sinus arrhythmia: about 700-930 ms
  const beats = [10000]; for (const g of gaps) beats.push(beats[beats.length - 1] + g);
  const r = summarize(beats), truth = 60000 * gaps.length / gaps.reduce((a, b) => a + b, 0);
  check(near(r.avg, truth, 1e-9), `average ${r.avg.toFixed(3)} = ${truth.toFixed(3)} BPM (total time / beats)`);
  check(near(r.min, 60000 / 930, 1e-9) && near(r.max, 60000 / 700, 1e-9), `lowest ${r.min.toFixed(1)} and highest ${r.max.toFixed(1)} BPM are the slowest and fastest gaps, unaltered`);
  check(r.rates.length === gaps.length, `${r.rates.length} per-beat rates for ${gaps.length} gaps`);
}

console.log('3. the average does not depend on where in the heartbeat the window started or ended (no "+-1 beat" error)');
{
  const worst = [];
  for (let offset = 0; offset < 833; offset += 37) {                               // 72 BPM, 10 s window starting at any phase
    const beats = []; for (let t = offset; t < offset + 10000; t += 833.33) beats.push(Math.round(t));
    worst.push(Math.abs(summarize(beats).avg - 72));
  }
  check(Math.max(...worst) < 0.25, `worst error over 23 different window positions: ${Math.max(...worst).toFixed(3)} BPM (counting beats in 10 s would be off by up to 6 BPM)`);
}

console.log('4. the Arduino clock wraps around after 49 days: still correct');
{
  const start = 0xFFFFFFFF - 2500, beats = Array.from({ length: 12 }, (_, i) => (start + Math.round(i * 833.33)) >>> 0);
  check(beats[11] < beats[0], 'the beat times really do wrap in this test');
  const r = summarize(beats); check(near(r.avg, 72, 0.15), `average ${r.avg.toFixed(2)} BPM across the wrap`);
}

console.log('5. too few beats: no result');
check(summarize([]) === null && summarize([1000]) === null && summarize([1000, 1800]) === null, 'under 3 beats -> null (the screen says "No pulse found")');
check(summarize([1000, 1800, 2600]) !== null, '3 beats is enough for an average');
check(summarize(null) === null && summarize(undefined) === null, 'no list at all -> null');
check(summarize([5000, 5000, 5000]) === null, 'beats at the same instant -> null, not a divide-by-zero');

console.log(fails ? `\nFAILED (${fails})` : '\nALL PASSED');
process.exit(fails ? 1 : 0);
