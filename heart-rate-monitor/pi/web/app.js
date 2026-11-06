/* BNTS heart rate display.
 * Listens to /events (from server.py) and draws two screens:
 *   - STANDBY: animated pixel heart + rippling pixel grid, shown while no pulse is detected
 *   - LIVE:    big BPM number, scrolling pulse graph, pixel-bar history
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
  const HIST_SECONDS    = 60;    // BPM history strip
  const BPM_ON_AFTER    = 3;     // consecutive valid readings before switching to LIVE
  const BPM_OFF_AFTER_MS = 1500; // time without a valid BPM before going back to STANDBY
  const PULSE_HINT_AMP  = 25;    // amplitude at which standby says "hold still..."
  const WAVE_POINTS     = WAVE_SECONDS * SAMPLE_HZ;

  const MESSAGES = [
    ["What's your heart rate?",  'Pop a finger on the sensor to find out'],
    ['Calm… or caffeinated?', 'Rest a finger on the sensor and see'],
    ['Got a pulse?',             "Let's see it — finger on the sensor!"],
    ['Feel the beat',            'Press gently and hold still'],
    ['Show us your heart',       'Place a finger on the sensor'],
  ];

  const $ = id => document.getElementById(id);
  const params = new URLSearchParams(location.search);
  if (params.has('debug')) document.body.classList.add('debug');

  // ---- state ---------------------------------------------------------------
  const S = {
    mode: 'standby', modeAt: 0,
    wave: [], hist: [], lastHistAt: 0,
    bpm: 0, shownBpm: 0, validRun: 0, lastValid: 0,
    beatAt: -99999, amp: 0, readingUntil: 0,
    yMin: 480, yMax: 540,
    connected: false, receiving: false, simulated: false,
    msgs: 0,
  };

  // ---- data stream ---------------------------------------------------------
  function connect() {
    const es = new EventSource('/events');
    es.onmessage = e => {
      let m; try { m = JSON.parse(e.data); } catch (_) { return; }
      if (m.type === 'status') {
        S.connected = !!m.connected; S.receiving = !!m.receiving; S.simulated = !!m.simulated;
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
    S.wave.push(m.s);
    if (S.wave.length > WAVE_POINTS) S.wave.shift();

    if (m.b > 0) {
      S.bpm = m.b; S.validRun++; S.lastValid = now;
      if (m.x) S.beatAt = now;
      if (S.mode === 'standby' && S.validRun >= BPM_ON_AFTER) setMode('live', now);
      if (S.mode === 'live' && now - S.lastHistAt >= 1000) {
        S.hist.push(m.b); if (S.hist.length > HIST_SECONDS) S.hist.shift();
        S.lastHistAt = now;
      }
    } else {
      S.validRun = 0;
      if (m.a >= PULSE_HINT_AMP) S.readingUntil = now + 800;
    }
  }

  function setMode(mode, now) {
    S.mode = mode; S.modeAt = now;
    $('live').classList.toggle('on', mode === 'live');
    $('standby').classList.toggle('on', mode === 'standby');
    if (mode === 'live') { S.shownBpm = S.bpm; S.hist = [S.bpm]; S.lastHistAt = now; }
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

  // rotate the headline every few seconds
  let msgIdx = 0;
  setInterval(() => {
    const el = $('sb-text');
    if (S.mode !== 'standby' || performance.now() < S.readingUntil) return;
    el.classList.add('swap');
    setTimeout(() => {
      msgIdx = (msgIdx + 1) % MESSAGES.length;
      $('sb-title').textContent = MESSAGES[msgIdx][0];
      $('sb-sub').textContent = MESSAGES[msgIdx][1];
      el.classList.remove('swap');
    }, 500);
  }, 5200);

  let readingShown = false;
  function updateStandbyText(now) {
    const reading = now < S.readingUntil;
    if (reading === readingShown) return;
    readingShown = reading;
    const el = $('sb-text');
    if (reading) {
      $('sb-title').textContent = 'Hold still…';
      $('sb-sub').textContent = 'Listening for your pulse';
    } else {
      $('sb-title').textContent = MESSAGES[msgIdx][0];
      $('sb-sub').textContent = MESSAGES[msgIdx][1];
    }
    el.classList.remove('swap');
  }

  // ---- LIVE screen ---------------------------------------------------------------
  const cvWave = $('cv-wave'), cvHist = $('cv-hist'), cvHeart = $('cv-heart');

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
    ctx.strokeStyle = glow; ctx.lineWidth = h * 0.05; ctx.stroke();                          // cheap glow
    ctx.strokeStyle = grad; ctx.lineWidth = Math.max(2.5, h * 0.016); ctx.stroke();

    // glowing head + ripple on every beat
    const hx = X(n - 1), hy = Y(S.wave[n - 1]);
    const age = (now - S.beatAt) / 1000;
    if (age < 0.6) {
      ctx.beginPath(); ctx.arc(hx, hy, h * 0.03 + age * h * 0.28, 0, 7);
      ctx.strokeStyle = `rgba(207,194,224,${0.8 * (1 - age / 0.6)})`; ctx.lineWidth = 2; ctx.stroke();
    }
    ctx.beginPath(); ctx.arc(hx, hy, h * 0.028, 0, 7); ctx.fillStyle = LAV; ctx.fill();
  }

  function drawHist() {
    const [ctx, w, h] = fit(cvHist);
    ctx.clearRect(0, 0, w, h);
    const top = h * 0.22, rows = 5, cell = (h - top - h * 0.06) / rows, gap = Math.max(2, cell * 0.12);
    const cols = Math.floor((w - gap) / cell);
    const data = S.hist.slice(-cols);
    const x0 = w - gap / 2 - data.length * cell + 0;           // newest on the right
    ctx.lineWidth = 1; ctx.strokeStyle = 'rgba(255,255,255,0.35)';
    data.forEach((bpm, i) => {
      const filled = 1 + Math.round(clamp((bpm - 45) / 80, 0, 1) * (rows - 1));
      const newest = i === data.length - 1;
      for (let r = 0; r < filled; r++) {
        const y = h - h * 0.03 - (r + 1) * cell;
        ctx.globalAlpha = newest ? 1 : 0.9;
        ctx.fillStyle = newest ? LAV : (r < 2 ? PURPLE : (r < 4 ? TEAL2 : TEAL));
        rr(ctx, x0 + i * cell + gap / 2, y + gap / 2, cell - gap, cell - gap, cell * 0.16); ctx.fill();
        ctx.globalAlpha = 0.5; ctx.stroke();
      }
    });
    ctx.globalAlpha = 1;
  }

  const HEART_PX = 9, HEART_PY = 8;
  function drawHeartIcon(now) {
    const [ctx, w, h] = fit(cvHeart);
    ctx.clearRect(0, 0, w, h);
    const age = (now - S.beatAt) / 1000;
    const pulse = age < 0.5 ? Math.exp(-age / 0.09) : 0;
    const s = 1 + 0.22 * pulse;
    const cell = Math.min(w / HEART_PX, h / HEART_PY) * 0.98, gap = Math.max(1, cell * 0.09);
    ctx.lineWidth = 1; ctx.strokeStyle = 'rgba(255,255,255,0.5)';
    for (const [x, y] of HEART_CELLS) {
      const px = w / 2 + (x + 0.5 - 4.5) * cell * s, py = h / 2 + (y + 0.5 - 4) * cell * s;
      const sz = cell * s - gap;
      ctx.fillStyle = HEART_COLOURS[(x + y * 2) % 4];
      rr(ctx, px - sz / 2, py - sz / 2, sz, sz, cell * 0.16); ctx.fill(); ctx.stroke();
    }
  }

  function moodFor(b) {
    if (b < 55)  return 'Zen master';
    if (b < 70)  return 'Super chill';
    if (b < 85)  return 'Nice and steady';
    if (b < 105) return 'Warming up';
    if (b < 130) return 'Pumped!';
    return 'Racing!';
  }

  let lastShown = -1, lastMood = '';
  function updateLiveText() {
    S.shownBpm += (S.bpm - S.shownBpm) * 0.25;
    const v = Math.round(S.shownBpm);
    if (v !== lastShown) { $('bpm-num').textContent = v; lastShown = v; }
    const mood = moodFor(S.bpm);
    if (mood !== lastMood) { $('bpm-tag').textContent = mood; lastMood = mood; }
  }

  // ---- status chip + debug -----------------------------------------------------------
  function updateChip() {
    const chip = $('chip');
    let msg = '';
    if (!S.connected)      msg = 'Sensor not connected — check the USB cable';
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

    if (S.mode === 'live' && now - S.lastValid > BPM_OFF_AFTER_MS) { setMode('standby', now); S.validRun = 0; }

    const fading = now - S.modeAt < 800;
    if (S.mode === 'standby' || fading) { drawStandby(now / 1000); updateStandbyText(now); }
    if (S.mode === 'live' || fading) { drawWave(now); drawHist(); drawHeartIcon(now); updateLiveText(); }
    updateChip();

    frames++;
    if (now - fpsAt > 1000) {
      fps = frames; frames = 0; fpsAt = now;
      if (document.body.classList.contains('debug')) {
        $('debug').textContent =
          `mode ${S.mode}  fps ${fps}\nbpm ${S.bpm}  amp ${S.amp}\nwave ${S.wave.length}  msgs ${S.msgs}\n` +
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
