/* BNTS heart rate display.
 *
 * The process (nothing is filtered; every beat the Arduino reports is used):
 *   1. IDLE     "Press screen to start" (animated pixel heart).
 *   2. MEASURE  After a press the sensor is live. Waits for the first beat, then records every beat for 10 seconds
 *               (heart beats with each real beat, pulse graph, countdown).
 *   3. RESULT   The average heart rate, shown for 10 seconds.
 *   4. back to IDLE.
 * If no pulse was found the screen says so for a few seconds, then returns to IDLE.
 *
 * Add ?debug to the URL for an on-screen readout. For testing, the timings can be shortened with
 * ?measure=4&result=3&wait=6&failed=2 (seconds).
 */
(() => {
  'use strict';

  // ---- brand palette -------------------------------------------------------
  const TEAL = '#19c1ca', TEAL2 = '#04bbc1', PURPLE = '#704988', LAV = '#cfc2e0';
  const HEART_COLOURS = [TEAL, PURPLE, LAV, TEAL2];

  const $ = id => document.getElementById(id);
  const params = new URLSearchParams(location.search);
  if (params.has('debug')) document.body.classList.add('debug');
  const secs = (key, def) => (params.has(key) ? Math.max(0.5, +params.get(key) || def) : def) * 1000;

  // ---- settings you might want to tweak -----------------------------------------
  const MEASURE_MS   = secs('measure', 10);  // how long every beat is recorded once the first beat arrives
  const RESULT_MS    = secs('result', 10);   // how long the average is shown
  const WAIT_MS      = secs('wait', 20);     // how long to wait for a finger after the screen is pressed
  const FAILED_MS    = secs('failed', 5);    // how long "No pulse found" stays up
  const NO_BEAT_HINT_MS = 3000;              // no beat for this long while measuring -> "keep your finger on the sensor"
  const NO_DATA_MS   = 3000;                 // nothing at all from the Arduino for this long -> give up
  const FRAME_MS     = 33;                   // ~30 fps keeps the Pi 3 cool
  const WAVE_SECONDS = 6;                    // width of the scrolling graph
  const SAMPLE_HZ    = 50;                   // must match the Arduino
  const WAVE_POINTS  = WAVE_SECONDS * SAMPLE_HZ;

  // ---- state ---------------------------------------------------------------
  const S = {
    mode: 'idle', modeAt: 0,                 // idle | waiting | measure | result | failed
    wave: [], yMin: 480, yMax: 540,
    lastBm: null,                            // the Arduino's beat clock at the last beat we saw
    beats: [], t0: 0, dataT: 0, lastDataAt: 0, beatAt: -99999,
    res: null,
    connected: false, receiving: false, simulated: false, wrong: false, oldFw: false,
    version: '', reloadPending: false, msgs: 0,
  };
  const SCREEN = { idle: 'standby', failed: 'standby', waiting: 'measure', measure: 'measure', result: 'result' };

  // ---- data stream ---------------------------------------------------------
  function connect() {
    const es = new EventSource('/events');
    es.onmessage = e => {
      let m; try { m = JSON.parse(e.data); } catch (_) { return; }
      if (m.type === 'status') {
        S.connected = !!m.connected; S.receiving = !!m.receiving; S.simulated = !!m.simulated;
        S.wrong = !!m.wrong_program; S.oldFw = !!m.old_firmware;
        // An update changed the display files: reload, but only while nobody is using the screen.
        if (m.version) { if (!S.version) S.version = m.version; else if (m.version !== S.version) S.reloadPending = true; }
      } else if (m.type === 'h') {
        onSample(m);
      }
    };
    es.onerror = () => { S.connected = false; S.receiving = false; };
  }

  // One line from the Arduino: the pulse graph, and (when the beat clock changes) a new beat.
  function onSample(m) {
    const now = performance.now();
    S.msgs++; S.dataT = m.t; S.lastDataAt = now;
    S.wave.push(m.s);
    if (S.wave.length > WAVE_POINTS) S.wave.shift();

    if (m.bm !== undefined) {
      if (S.lastBm === null) S.lastBm = m.bm;                   // first line we see: remember it, don't count it
      else if (m.bm !== S.lastBm) {
        const backwards = ((m.bm - S.lastBm) >>> 0) > 0x80000000;  // the clock went backwards: the Arduino restarted
        S.lastBm = m.bm;
        if (!backwards) onBeat(m.bm, m.t, now);
      }
    }
    if (S.mode === 'measure' && m.t - S.t0 >= MEASURE_MS) finishMeasure(now);
  }

  function onBeat(bm, t, now) {
    S.beatAt = now;
    if (S.mode === 'waiting') {                                  // the first beat starts the clock
      S.beats = [bm]; S.t0 = t;
      setMode('measure', now);
    } else if (S.mode === 'measure') {
      S.beats.push(bm);
    }
  }

  // ---- the process ---------------------------------------------------------
  function press() {
    const now = performance.now();
    if (S.mode === 'idle' || S.mode === 'failed') {
      S.beats = []; S.t0 = 0;
      setMode('waiting', now);                                   // the sensor is live from now
    }
  }

  function finishMeasure(now) {
    const r = BNTSMeasure.summarize(S.beats);
    if (!r) return fail(now);
    S.res = r;
    const spanS = Math.round(r.spanMs / 1000);
    $('bpm-num').textContent = Math.round(r.avg);
    $('basis').textContent = `Average of ${r.beats} beats over ${spanS} seconds`;
    setMode('result', now);
  }

  function fail(now) { setMode('failed', now); }

  function setMode(mode, now) {
    S.mode = mode; S.modeAt = now;
    const screen = SCREEN[mode];
    for (const id of ['standby', 'measure', 'result']) $(id).classList.toggle('on', id === screen);
    $('measure').classList.toggle('waiting', mode === 'waiting');
    if (mode === 'idle' || mode === 'failed') setStandbyText(mode);
  }

  // Standby wording, with a quick fade between the two messages.
  let shownText = 'idle';
  function setStandbyText(mode) {
    if (mode === shownText) return;
    shownText = mode;
    const el = $('sb-text'); el.classList.add('swap');
    setTimeout(() => {
      $('sb-title').textContent = shownText === 'failed' ? 'No pulse found' : 'Press screen to start';
      $('sb-sub').textContent = shownText === 'failed' ? 'Rest a finger on the sensor and try again' : 'to check your heart rate';
      el.classList.toggle('press', shownText === 'idle');
      el.classList.remove('swap');
    }, 220);
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
  const HEART_PX = 9, HEART_PY = 8;

  // lub-dub pulse: 1 at each thump, decaying
  function lubdub(tb) {
    return Math.exp(-tb / 0.07) + (tb > 0.27 ? 0.6 * Math.exp(-(tb - 0.27) / 0.07) : 0);
  }

  // ---- IDLE animation ---------------------------------------------------------
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

  // ---- heart + pulse graph (measure and result screens) -----------------------------------
  const cvHeart = $('cv-heart'), cvMHeart = $('cv-m-heart'), cvWave = $('cv-wave'), cvWaveM = $('cv-wave-m');

  function drawHeartOn(cv, s) {
    const [ctx, w, h] = fit(cv);
    ctx.clearRect(0, 0, w, h);
    const cell = Math.min(w / HEART_PX, h / HEART_PY) * 0.98, gap = Math.max(1, cell * 0.09);
    ctx.lineWidth = 1; ctx.strokeStyle = 'rgba(255,255,255,0.5)';
    for (const [x, y] of HEART_CELLS) {
      const px = w / 2 + (x + 0.5 - 4.5) * cell * s, py = h / 2 + (y + 0.5 - 4) * cell * s;
      const sz = cell * s - gap;
      ctx.fillStyle = HEART_COLOURS[(x + y * 2) % 4];
      rr(ctx, px - sz / 2, py - sz / 2, sz, sz, cell * 0.16); ctx.fill(); ctx.stroke();
    }
  }
  const beatPulse = age => (age < 0.5 ? 1 + 0.22 * Math.exp(-age / 0.09) : 1);

  function drawWave(cv, now) {
    const [ctx, w, h] = fit(cv);
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

  const bar = (id, frac) => { $(id).style.width = (clamp(frac, 0, 1) * 100).toFixed(1) + '%'; };
  const setText = (id, txt) => { const el = $(id); if (el.textContent !== txt) el.textContent = txt; };

  // ---- MEASURE screen ---------------------------------------------------------------
  function drawMeasure(now) {
    drawHeartOn(cvMHeart, beatPulse((now - S.beatAt) / 1000));
    drawWave(cvWaveM, now);
    if (S.mode === 'waiting') { bar('m-bar', 0); return; }
    const elapsed = clamp(S.dataT - S.t0, 0, MEASURE_MS);
    setText('m-count', String(Math.max(0, Math.ceil((MEASURE_MS - elapsed) / 1000))));
    const nb = S.beats.length;
    setText('m-beats', now - S.beatAt > NO_BEAT_HINT_MS ? 'Keep your finger on the sensor' : `${nb} ${nb === 1 ? 'beat' : 'beats'}`);
    bar('m-bar', elapsed / MEASURE_MS);
  }

  // ---- RESULT screen ---------------------------------------------------------------
  function drawResult(now) {
    const period = 60000 / S.res.avg;                       // the heart beats at the measured rate
    drawHeartOn(cvHeart, beatPulse((((now - S.modeAt) % period) / 1000)));
    drawWave(cvWave, now);
    bar('r-bar', 1 - (now - S.modeAt) / RESULT_MS);
  }

  // ---- status chip + debug -----------------------------------------------------------
  function updateChip() {
    const chip = $('chip');
    let msg = '';
    if (!S.connected)      msg = 'Sensor not connected — check the USB cable';
    else if (S.oldFw)      msg = 'The Arduino has old firmware — flash it again';
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
    const age = now - S.modeAt;

    // timers
    if (S.mode === 'waiting' && age > WAIT_MS) fail(now);                       // nobody put a finger on
    else if ((S.mode === 'waiting' || S.mode === 'measure') && now - Math.max(S.lastDataAt, S.modeAt) > NO_DATA_MS) fail(now);   // Arduino went quiet
    else if (S.mode === 'result' && age >= RESULT_MS) setMode('idle', now);
    else if (S.mode === 'failed' && age >= FAILED_MS) setMode('idle', now);
    if (S.reloadPending && S.mode === 'idle' && age > 2000) location.reload();   // pick up an update while idle

    const fading = now - S.modeAt < 800;
    if (SCREEN[S.mode] === 'standby' || fading) drawStandby(now / 1000);
    if (SCREEN[S.mode] === 'measure' || (fading && S.mode !== 'idle')) drawMeasure(now);
    if (S.res && (S.mode === 'result' || (fading && S.mode === 'idle'))) drawResult(now);
    updateChip();

    frames++;
    if (now - fpsAt > 1000) {
      fps = frames; frames = 0; fpsAt = now;
      if (document.body.classList.contains('debug')) {
        $('debug').textContent =
          `mode ${S.mode}  fps ${fps}  beats ${S.beats.length}\nwave ${S.wave.length}  msgs ${S.msgs}  bm ${S.lastBm}\n` +
          `conn ${S.connected}  rx ${S.receiving}${S.simulated ? '  (simulated)' : ''}`;
      }
    }
  }

  // Press the screen (or Space / Enter) to start. Everything else about the touchscreen is switched off: no menus,
  // drags, zoom or scrolling.
  document.addEventListener('pointerdown', press);
  document.addEventListener('keydown', e => { if (e.code === 'Space' || e.code === 'Enter') press(); });
  for (const ev of ['contextmenu', 'dragstart', 'selectstart', 'gesturestart', 'wheel'])
    document.addEventListener(ev, e => e.preventDefault(), { passive: false });
  for (const ev of ['touchstart', 'touchmove'])
    document.addEventListener(ev, e => e.preventDefault(), { passive: false });

  connect();
  requestAnimationFrame(frame);
})();
