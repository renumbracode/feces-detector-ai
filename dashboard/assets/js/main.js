(function () {
  'use strict';

  const esc = (s) => String(s == null ? '' : s).replace(/[&<>"']/g,
    (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

  const qs = (sel) => document.querySelector(sel);

  function toast(msg, type) {
    const wrap = qs('#toasts');
    if (!wrap) return;
    const el = document.createElement('div');
    el.className = 'toast ' + (type || '');
    el.textContent = msg;
    wrap.appendChild(el);
    setTimeout(() => {
      el.style.opacity = '0';
      el.style.transform = 'translateY(-8px)';
      setTimeout(() => el.remove(), 260);
    }, 4200);
  }

  function initClock() {
    const el = qs('#clock');
    if (!el) return;
    const tick = () => {
      el.textContent = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
    };
    tick();
    setInterval(tick, 1000);
  }

  function initSidebar() {
    const toggle = qs('#sidebar-toggle');
    if (!toggle) return;
    const close = () => document.body.classList.remove('nav-open');
    toggle.addEventListener('click', () => document.body.classList.toggle('nav-open'));
    const backdrop = qs('#sidebar-backdrop');
    if (backdrop) backdrop.addEventListener('click', close);
    document.querySelectorAll('.nav-link').forEach((l) => {
      l.addEventListener('click', () => { if (window.innerWidth <= 900) close(); });
    });
  }

  function initFlash() {
    document.querySelectorAll('.flash').forEach((el) => {
      const btn = el.querySelector('.dismiss');
      if (btn) btn.addEventListener('click', () => el.remove());
      setTimeout(() => {
        el.style.opacity = '0';
        setTimeout(() => el.remove(), 320);
      }, 6000);
    });
  }

  function initConfirmForms() {
    document.querySelectorAll('form[data-confirm]').forEach((form) => {
      form.addEventListener('submit', (e) => {
        const msg = form.getAttribute('data-confirm');
        if (msg && !window.confirm(msg)) {
          e.preventDefault();
          return;
        }
        const btn = form.querySelector('button[type=submit]');
        if (btn) {
          btn.disabled = true;
          btn.textContent = 'Sending...';
        }
      });
    });
  }

  function fmtBytes(b) {
    if (b == null) return '';
    if (b >= 1048576) return (b / 1048576).toFixed(1) + ' MB';
    if (b >= 1024) return (b / 1024).toFixed(0) + ' KB';
    return b + ' B';
  }

  function fmtUptime(s) {
    s = Math.floor(s || 0);
    const d = Math.floor(s / 86400);
    const h = Math.floor((s % 86400) / 3600);
    const m = Math.floor((s % 3600) / 60);
    if (d) return d + 'd ' + h + 'h';
    if (h) return h + 'h ' + m + 'm';
    return m + 'm';
  }

  function statusChips(j) {
    const parts = [];
    if (j.ip) parts.push('<span class="chip">IP ' + esc(j.ip) + '</span>');
    parts.push('<span class="badge ' + (j.sprayActive ? 'badge-amber' : 'badge-no') + '">Spray ' +
      (j.sprayActive ? 'ACTIVE' : 'off') + '</span>');
    /* Label the two detectors separately. "Conf" alone is ambiguous now that both
     * can trigger a spray: it is the on-device FOMO score only. */
    if (typeof j.lastConfidence === 'number') {
      parts.push('<span class="chip">FOMO ' + j.lastConfidence.toFixed(2) + '</span>');
    }
    if (j.model) {
      parts.push('<span class="chip">' + esc(j.model) + '</span>');
    }
    /* The server-side YOLOv8 verdict. This can now trigger a spray in its own
     * right, so it is labelled as a trigger source, not just an overlay. */
    if (j.vrf && j.vrf.valid) {
      parts.push('<span class="chip ' + (j.vrf.detected ? 'chip-verified' : 'chip-off') + '">' +
        'YOLO ' + Number(j.vrf.conf || 0).toFixed(2) + (j.vrf.detected ? ' hit' : ' clear') +
        '</span>');
    }
    /* Which detector is currently responsible for a detection, matching what the
     * firmware actually ORs. Without this the panel cannot tell a device hit
     * from a server hit. Multi-instance: the count is how many feces spots are
     * in the frame, not just whether one is. */
    {
      const sig = fecesSignal(j);
      if (sig.detected) {
        const label = sig.source === 'both' ? 'FOMO+YOLO'
                    : (sig.source === 'server' ? 'via YOLOv8 server' : 'via FOMO device');
        const count = sig.instanceCount > 1 ? ' ×' + sig.instanceCount : '';
        parts.push('<span class="chip chip-verified">Detected' + count + ' ' + esc(label) + '</span>');
      }
    }
    if (typeof j.fps === 'number' && j.fps > 0) {
      parts.push('<span class="chip">' + j.fps + ' FPS</span>');
    }
    if (typeof j.heap === 'number' && j.heap > 0) {
      parts.push('<span class="chip">Heap ' + fmtBytes(j.heap) + '</span>');
    }
    if (typeof j.uptimeS === 'number') {
      parts.push('<span class="chip">Up ' + fmtUptime(j.uptimeS) + '</span>');
    }
    return parts.join('');
  }

  /* ----- shared detection signal -----
   * The overlay on the live page and the alert sound elsewhere both need to
   * answer one question -- "does this frame contain feces?" -- from the same
   * rules. Keeping the rules here is what stops the picture and the sound from
   * disagreeing about the same frame.
   *
   * A verifier answer is only trusted while it is fresh. The device polls the
   * verifier and reports how old the answer is in vrf.ageMs, so an answer
   * older than this means the verifier stopped answering (crashed, PC asleep,
   * network down) rather than the pen actually being clean. Without this the
   * last "no feces" would stay on screen indefinitely.
   *
   * 15000 is not arbitrary: it is VERIFY_STALL_MS in the firmware
   * (components/detection_service/detection_service.c), the age at which the
   * DEVICE itself declares the poll leg stalled and forces an attempt. The
   * dashboard used to be stricter than the device, at 6000, and the board's
   * real poll cycle is ~3.4 s -- so a single hiccuped poll (5 s client
   * timeout plus a 4 s backoff) pushed ageMs past 6 s and the overlay cried
   * "VERIFY OFFLINE" while the server was answering in 0.4 s. Matching the
   * firmware's own threshold keeps the two in agreement, so the panel only
   * says "offline" when the device agrees that the leg is stalled. */
  const VRF_STALE_MS = 15000;

  function fecesSignal(j) {
    j = j || {};
    const v = j.vrf;
    const vrfStale = !!(v && v.valid && Number(v.ageMs) > VRF_STALE_MS);
    const vrfFresh = !!(v && v.valid && !vrfStale);

    /* Two detectors can spray, and the firmware ORs them: the on-device FOMO
     * class, and the server-side YOLOv8 at server_trigger_conf. The picture and
     * the sound must therefore follow the SAME union the relay follows --
     * otherwise the UI contradicts the hardware (it showed "no detection" while
     * the server was spraying).
     *
     * Each source is still reported separately, plus which one decided, because
     * "FOMO hit 0.41" and "YOLOv8 hit 0.28" are materially different claims and
     * the panel should not blur them into one green box.
     *
     * The verifier's answer is only trusted while fresh (see VRF_STALE_MS);
     * a stale answer must not keep claiming a detection. */
    const deviceDetected = !!(j.objectPresent && typeof j.classId === 'number' &&
                             j.classId === 0);
    const deviceConf = Number(j.lastConfidence || 0);
    const serverDetected = vrfFresh ? !!v.detected : false;
    const serverConf = vrfFresh ? Number(v.conf || 0) : 0;

    const detected = deviceDetected || serverDetected;
    let source = detected ? (deviceDetected && serverDetected ? 'both'
                            : (deviceDetected ? 'fomo' : 'server'))
                          : 'none';

    /* Multi-instance: prefer the box arrays the new firmware/server send
     * (j.boxes for FOMO, v.boxes for YOLOv8, each score-descending). Older
     * single-box payloads fall back to the legacy j.box / v.x..h fields.
     * An array that EXISTS but is empty means "detected, no location" -- the
     * new firmware used to fabricate a centred 0.3/0.3/0.4/0.4 square in its
     * legacy fields, and trusting it here rendered a green box stuck dead
     * centre. Only a missing key (old firmware) may use the legacy fallback. */
    const asBoxList = (arr, legacy) => {
      if (Array.isArray(arr)) {
        return arr.filter((b) => b && Number(b.w) > 0.001 && Number(b.h) > 0.001);
      }
      return legacy ? [legacy] : [];
    };
    const deviceBoxes = asBoxList(j.boxes, j.box);
    const serverBoxes = vrfFresh ? asBoxList(v.boxes,
        (Number(v.w) > 0.001 || Number(v.h) > 0.001) ? v : null) : [];

    /* Union of both detectors' boxes, so every distinct spot gets its own
     * overlay wherever either model saw it. The old device-preferred pick let
     * YOLOv8-only spots go unboxed (FOMO sits below its threshold while the
     * server fires). Boxes overlapping at IoU >= 0.15 are the same physical
     * object seen twice -- keep the higher score, drop the dupe. The threshold
     * used to be 0.30, which also ate genuinely distinct spots a few percent
     * apart; the server's NMS now collapses same-blob duplicates upstream, so
     * the dashboard can afford to be permissive and let close neighbours each
     * keep their own box. Entries carry
     * their own conf when the wire provides one; otherwise they inherit the
     * source's headline confidence. */
    const iou = (a, b) => {
      const ix = Math.max(0, Math.min(a.x + a.w, b.x + b.w) - Math.max(a.x, b.x));
      const iy = Math.max(0, Math.min(a.y + a.h, b.y + b.h) - Math.max(a.y, b.y));
      const inter = ix * iy;
      const uni = a.w * a.h + b.w * b.h - inter;
      return uni > 0 ? inter / uni : 0;
    };
    const tag = (arr, conf) => arr.map((b) => ({
      x: b.x, y: b.y, w: b.w, h: b.h, s: Number(b.conf) || conf || 0
    })).sort((a, b) => b.s - a.s);
    const merged = tag(deviceBoxes, deviceConf);
    tag(serverBoxes, serverConf).forEach((b) => {
      if (!merged.some((m) => iou(m, b) >= 0.15)) merged.push(b);
    });
    merged.sort((a, b) => b.s - a.s);
    const boxes = merged.slice(0, 8);
    const boxData = boxes.length ? boxes[0] : null;

    /* How many feces spots are in the frame right now: the deduped union when
     * geometry exists, else the reported counts (max of the two detectors, so
     * one optimistic source is enough to raise the count). */
    const deviceCount = deviceDetected ? (Number(j.fecesCount) || deviceBoxes.length || 1) : 0;
    const serverCount = serverDetected ? (Number(v.fecesCount) || serverBoxes.length || 1) : 0;
    const instanceCount = boxes.length ? boxes.length : Math.max(deviceCount, serverCount);

    return {
      detected: detected,
      conf: deviceDetected ? deviceConf : serverConf,
      present: !!(j.objectPresent && typeof j.classId === 'number' && j.classId >= 0) ||
               serverDetected,
      isPig: !!(j.objectPresent && j.classId === 1) && !serverDetected,
      source: source,
      deviceDetected: deviceDetected,
      deviceConf: deviceDetected ? deviceConf : null,
      serverDetected: serverDetected,
      serverConf: serverDetected ? serverConf : null,
      /* Kept for callers that branch on which detector answered. `useVrf` now
       * means "the verifier decided this detection on its own". */
      useVrf: serverDetected && !deviceDetected,
      vrfStale: vrfStale,
      boxData: boxData,
      boxes: boxes,
      instanceCount: instanceCount,
    };
  }

  /* ----- alert sounds -----
   * WebAudio only, no asset files: two short oscillators per sound is enough
   * for a pen alarm and keeps the dashboard a single-script page.
   *
   * Sound is enabled by default and the button mutes it. Browsers still refuse
   * to start audio until a user gesture, so createAlerter() installs a one-shot
   * unlock on the first interaction; until that fires, alarms are not scheduled
   * at all (see canPlay) rather than being silently discarded on a suspended
   * context. */
  const RETRIGGER_GAP_MS = 8000;

  function createAlerter(opts) {
    opts = opts || {};
    const btn = opts.button ? qs(opts.button) : null;

    let actx = null;
    const ctx = () => {
      if (!actx) actx = new (window.AudioContext || window.webkitAudioContext)();
      if (actx.state === 'suspended') actx.resume();
      return actx;
    };
    const tone = (freq, dur, type, gain, when) => {
      const a = ctx();
      const o = a.createOscillator();
      const g = a.createGain();
      o.type = type; o.frequency.value = freq;
      g.gain.setValueAtTime(0.0001, when);
      g.gain.exponentialRampToValueAtTime(gain, when + 0.02);
      g.gain.exponentialRampToValueAtTime(0.0001, when + dur);
      o.connect(g); g.connect(a.destination);
      o.start(when); o.stop(when + dur + 0.03);
    };
    /* Feces alarm: the old chime was two overlapping tones lasting ~0.6 s, which
     * is easy to miss from across a pig barn or over a pump relay clatter. Four
     * repeats of that same two-tone pair over ~5 s reads as an alarm instead of
     * a blip, and keeps the original timbre so it stays recognisable.
     *
     * Scheduling is the point. Every burst is handed to the AudioContext clock
     * up front, so the pattern is sample-accurate with no timers, and nothing is
     * left running afterwards: each oscillator self-stops at when + dur + 0.03.
     * That is why this is a bounded motif rather than a loop-until-clear, which
     * would have to stop itself and so could outlive the signal that started it. */
    const ALARM_BURSTS = 4;
    const ALARM_SPACING = 1.35;   /* seconds between burst starts */
    const ALARM_TAIL = 0.09;      /* second tone's offset inside a burst */

    const fecesAlarm = () => {
      const t = ctx().currentTime;
      for (let i = 0; i < ALARM_BURSTS; i++) {
        const at = t + i * ALARM_SPACING;
        tone(880, 0.35, 'sine', 0.18, at);
        tone(1320, 0.5, 'sine', 0.14, at + ALARM_TAIL);
      }
    };
    const warnBuzzer = () => {
      const t = ctx().currentTime;
      tone(190, 0.16, 'square', 0.10, t);
      tone(150, 0.16, 'square', 0.10, t + 0.22);
    };

    /* Sound is ON by default; the button mutes it. No persistence is
     * deliberate: a remembered mute would silently survive a reload and into a
     * demo, where it reads as "the alarm didn't fire" rather than "someone
     * muted the page". Starting on every load means a panel observer hears the
     * alarm unless they choose otherwise. */
    let audioOn = true;

    /* Browsers refuse to start audio until a user gesture, so the flag above can
     * be true while the AudioContext is still suspended -- which would mean the
     * button claims sound is on and nothing plays. This one-shot unlock runs on
     * the first real interaction anywhere and then removes itself, after which
     * alarms work with no further opt-in.
     *
     * pointerdown AND keydown both matter: a keyboard user should not have to
     * click to make sound legal, and a touch user should not need a key.
     *
     * canPlay() is the single source of truth for "will this actually be heard":
     * it is derived from the live context state rather than tracked separately,
     * so the flag cannot drift out of step with what the browser is doing. */
    const canPlay = () => (actx !== null && actx.state === 'running');

    const unlock = () => {
      /* Only auto-enabled sound needs this. If the operator muted the page
       * before their first gesture, do not silently start making noise. */
      if (!audioOn) return;
      ctx();
      if (canPlay()) {
        document.removeEventListener('pointerdown', unlock);
        document.removeEventListener('keydown', unlock);
      }
    };
    if (!canPlay()) {
      document.addEventListener('pointerdown', unlock);
      document.addEventListener('keydown', unlock);
    }
    /* Latch rather than plain edge detection: one detection episode is one
     * alarm, however many polls span it. `armed` only returns true once the
     * signal has gone clear again. The gap floor covers the case where the
     * verifier flickers clear/hit faster than a human can reset it. This matters
     * more now that one alarm lasts ~5 s: without the latch a sustained
     * detection would schedule a fresh alarm every poll for as long as the pen
     * stayed dirty. */
    let armed = true;
    let lastAlarmAt = 0;

    if (btn) {
      btn.addEventListener('click', (e) => {
        e.target.blur();
        /* Clicking the button IS a gesture, so this also unlocks audio for the
         * case where the operator's first interaction is the mute toggle itself
         * (they never touched the page body first). */
        ctx();
        audioOn = !audioOn;
        /* Label states what is TRUE, not what a click would do: sound starts on,
         * so the button reads "Sound on" until it is muted. Previously the label
         * advertised the action, which is why "Sound on" used to mean "muted". */
        e.target.textContent = audioOn ? '🔊 Sound on' : '🔇 Sound off';
        e.target.title = audioOn ? 'Disable detection sounds' : 'Enable detection sounds';
        /* Play the alarm once on enable so the operator can hear the tone,
         * length and loudness immediately rather than waiting for a real
         * detection to judge them. Deliberately does not touch `armed` or
         * `lastAlarmAt`: a preview must not consume the one alarm a detection
         * episode is allowed, or a hit arriving seconds later would go silent.
         *
         * Only if the context is actually running: the click that unmutes also
         * unlocks, so this normally plays. The guard covers the browsers that
         * keep the context suspended for a tick after resume(), where scheduling
         * would otherwise be discarded without any error. */
        if (audioOn && canPlay()) fecesAlarm();
      });
    }

    return {
      /* Returns the signal so a caller can reuse it instead of recomputing. */
      update: function (j, sig) {
        const s = sig || fecesSignal(j);
        /* The pig warning is a presence alarm, so it keys off `isPig` and not
         * off `detected`: a pig frame is never `detected` (detected means the
         * feces class), so gating it on that would make it unreachable. */
        if (opts.pigBuzzer && s.isPig && audioOn && canPlay()) warnBuzzer();
        if (!s.detected) {
          armed = true;
        } else {
          const now = Date.now();
          /* canPlay() matters now that sound is on by default: a detection in
           * the first moments after load, before any click, would otherwise be
           * scheduled on a suspended context and lost. Leaving `armed` set
           * means the very next poll still alarms once audio is legal, so a
           * real detection is never swallowed by the autoplay policy. */
          if (audioOn && armed && canPlay() &&
              (now - lastAlarmAt) >= RETRIGGER_GAP_MS) {
            fecesAlarm();
            lastAlarmAt = now;
            armed = false;
          }
        }
        return s;
      },
    };
  }

  function initStatusPoll(opts) {
    const chipsEl = qs(opts.chipsEl);
    const bannerEl = opts.bannerEl ? qs(opts.bannerEl) : null;
    const onUpdate = typeof opts.onUpdate === 'function' ? opts.onUpdate : null;
    if (!chipsEl) return;
    /* A throw inside onUpdate is a bug in the view, not a dead device. Keep it
     * out of the network try so it can never paint the "unreachable" banner
     * while the detector is answering perfectly. */
    function publish(j) {
      if (!onUpdate) return;
      try { onUpdate(j); }
      catch (err) { console.warn('status onUpdate failed', err); }
    }
    async function tick() {
      try {
        const res = await fetch(opts.url);
        if (!res.ok) throw new Error(res.status);
        const j = await res.json();
        chipsEl.innerHTML = statusChips(j);
        publish(j);
        if (bannerEl) bannerEl.hidden = true;
      } catch (err) {
        chipsEl.innerHTML = '<span class="chip chip-off">Device unreachable</span>';
        publish(null);
        if (bannerEl) bannerEl.hidden = false;
      }
    }
    tick();
    setInterval(tick, opts.interval || 5000);
  }

  function confRow(r) {
    const conf = Number(r.confidence || 0);
    const pct = Math.round(conf * 100);
    const trig = r.triggered ? 'Yes' : 'No';
    const badge = trig === 'Yes' ? 'badge-yes' : 'badge-no';
    const verified = r.verified
      ? '<span class="chip chip-verified">✓ ' + (Number(r.yolo_conf) || 0).toFixed(2) + '</span>'
      : '<span class="muted">—</span>';
    const count = Math.max(1, Number(r.instance_count) || 1);
    const countCell = count > 1
      ? '<span class="chip chip-verified">×' + count + '</span>'
      : '<span class="muted">1</span>';
    return '<tr>' +
      '<td class="num">' + r.id + '</td>' +
      '<td>' + esc(r.event_time || '') + '</td>' +
      '<td><div class="conf"><span class="conf-text">' + conf.toFixed(2) +
      '</span><span class="conf-bar"><i style="width:' + pct + '%"></i></span></div></td>' +
      '<td>' + countCell + '</td>' +
      '<td><span class="badge ' + badge + '">' + trig + '</span></td>' +
      '<td class="num">' + (Number(r.spray_duration) || 0) + '</td>' +
      '<td><span class="chip">' + esc(r.source_ip || '') + '</span></td>' +
      '<td><span class="chip">' + esc(r.model_version || 'fomo:v1') + '</span></td>' +
      '<td>' + verified + '</td>' +
      '<td class="muted">' + esc(r.notes || '') + '</td>' +
      '</tr>';
  }

  function initDashboardRefresh(url, interval) {
    async function tick() {
      try {
        const res = await fetch(url);
        if (!res.ok) throw new Error(res.status);
        const j = await res.json();
        const set = (sel, v) => { const el = qs(sel); if (el) el.textContent = v; };
        set('#stat-total', j.total);
        set('#stat-triggered', j.triggered);
        set('#stat-today', j.today);
        set('#stat-avg', (Number(j.avgConf) || 0).toFixed(2));
        const tb = qs('#recent-tbody');
        if (tb) {
          if (Array.isArray(j.recent) && j.recent.length) {
            tb.innerHTML = j.recent.map(confRow).join('');
          } else {
            tb.innerHTML = '<tr><td colspan="10" class="empty">No detections yet.</td></tr>';
          }
        }
      } catch (e) { /* keep last view */ }
    }
    tick();
    setInterval(tick, interval || 10000);
  }

  /* ----- pen calibration -----
   *
   * The pen is a known 20 x 30 cm rectangle in the picture, so four corners
   * are enough to fit the mapping between the image and the pen floor. The
   * camera sits at one end and the sprayer at the other, so the floor is seen
   * in perspective: a flat "pixels to centimetres" factor would be out by up
   * to a factor of two across the pen. A homography is exact, and it needs no
   * camera height, no tilt angle and no lens field of view to be guessed.
   *
   * The matrix maps pen centimetres to a fraction of the image and has eight
   * free terms -- the ninth is fixed at 1, which is what makes the system
   * linear and solvable. The server stores those eight numbers and only ever
   * applies them, so the fit happens once, in the browser, where the operator
   * can see it.
   */
  const penCalibration = {
    /* Fit the matrix to four point pairs {from:[X,Y] cm, to:[u,v] fraction}. */
    solve: function (pairs) {
      if (!pairs || pairs.length < 4) return null;
      /* Two rows per correspondence: the u equation and the v equation. */
      const m = [];
      for (const p of pairs) {
        const X = p.from[0], Y = p.from[1], u = p.to[0], v = p.to[1];
        m.push([X, Y, 1, 0, 0, 0, -u * X, -u * Y, u]);
        m.push([0, 0, 0, X, Y, 1, -v * X, -v * Y, v]);
      }
      const n = m.length;
      /* Gauss-Jordan with partial pivoting. */
      for (let col = 0; col < 8; col++) {
        let piv = col;
        for (let r = col + 1; r < n; r++) {
          if (Math.abs(m[r][col]) > Math.abs(m[piv][col])) piv = r;
        }
        /* A vanishing pivot means the points are degenerate -- four corners
         * in a line, or two of them on the same spot. */
        if (Math.abs(m[piv][col]) < 1e-12) return null;
        if (piv !== col) { const t = m[piv]; m[piv] = m[col]; m[col] = t; }
        for (let r = 0; r < n; r++) {
          if (r === col) continue;
          const f = m[r][col] / m[col][col];
          if (f === 0) continue;
          for (let c = col; c <= 8; c++) m[r][c] -= f * m[col][c];
        }
      }
      const h = [];
      for (let r = 0; r < 8; r++) h.push(m[r][8] / m[r][r]);
      return h;
    },

    /* Pen centimetres -> image fraction. */
    cmToImage: function (h, X, Y) {
      if (!h) return null;
      const w = h[6] * X + h[7] * Y + 1;
      if (Math.abs(w) < 1e-9) return null;
      return [(h[0] * X + h[1] * Y + h[2]) / w, (h[3] * X + h[4] * Y + h[5]) / w];
    },

    /* The inverse matrix, as eight numbers with the bottom-right term at 1.
     * Same adjugate the server uses, so the two agree exactly. */
    invert: function (h) {
      if (!h || h.length !== 8) return null;
      const m00 = h[0], m01 = h[1], m02 = h[2];
      const m10 = h[3], m11 = h[4], m12 = h[5];
      const m20 = h[6], m21 = h[7], m22 = 1;
      const c00 = m11 * m22 - m12 * m21;
      const c01 = -(m10 * m22 - m12 * m20);
      const c02 = m10 * m21 - m11 * m20;
      const c10 = -(m01 * m22 - m02 * m21);
      const c11 = m00 * m22 - m02 * m20;
      const c12 = -(m00 * m21 - m01 * m20);
      const c20 = m01 * m12 - m02 * m11;
      const c21 = -(m00 * m12 - m02 * m10);
      const c22 = m00 * m11 - m01 * m10;
      const adj = [[c00, c10, c20], [c01, c11, c21], [c02, c12, c22]];
      const det = m00 * c00 + m01 * c01 + m02 * c02;
      if (Math.abs(det) < 1e-12) return null;
      const inv = [[0, 0, 0], [0, 0, 0], [0, 0, 0]];
      for (let r = 0; r < 3; r++) {
        for (let c = 0; c < 3; c++) inv[r][c] = adj[r][c] / det;
      }
      const k = inv[2][2];
      if (Math.abs(k) < 1e-12) return null;
      for (let r = 0; r < 3; r++) {
        for (let c = 0; c < 3; c++) inv[r][c] /= k;
      }
      return [inv[0][0], inv[0][1], inv[0][2],
              inv[1][0], inv[1][1], inv[1][2],
              inv[2][0], inv[2][1]];
    },

    /* Image fraction -> pen centimetres. */
    imageToCm: function (inv, u, v) {
      if (!inv) return null;
      const w = inv[6] * u + inv[7] * v + 1;
      if (Math.abs(w) < 1e-9) return null;
      return [(inv[0] * u + inv[1] * v + inv[2]) / w, (inv[3] * u + inv[4] * v + inv[5]) / w];
    },

    /* A normalized box (top-left) -> centimetres on the pen. x/y place the
     * box's top-left corner; w/h are its extent along the pen's two axes,
     * which in perspective is the axis-aligned envelope of the four
     * projected corners rather than a rectangle. */
    boxToCm: function (inv, x, y, w, h) {
      if (!inv) return null;
      const cs = [[x, y], [x + w, y], [x + w, y + h], [x, y + h]].map((c) =>
        penCalibration.imageToCm(inv, c[0], c[1]));
      if (cs.some((p) => !p)) return null;
      const xs = cs.map((p) => p[0]);
      const ys = cs.map((p) => p[1]);
      const tl = penCalibration.imageToCm(inv, x, y);
      return {
        x: tl[0],
        y: tl[1],
        w: Math.max.apply(null, xs) - Math.min.apply(null, xs),
        h: Math.max.apply(null, ys) - Math.min.apply(null, ys),
      };
    },
  };

  /* ----- pen edge analysis -----
   *
   * Clicking the pen's corners by hand is the fiddly part of calibrating it:
   * a few pixels of slop and the fit is wrong. The pen's sides are straight
   * lines in the picture, though, and they are visible -- so the operator's
   * clicks only have to say roughly WHICH side each one is, and the sides can
   * be re-found from the image itself. The corners then follow from the
   * sides' intersections.
   *
   * Everything here works on a plain pixel buffer rather than a canvas, so
   * it can be tested without a browser.
   */
  const penEdges = {
    /* Greyscale, blur, Sobel. The threshold is ADAPTIVE -- the 85th
     * percentile of the gradient magnitudes -- so it works whatever the
     * pen's contrast is, rather than against a hard-coded number that only
     * suits one picture. */
    analyse: function (imageData) {
      const W = imageData.width;
      const H = imageData.height;
      const src = imageData.data;
      const grey = new Float32Array(W * H);
      for (let i = 0, n = W * H; i < n; i++) {
        const o = i * 4;
        grey[i] = 0.299 * src[o] + 0.587 * src[o + 1] + 0.114 * src[o + 2];
      }

      /* A 3x3 box blur: one pass of noise removal. Without it the tiniest
       * speckle outvotes the pen's edge. */
      const blur = new Float32Array(W * H);
      for (let y = 0; y < H; y++) {
        for (let x = 0; x < W; x++) {
          let sum = 0, n = 0;
          for (let dy = -1; dy <= 1; dy++) {
            const yy = y + dy;
            if (yy < 0 || yy >= H) continue;
            for (let dx = -1; dx <= 1; dx++) {
              const xx = x + dx;
              if (xx < 0 || xx >= W) continue;
              sum += grey[yy * W + xx];
              n++;
            }
          }
          blur[y * W + x] = sum / n;
        }
      }

      const mag = new Float32Array(W * H);
      for (let y = 1; y < H - 1; y++) {
        for (let x = 1; x < W - 1; x++) {
          const i = y * W + x;
          const gx = -blur[i - W - 1] - 2 * blur[i - W] - blur[i - W + 1]
                     + blur[i + W - 1] + 2 * blur[i + W] + blur[i + W + 1];
          const gy = -blur[i - W - 1] - 2 * blur[i - 1] - blur[i + W - 1]
                     + blur[i - W + 1] + 2 * blur[i + 1] + blur[i + W + 1];
          mag[i] = Math.hypot(gx, gy);
        }
      }

      /* The threshold has to be above the NOISE, and a percentile of all the
       * samples is no use for that: in a picture of a mostly flat pen floor
       * the 85th percentile lands among the noise, and then every speckle
       * counts as an edge and the "refinement" finds whatever is nearest.
       *
       * What matters is the STRONGEST edge in the picture, so the threshold
       * is a fraction of that. The 99.9th percentile stands in for the
       * maximum, so that one hot pixel cannot drag the bar up with it. */
      const peak = penEdges.percentile(mag, 0.999);
      const threshold = peak * 0.25;

      return {
        W: W, H: H, mag: mag,
        peak: peak,
        threshold: threshold,
      };
    },

    /* The value below which p (0..1) of the samples lie. A histogram
     * rather than a sort: the buffer is ~100k samples and this runs on
     * every frame the operator calibrates. */
    percentile: function (values, p) {
      const MAX = 512;
      let max = 0;
      for (let i = 0; i < values.length; i++) {
        if (values[i] > max) max = values[i];
      }
      if (max <= 0) return 0;

      const hist = new Uint32Array(MAX);
      for (let i = 0; i < values.length; i++) {
        let idx = Math.floor((values[i] / max) * (MAX - 1));
        if (idx < 0) idx = 0;
        if (idx >= MAX) idx = MAX - 1;
        hist[idx]++;
      }

      const want = values.length * p;
      let seen = 0;
      for (let b = 0; b < MAX; b++) {
        seen += hist[b];
        if (seen >= want) return (b / (MAX - 1)) * max;
      }
      return max;
    },

    /**
     * Refine a quadrilateral against the image's edges.
     *
     * `corners` are in the analysis's pixel space, in the order
     * near-left, near-right, far-right, far-left. Each side is re-found by
     * walking along the clicked segment and looking for the strongest edge
     * just off it; the four sides are then intersected for the corners.
     *
     * A side with too little edge support is left as the operator clicked it
     * -- and reported, because a guessed edge is worse than a rough click.
     */
    refine: function (analysis, corners, opts) {
      opts = opts || {};
      const band = opts.band != null ? opts.band : 10;
      const step = opts.step != null ? opts.step : 1.5;
      const minSupport = opts.minSupport != null ? opts.minSupport : 0.15;
      const maxMove = opts.maxMove != null ? opts.maxMove : Infinity;
      const trim = opts.trim != null ? opts.trim : 0.12;

      const sides = [];
      for (let k = 0; k < 4; k++) {
        const a = corners[k];
        const b = corners[(k + 1) % 4];
        sides.push(penEdges.refineSide(analysis, a, b, band, step, minSupport, trim));
      }

      /* The corners are the intersections of the refined sides: corner k sits
       * between side k-1 and side k. */
      const out = [];
      const moved = [];
      const suspicious = [];
      for (let k = 0; k < 4; k++) {
        const prev = sides[(k + 3) % 4];
        const next = sides[k];
        const l1 = prev && prev.ok ? prev.line : penEdges.lineThrough(corners[(k + 3) % 4], corners[k]);
        const l2 = next && next.ok ? next.line : penEdges.lineThrough(corners[k], corners[(k + 1) % 4]);
        const hit = penEdges.intersect(l1, l2);
        const clicked = corners[k];
        if (!hit) {
          out.push(clicked.slice());
          moved.push(0);
          suspicious.push(false);
          continue;
        }
        const d = Math.hypot(hit[0] - clicked[0], hit[1] - clicked[1]);
        out.push(hit);
        moved.push(d);
        suspicious.push(d > maxMove);
      }

      return { corners: out, sides: sides, moved: moved, suspicious: suspicious };
    },

    /* Walk along a segment and find the strongest edge just off it. */
    refineSide: function (analysis, a, b, band, step, minSupport, trim) {
      const W = analysis.W, H = analysis.H, mag = analysis.mag;
      const threshold = analysis.threshold;
      const dx = b[0] - a[0], dy = b[1] - a[1];
      const len = Math.hypot(dx, dy);
      if (len < 6) {
        return { ok: false, support: 0, line: penEdges.lineThrough(a, b) };
      }
      const ux = dx / len, uy = dy / len;
      const nx = -uy, ny = ux;
      const steps = Math.max(2, Math.round(len / step));
      /* The ends of a side are the corners, and a corner BENDS: the edge
       * there is not on the straight line the rest of the side follows.
       * Fitting through those points drags the line towards the corner and
       * puts the intersection in the wrong place, so the ends are trimmed
       * off before the line is fitted. */
      const trimLen = trim ? Math.min(len * 0.4, len * trim) : 0;
      const pts = [];
      let searched = 0;
      for (let s = 0; s <= steps; s++) {
        const t = (s / steps) * len;
        if (t < trimLen || t > len - trimLen) continue;
        searched++;
        const cx = a[0] + ux * t, cy = a[1] + uy * t;
        let bestOff = 0, bestVal = 0;
        for (let o = -band; o <= band; o++) {
          const x = Math.round(cx + nx * o);
          const y = Math.round(cy + ny * o);
          if (x < 0 || y < 0 || x >= W || y >= H) continue;
          const v = mag[y * W + x];
          if (v > bestVal) { bestVal = v; bestOff = o; }
        }
        if (bestVal <= threshold) continue;
        /* Sub-pixel: a parabola through the three samples around the peak
         * says where the edge really is. Without it the corner lands on a
         * whole pixel off, which is the very error this is meant to remove.
         * The parabola gives a SHIFT from the peak, so the peak's own offset
         * has to be added to it. */
        const shift = penEdges.subpixel(mag, W, H, cx + nx * bestOff, cy + ny * bestOff, nx, ny);
        const off = bestOff + shift;
        pts.push([cx + nx * off, cy + ny * off]);
      }
      const support = searched ? pts.length / searched : 0;
      if (support < minSupport || pts.length < 2) {
        return { ok: false, support: support, line: penEdges.lineThrough(a, b) };
      }
      return { ok: true, support: support, line: penEdges.fitLine(pts) };
    },

    /* The offset, to a fraction of a pixel, of the gradient peak. */
    subpixel: function (mag, W, H, x0, y0, nx, ny) {
      const at = (d) => {
        const x = Math.round(x0 + nx * d);
        const y = Math.round(y0 + ny * d);
        if (x < 0 || y < 0 || x >= W || y >= H) return 0;
        return mag[y * W + x];
      };
      const m = at(0);
      const l = at(-1);
      const r = at(1);
      const den = l - 2 * m + r;
      if (Math.abs(den) < 1e-6) return 0;
      const shift = 0.5 * (l - r) / den;
      /* A parabola's vertex cannot be further than half a sample from the
       * peak, or the fit is noise. */
      return Math.max(-1, Math.min(1, shift));
    },

    /* A line through two points. */
    lineThrough: function (a, b) {
      const dx = b[0] - a[0], dy = b[1] - a[1];
      const len = Math.hypot(dx, dy) || 1;
      return { px: a[0], py: a[1], dx: dx / len, dy: dy / len };
    },

    /* The best-fit line through a set of points -- total least squares, so
     * the line is not pulled towards one axis by the aspect of the picture. */
    fitLine: function (pts) {
      let mx = 0, my = 0;
      for (const p of pts) { mx += p[0]; my += p[1]; }
      mx /= pts.length;
      my /= pts.length;
      let sxx = 0, sxy = 0, syy = 0;
      for (const p of pts) {
        const dx = p[0] - mx, dy = p[1] - my;
        sxx += dx * dx; sxy += dx * dy; syy += dy * dy;
      }
      const ang = 0.5 * Math.atan2(2 * sxy, sxx - syy);
      return { px: mx, py: my, dx: Math.cos(ang), dy: Math.sin(ang) };
    },

    /* Where two lines cross. */
    intersect: function (l1, l2) {
      const den = l1.dx * l2.dy - l1.dy * l2.dx;
      if (Math.abs(den) < 1e-6) return null;
      const t = ((l2.px - l1.px) * l2.dy - (l2.py - l1.py) * l2.dx) / den;
      return [l1.px + t * l1.dx, l1.py + t * l1.dy];
    },
  };

  window.__dash = { toast, initStatusPoll, initDashboardRefresh, fecesSignal, createAlerter, penCalibration, penEdges };

  document.addEventListener('DOMContentLoaded', () => {
    initClock();
    initSidebar();
    initFlash();
    initConfirmForms();
  });
})();
