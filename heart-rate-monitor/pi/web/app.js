/* BNTS heart rate display.
 * Listens to /events (from server.py) and draws four screens:
 *   - STANDBY: animated pixel heart + rippling pixel grid, shown while nobody is touching the sensor
 *   - DETECT:  "Finger detected" - the heart beats with every real beat and a pixel lights for each, while the heart rate is verified
 *   - LIVE:    big BPM number and a scrolling pulse graph
 *   - SUMMARY: 5 seconds of results (average, lowest, highest, time) after the finger comes off
 * Add ?debug to the URL for an on-screen readout.
 */
(() => {
  'use strict';

  // ---- brand palette -------------------------------------------------------
  const TEAL = '#19c1ca', TEAL2 = '#04bbc1', PURPLE = '#704988', LAV = '#cfc2e0';
  const HEART_COLOURS = [TEAL, PURPLE, LAV, TEAL2];

  // ---- settings you might want to tweak -----------------------------------------
  const FRAME_MS        = 33;    // ~30 fps keeps the Pi 3 cool
  const WAVE_SECONDS    = 6;     // width of the scrolling graph
  const SAMPLE_HZ       = 50;    // must match the Arduino
  const BPM_ON_AFTER    = 3;     // consecutive valid readings before switching to LIVE
  const PULSE_HINT_AMP  = 25;    // amplitude at or above this = beats are being heard
  const END_AFTER_MS    = 300;   // finger gone this long (the Arduino has already waited 3 s) -> results screen
  const HOLD_MAX_MS     = 12000; // safety: beats but no reading for this long also ends the session
  const SUMMARY_MS      = 5000;  // how long the results screen stays up
  const SESSION_SAMPLE_MS = 3000;// the readings are recorded this often for the results
  const CONFIRM_SECONDS = 2;     // roughly how long the Arduino takes to verify a finger before the first reading
  const DETECT_LOST_MS  = 400;   // the finger signal gone this long while verifying -> back to standby with a hint
  const HINT_MS         = 3500;  // how long the "keep holding" hint stays up after a finger is lifted too early
  const WAVE_POINTS     = WAVE_SECONDS * SAMPLE_HZ;

  const $ = id => document.getElementById(id);
  const params = new URLSearchParams(location.search);
  if (params.has('debug')) document.body.classList.add('debug');

  // ---- state ---------------------------------------------------------------
  const S = {
    mode: 'standby', modeAt: 0,
    wave: [],
    bpm: 0, shownBpm: 0,
    validRun: 0, lastValid: 0,
    beatAt: -99999, amp: 0, stage: 0, progress: 0, hasProgress: false, lastHeard: 0, stageLowSince: 0, hintUntil: 0,
    session: { samples: [], startAt: 0, nextSampleAt: 0 }, summary: null, summaryUntil: 0,
    yMin: 480, yMax: 540,
    connected: false, receiving: false, simulated: false,
    version: '', reloadPending: false, wrong: false,
    msgs: 0,
  };

  // ---- data stream ---------------------------------------------------------
  function connect() {
    const es = new EventSource('/events');
    es.onmessage = e => {
      let m; try { m = JSON.parse(e.data); } catch (_) { return; }
      if (m.type === 'status') {
        S.connected = !!m.connected; S.receiving = !!m.receiving; S.simulated = !!m.simulated; S.wrong = !!m.wrong_program;
        // An update changed the display files: reload, but only once nobody is using the screen.
        if (m.version) { if (!S.version) S.version = m.version; else if (m.version !== S.version) S.reloadPending = true; }
      } else if (m.type === 'h') {
        onSample(m);
      }
    };
    es.onerror = () => { S.connected = false; S.receiving = false; };
  }

  function onSample(m) {
    const now = performance.now();
    S.msgs++;
    S.amp = m.a;
    // Stage from the Arduino: 0 = no finger, 1 = finger detected for 1 s (verifying), 2 = heart rate verified.
    // Older firmware doesn't send it, so work it out from the other fields.
    S.stage = (m.g !== undefined) ? m.g : (m.b > 0 ? 2 : (m.a >= PULSE_HINT_AMP ? 1 : 0));
    S.hasProgress = m.p !== undefined; S.progress = m.p || 0;
    if (m.x) S.beatAt = now;                       // a real beat (the Arduino passes these on from the 2nd steady beat)
    S.wave.push(m.s);
    if (S.wave.length > WAVE_POINTS) S.wave.shift();

    if (m.b > 0 || m.a >= PULSE_HINT_AMP) S.lastHeard = now;
    if (m.b > 0) {
      S.bpm = m.b; S.validRun++; S.lastValid = now;
      // A confirmed finger starts LIVE (and cuts the results short if a new student steps up).
      if (S.mode !== 'live' && S.validRun >= BPM_ON_AFTER) setMode('live', now);
    } else {
      S.validRun = 0;
    }
  }

  function setMode(mode, now) {
    S.mode = mode; S.modeAt = now;
    for (const id of ['standby', 'detect', 'live', 'summary']) $(id).classList.toggle('on', id === mode);
    if (mode === 'detect') { S.stageLowSince = 0; pipsOn = 0; for (const p of $('pips').children) p.className = ''; }
    if (mode === 'live') {
      S.shownBpm = S.bpm;                            // the first reading is ready the moment we go live
      S.session = { samples: [S.bpm], startAt: now, nextSampleAt: now + SESSION_SAMPLE_MS };
      S.lastHeard = now;
      const v = $('verified'); v.classList.remove('show'); void v.offsetWidth; v.classList.add('show');   // "Heart rate verified"
    }
  }

  // The finger has gone: work out the results and show them for a few seconds.
  function endSession(now) {
    const samples = S.session.samples;
    S.validRun = 0;
    if (!samples.length) { setMode('standby', now); return; }
    const sum = samples.reduce((a, v) => a + v, 0);
    const secs = Math.max(0, Math.round((S.beatAt - S.session.startAt) / 1000)) + CONFIRM_SECONDS;
    S.summary = { samples: samples.slice(), min: Math.min(...samples), max: Math.max(...samples),
                  avg: Math.round(sum / samples.length), secs };
    $('sum-avg').textContent = S.summary.avg;
    $('sum-min').textContent = S.summary.min;
    $('sum-max').textContent = S.summary.max;
    $('sum-time').textContent = secs < 60 ? `${secs} s` : `${Math.floor(secs / 60)}:${String(secs % 60).padStart(2, '0')}`;
    S.summaryUntil = now + SUMMARY_MS;
    setMode('summary', now);
  }

  // ---- canvas helpers ---------------------------------------------------------
  function fit(cv) {
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    const w = Math.max(1, Math.round(cv.clientWidth * dpr));
    const h = Math.max(1, Math.round(cv.clientHeight * dpr));
    if (cv.width !== w || cv.height !== h) { cv.width = w; cv.height = h; }
    return [cv.getContext('2d'), w, h];
  }
  function rr(ctx, x, y, w, h, r) {
    ctx.beginPath();
    if (ctx.roundRect) ctx.roundRect(x, y, w, h, r); else ctx.rect(x, y, w, h);
  }
  const g = (x, mu, sd) => { const d = (x - mu) / sd; return Math.exp(-d * d); };
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));

  // 9 x 8 pixel heart
  const HEART = [
    '.XX...XX.',
    'XXXX.XXXX',
    'XXXXXXXXX',
    'XXXXXXXXX',
    '.XXXXXXX.',
    '..XXXXX..',
    '...XXX...',
    '....X....',
  ];
  const HEART_CELLS = [];
  HEART.forEach((row, y) => [...row].forEach((c, x) => { if (c === 'X') HEART_CELLS.push([x, y]); }));

  // lub-dub pulse: 1 at each thump, decaying
  function lubdub(tb) {
    return Math.exp(-tb / 0.07) + (tb > 0.27 ? 0.6 * Math.exp(-(tb - 0.27) / 0.07) : 0);
  }

  // ---- STANDBY animation ---------------------------------------------------------
  const cvSb = $('cv-standby');
  const particles = [];
  let lastT = 0, lastCycle = -1;
  const PERIOD = 1.15;

  function ecgShape(u) {
    return 0.12 * g(u, .12, .035) - 0.10 * g(u, .265, .010) + 1.0 * g(u, .30, .012)
         - 0.22 * g(u, .335, .012) + 0.26 * g(u, .55, .05);
  }

  function drawStandby(t) {
    const [ctx, w, h] = fit(cvSb);
    ctx.clearRect(0, 0, w, h);
    const dt = Math.min(0.1, t - lastT); lastT = t;

    const rows = 14, cell = h / rows, gap = Math.max(2, cell * 0.08), rad = cell * 0.18;
    const cols = Math.ceil(w / cell) + 1, ox = (w - cols * cell) / 2;
    const hx0 = Math.round((cols - 9) / 2), hy0 = 1;
    const hcx = (hx0 + 4.5), hcy = (hy0 + 4);                // heart centre, in cell units

    const cyc = Math.floor(t / PERIOD), tb = t - cyc * PERIOD;
    const pulse = lubdub(tb);
    if (cyc !== lastCycle) {                                   // new heartbeat: burst of pixels
      lastCycle = cyc;
      for (let i = 0; i < 9; i++) {
        const a = Math.random() * Math.PI * 2, sp = (3 + Math.random() * 5) * cell;
        particles.push({ x: (hcx * cell) + ox, y: hcy * cell, vx: Math.cos(a) * sp, vy: Math.sin(a) * sp - cell,
          life: 1, decay: 0.5 + Math.random() * 0.5, size: cell * (0.2 + Math.random() * 0.25),
          c: HEART_COLOURS[(Math.random() * 4) | 0] });
      }
      if (particles.length > 90) particles.splice(0, particles.length - 90);
    }

    // -- background pixel grid: colour waves + ripples radiating from the heart
    ctx.lineWidth = 1;
    ctx.strokeStyle = 'rgba(255,255,255,0.05)';
    for (let cy = 0; cy < rows; cy++) {
      for (let cx = 0; cx < cols; cx++) {
        const base = (0.5 + 0.5 * Math.sin(cx * 0.55 + cy * 0.42 - t * 0.9)) *
                     (0.5 + 0.5 * Math.sin(cx * 0.21 - cy * 0.33 + t * 0.55));
        const dx = cx + 0.5 - hcx, dy = cy + 0.5 - hcy, d = Math.hypot(dx, dy);
        let ring = 0;
        for (let k = 0; k < 3; k++) {
          const age = tb + k * PERIOD, r = age * 8.5;
          ring += Math.max(0, 1 - age / 2.8) * g(d, r, 1.0);
        }
        const lit = clamp(base * 0.5 + ring, 0, 1);
        let fill, alpha;
        if (ring > 0.35)      { fill = ring > 0.7 ? LAV : TEAL; alpha = clamp(ring * 0.7, 0.2, 0.6); }
        else if (lit > 0.22)  { fill = base > 0.45 ? TEAL2 : PURPLE; alpha = clamp(lit * 0.55, 0.1, 0.36); }
        else                  { fill = PURPLE; alpha = 0.09; }
        if (cx >= hx0 - 1 && cx <= hx0 + 9 && cy >= hy0 - 1 && cy <= hy0 + 8) alpha *= 0.4;  // keep the heart crisp
        ctx.globalAlpha = alpha; ctx.fillStyle = fill;
        rr(ctx, ox + cx * cell + gap / 2, cy * cell + gap / 2, cell - gap, cell - gap, rad);
        ctx.fill();
        if (alpha > 0.15) { ctx.globalAlpha = alpha * 0.5; ctx.stroke(); }
      }
    }

    // -- the big pixel heart (same grid as the background), colours drifting
    const s = 1 + 0.15 * pulse;
    ctx.lineWidth = Math.max(1, cell * 0.04);
    ctx.strokeStyle = 'rgba(255,255,255,0.5)';
    for (const [x, y] of HEART_CELLS) {
      const ci = ((Math.floor(x * 0.6 + y * 0.9 + t * 2.2) % 4) + 4) % 4;
      const px = ox + (hcx + (x + 0.5 - 4.5) * s) * cell, py = (hcy + (y + 0.5 - 4) * s) * cell;
      const sz = cell * s - gap;
      ctx.globalAlpha = 1; ctx.fillStyle = HEART_COLOURS[ci];
      rr(ctx, px - sz / 2, py - sz / 2, sz, sz, rad); ctx.fill();
      ctx.globalAlpha = 0.55; ctx.stroke();
    }

    // -- pixel confetti
    ctx.lineWidth = 1;
    for (let i = particles.length - 1; i >= 0; i--) {
      const p = particles[i];
      p.life -= p.decay * dt;
      if (p.life <= 0) { particles.splice(i, 1); continue; }
      p.x += p.vx * dt; p.y += p.vy * dt; p.vx *= 0.985; p.vy = p.vy * 0.985 - cell * 0.6 * dt;
      ctx.globalAlpha = clamp(p.life, 0, 1) * 0.9; ctx.fillStyle = p.c;
      rr(ctx, p.x - p.size / 2, p.y - p.size / 2, p.size, p.size, p.size * 0.2); ctx.fill();
    }

    // -- ECG blip sweeping along the bottom
    const base = h - cell * 0.7, amp = cell * 1.1, period = w / 3.2, trail = w * 0.38;
    const head = (t * w * 0.26) % (w + trail);
    ctx.lineWidth = Math.max(2, cell * 0.09); ctx.lineJoin = 'round'; ctx.lineCap = 'round';
    ctx.strokeStyle = TEAL;
    const CH = 8;
    for (let c = 0; c < CH; c++) {
      const xa = head - trail * (1 - c / CH), xb = head - trail * (1 - (c + 1) / CH);
      ctx.globalAlpha = 0.08 + 0.85 * ((c + 1) / CH) ** 2;
      ctx.beginPath();
      let first = true;
      for (let x = Math.max(0, xa); x <= Math.min(w, xb); x += 3) {
        const y = base - amp * ecgShape(((x / period) % 1 + 1) % 1);
        if (first) { ctx.moveTo(x, y); first = false; } else ctx.lineTo(x, y);
      }
      ctx.stroke();
    }
    ctx.globalAlpha = 1;
  }

  // Standby wording: the usual prompt, or - for a few seconds after a finger was lifted before the heart rate was
  // verified - a reminder to hold on a little longer.
  let hintShown = false;
  function updateStandbyText(now) {
    const want = now < S.hintUntil;
    if (want === hintShown) return;
    hintShown = want;
    const el = $('sb-text'); el.classList.add('swap');
    setTimeout(() => {
      $('sb-title').textContent = hintShown ? 'Keep your finger on the sensor' : 'Check your heart rate';
      $('sb-sub').textContent = hintShown ? 'Hold until your heart rate appears' : 'Touch the sensor and hold';
      el.classList.remove('swap');
    }, 220);
  }

  // ---- DETECT screen: responds to every real beat -----------------------------------------
  const cvDetect = $('cv-detect');
  let pipsOn = 0;
  function drawDetect(now) {
    const age = (now - S.beatAt) / 1000;
    const pulse = age < 0.5 ? Math.exp(-age / 0.09) : 0;
    drawHeartOn(cvDetect, 1 + 0.2 * pulse, 0.58, (ctx, w, h) => {
      // a ripple spreads from the heart with each real beat
      if (age < 1.1) {
        const r = Math.min(w, h) * (0.3 + 0.19 * age / 1.1);        // stays inside the canvas
        ctx.beginPath(); ctx.arc(w / 2, h / 2, r, 0, 7);
        ctx.strokeStyle = `rgba(25,193,202,${0.55 * (1 - age / 1.1)})`; ctx.lineWidth = Math.max(2, h * 0.012); ctx.stroke();
        ctx.beginPath(); ctx.arc(w / 2, h / 2, r * 0.82, 0, 7);
        ctx.strokeStyle = `rgba(207,194,224,${0.35 * (1 - age / 1.1)})`; ctx.stroke();
      }
    });
    // one pixel lights for each steady beat counted so far
    const pips = $('pips');
    pips.classList.toggle('hidden', !S.hasProgress);
    const want = Math.min(S.progress, pips.children.length);
    if (want !== pipsOn) {
      for (let i = 0; i < pips.children.length; i++) {
        const p = pips.children[i];
        const lit = i < want;
        if (lit && !p.classList.contains('on')) { p.className = 'on'; void p.offsetWidth; p.classList.add('pop'); }
        else if (!lit) p.className = '';
      }
      pipsOn = want;
    }
  }

  // ---- LIVE screen ---------------------------------------------------------------
  const cvWave = $('cv-wave'), cvHeart = $('cv-heart');

  function drawWave(now) {
    const [ctx, w, h] = fit(cvWave);
    ctx.clearRect(0, 0, w, h);
    const n = S.wave.length;

    // faint square grid, echoing the pixel motif
    const cell = h / 6;
    ctx.strokeStyle = 'rgba(207,194,224,0.07)'; ctx.lineWidth = 1; ctx.beginPath();
    for (let x = (w % cell) / 2; x < w; x += cell) { ctx.moveTo(x, 0); ctx.lineTo(x, h); }
    for (let y = cell / 2; y < h; y += cell)       { ctx.moveTo(0, y); ctx.lineTo(w, y); }
    ctx.stroke();
    if (n < 2) return;

    // auto-scale to what the sensor is doing right now
    let lo = Infinity, hi = -Infinity;
    for (let i = 0; i < n; i++) { const v = S.wave[i]; if (v < lo) lo = v; if (v > hi) hi = v; }
    if (hi - lo < 40) { const mid = (hi + lo) / 2; lo = mid - 20; hi = mid + 20; }
    const pad = (hi - lo) * 0.18; lo -= pad; hi += pad;
    S.yMin += (lo - S.yMin) * 0.08; S.yMax += (hi - S.yMax) * 0.08;
    const padR = h * 0.07;
    const X = i => (w - padR) - (n - 1 - i) / (WAVE_POINTS - 1) * (w - padR);
    const Y = v => h - (v - S.yMin) / (S.yMax - S.yMin) * h;

    const grad = ctx.createLinearGradient(0, 0, w, 0);
    grad.addColorStop(0, 'rgba(25,193,202,0)');
    grad.addColorStop(0.45, 'rgba(25,193,202,0.8)');
    grad.addColorStop(1, 'rgba(25,193,202,1)');
    ctx.lineJoin = 'round'; ctx.lineCap = 'round';
    ctx.beginPath();
    for (let i = 0; i < n; i++) { const x = X(i), y = Y(S.wave[i]); i ? ctx.lineTo(x, y) : ctx.moveTo(x, y); }
    const glow = ctx.createLinearGradient(0, 0, w, 0);
    glow.addColorStop(0, 'rgba(25,193,202,0)'); glow.addColorStop(0.5, 'rgba(25,193,202,0.16)');
    glow.addColorStop(1, 'rgba(25,193,202,0.2)');
    ctx.strokeStyle = glow; ctx.lineWidth = h * 0.034; ctx.stroke();                          // cheap glow
    ctx.strokeStyle = grad; ctx.lineWidth = Math.max(2.5, h * 0.011); ctx.stroke();

    // glowing head + ripple on every beat
    const hx = X(n - 1), hy = Y(S.wave[n - 1]);
    const age = (now - S.beatAt) / 1000;
    if (age < 0.6) {
      ctx.beginPath(); ctx.arc(hx, hy, h * 0.03 + age * h * 0.28, 0, 7);
      ctx.strokeStyle = `rgba(207,194,224,${0.8 * (1 - age / 0.6)})`; ctx.lineWidth = 2; ctx.stroke();
    }
    ctx.beginPath(); ctx.arc(hx, hy, h * 0.028, 0, 7); ctx.fillStyle = LAV; ctx.fill();
  }

  const HEART_PX = 9, HEART_PY = 8;
  function drawHeartIcon(now) {
    const age = (now - S.beatAt) / 1000;
    const pulse = age < 0.5 ? Math.exp(-age / 0.09) : 0;
    drawHeartOn(cvHeart, 1 + 0.22 * pulse);
  }
  function drawHeartOn(cv, s, fill = 0.98, before = null) {
    const [ctx, w, h] = fit(cv);
    ctx.clearRect(0, 0, w, h);
    if (before) before(ctx, w, h);
    const cell = Math.min(w / HEART_PX, h / HEART_PY) * fill, gap = Math.max(1, cell * 0.09);
    ctx.lineWidth = 1; ctx.strokeStyle = 'rgba(255,255,255,0.5)';
    for (const [x, y] of HEART_CELLS) {
      const px = w / 2 + (x + 0.5 - 4.5) * cell * s, py = h / 2 + (y + 0.5 - 4) * cell * s;
      const sz = cell * s - gap;
      ctx.fillStyle = HEART_COLOURS[(x + y * 2) % 4];
      rr(ctx, px - sz / 2, py - sz / 2, sz, sz, cell * 0.16); ctx.fill(); ctx.stroke();
    }
  }

  let lastShown = -1;
  function updateLiveText() {
    // The Arduino already averages, cross-checks and only changes its reading every 3 s,
    // so the display simply shows what it is given (with a quick count-up between values).
    S.shownBpm += (S.bpm - S.shownBpm) * 0.25;
    const v = Math.round(S.shownBpm);
    if (v !== lastShown) { $('bpm-num').textContent = v; lastShown = v; }
  }

  // ---- SUMMARY screen ------------------------------------------------------------
  const cvSum = $('cv-sum'), cvSumHeart = $('cv-sum-heart');

  function drawSummary(now) {
    const sm = S.summary;
    if (!sm) return;
    drawHeartOn(cvSumHeart, 1);
    $('sum-timer-bar').style.width = (clamp(1 - (now - S.modeAt) / SUMMARY_MS, 0, 1) * 100).toFixed(1) + '%';

    const [ctx, w, h] = fit(cvSum);
    ctx.clearRect(0, 0, w, h);
    const cell = h / 6;
    ctx.strokeStyle = 'rgba(207,194,224,0.07)'; ctx.lineWidth = 1; ctx.beginPath();
    for (let x = (w % cell) / 2; x < w; x += cell) { ctx.moveTo(x, 0); ctx.lineTo(x, h); }
    for (let y = cell / 2; y < h; y += cell)       { ctx.moveTo(0, y); ctx.lineTo(w, y); }
    ctx.stroke();

    const n = sm.samples.length;
    const span = Math.max(sm.max - sm.min, 16), mid = (sm.max + sm.min) / 2;
    const y0 = mid - span * 0.8, y1 = mid + span * 0.8;
    const padL = w * 0.06, padR = w * 0.06, padT = h * 0.14, padB = h * 0.1;
    const X = i => n === 1 ? w / 2 : padL + i / (n - 1) * (w - padL - padR);
    const Y = v => h - padB - (v - y0) / (y1 - y0) * (h - padT - padB);
    const reveal = clamp((now - S.modeAt) / 900, 0, 1);          // the line draws itself in

    // average line
    ctx.setLineDash([h * 0.03, h * 0.03]); ctx.strokeStyle = 'rgba(207,194,224,0.45)'; ctx.lineWidth = 1.5;
    ctx.beginPath(); ctx.moveTo(padL * 0.5, Y(sm.avg)); ctx.lineTo(w - padR * 0.5, Y(sm.avg)); ctx.stroke();
    ctx.setLineDash([]);

    // readings
    ctx.lineJoin = 'round'; ctx.lineCap = 'round';
    if (n > 1) {
      const upto = reveal * (n - 1);
      ctx.beginPath(); ctx.moveTo(X(0), Y(sm.samples[0]));
      for (let i = 1; i <= Math.floor(upto); i++) ctx.lineTo(X(i), Y(sm.samples[i]));
      const f = upto - Math.floor(upto), j = Math.floor(upto);
      if (j < n - 1 && f > 0) ctx.lineTo(X(j) + (X(j + 1) - X(j)) * f, Y(sm.samples[j]) + (Y(sm.samples[j + 1]) - Y(sm.samples[j])) * f);
      ctx.strokeStyle = 'rgba(25,193,202,0.2)'; ctx.lineWidth = h * 0.034; ctx.stroke();
      ctx.strokeStyle = TEAL; ctx.lineWidth = Math.max(2.5, h * 0.011); ctx.stroke();
    }
    // pixel markers on each reading; lowest = purple, highest = lavender
    const sz = h * 0.05;
    sm.samples.forEach((v, i) => {
      if (n > 1 && i > reveal * (n - 1) + 0.001) return;
      const isMin = v === sm.min, isMax = v === sm.max;
      const big = (isMin || isMax) && sm.max !== sm.min;
      const s2 = big ? sz * 1.5 : sz;
      ctx.fillStyle = big ? (isMax ? LAV : PURPLE) : TEAL2;
      ctx.strokeStyle = 'rgba(255,255,255,0.6)'; ctx.lineWidth = 1;
      rr(ctx, X(i) - s2 / 2, Y(v) - s2 / 2, s2, s2, s2 * 0.2); ctx.fill(); ctx.stroke();
    });
  }

  // ---- status chip + debug -----------------------------------------------------------
  function updateChip() {
    const chip = $('chip');
    let msg = '';
    if (!S.connected)      msg = 'Sensor not connected — check the USB cable';
    else if (S.wrong)      msg = "The Arduino isn't running the heart rate program — flash it again";
    else if (!S.receiving) msg = 'Waiting for data from the sensor…';
    chip.textContent = msg;
    chip.classList.toggle('show', !!msg);
  }

  // ---- main loop ---------------------------------------------------------------------------
  let last = 0, frames = 0, fpsAt = 0, fps = 0;
  function frame(ts) {
    requestAnimationFrame(frame);
    if (ts - last < FRAME_MS - 2) return;
    last = ts;
    const now = performance.now();

    if (S.mode === 'live') {
      // Record the readings for the results screen (only while beats are really arriving).
      if (now >= S.session.nextSampleAt) {
        S.session.nextSampleAt = now + SESSION_SAMPLE_MS;
        if (now - S.beatAt < 1500) S.session.samples.push(S.bpm);
      }
      // Finger gone? (The Arduino has already waited 3 s for it to come back.)
      if (now - S.lastHeard > END_AFTER_MS || now - S.lastValid > HOLD_MAX_MS) endSession(now);
    } else if (S.mode === 'summary' && now >= S.summaryUntil) {
      setMode('standby', now); S.validRun = 0;
    }
    // A finger steady for 1 s (stage 1) opens the DETECT screen, even if results are showing; if the signal goes
    // before the heart rate is verified, go back to standby and remind them to hold on.
    if ((S.mode === 'standby' || S.mode === 'summary') && S.stage === 1 && S.bpm >= 0 && S.validRun === 0) { setMode('detect', now); }
    if (S.mode === 'detect') {
      if (S.stage >= 1) S.stageLowSince = 0;
      else if (!S.stageLowSince) S.stageLowSince = now;
      else if (now - S.stageLowSince > DETECT_LOST_MS) { S.hintUntil = now + HINT_MS; setMode('standby', now); S.validRun = 0; }
    }
    if (S.reloadPending && S.mode === 'standby' && now - S.modeAt > 2000) location.reload();

    const fading = now - S.modeAt < 800;
    if (S.mode === 'standby' || fading) { drawStandby(now / 1000); updateStandbyText(now); }
    if (S.mode === 'detect' || fading) drawDetect(now);
    if (S.mode === 'live' || fading) { drawWave(now); drawHeartIcon(now); updateLiveText(); }
    if (S.mode === 'summary' || fading) drawSummary(now);
    updateChip();

    frames++;
    if (now - fpsAt > 1000) {
      fps = frames; frames = 0; fpsAt = now;
      if (document.body.classList.contains('debug')) {
        $('debug').textContent =
          `mode ${S.mode}  fps ${fps}\nbpm ${S.bpm}  amp ${S.amp}  stage ${S.stage}  progress ${S.progress}\nwave ${S.wave.length}  msgs ${S.msgs}\n` +
          `conn ${S.connected}  rx ${S.receiving}${S.simulated ? '  (simulated)' : ''}`;
      }
    }
  }

  // The touchscreen is display-only: swallow menus, drags, zoom and scroll gestures.
  for (const ev of ['contextmenu', 'dragstart', 'selectstart', 'gesturestart', 'wheel'])
    document.addEventListener(ev, e => e.preventDefault(), { passive: false });
  for (const ev of ['touchstart', 'touchmove'])
    document.addEventListener(ev, e => e.preventDefault(), { passive: false });

  connect();
  requestAnimationFrame(frame);
})();
