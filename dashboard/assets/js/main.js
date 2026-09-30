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
    if (typeof j.lastConfidence === 'number') {
      parts.push('<span class="chip">Conf ' + j.lastConfidence.toFixed(2) + '</span>');
    }
    if (j.model) {
      parts.push('<span class="chip">' + esc(j.model) + '</span>');
    }
    /* The overlay square is driven by the server-side YOLOv8 answer while the
     * on-device model is still the pre-export stub, so say so plainly. */
    if (j.vrf && j.vrf.valid) {
      parts.push('<span class="chip ' + (j.vrf.detected ? 'chip-verified' : 'chip-off') + '">' +
        'YOLO ' + Number(j.vrf.conf || 0).toFixed(2) + (j.vrf.detected ? ' hit' : ' clear') +
        '</span>');
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
   * verifier every 2 s and reports how old the answer is in vrf.ageMs, so an
   * answer older than this means the verifier stopped answering (crashed, PC
   * asleep, network down) rather than the pen actually being clean. Without
   * this the last "no feces" would stay on screen indefinitely. */
  const VRF_STALE_MS = 6000;

  function fecesSignal(j) {
    j = j || {};
    const v = j.vrf;
    const vrfStale = !!(v && v.valid && Number(v.ageMs) > VRF_STALE_MS);
    const vrfFresh = !!(v && v.valid && !vrfStale);
    /* Until a real FOMO model is exported the device reports fomo:stub, so the
     * server-side YOLOv8 answer (vrf) is the only real detection available. */
    if (vrfFresh) {
      return {
        detected: !!v.detected,
        conf: Number(v.conf || 0),
        present: true,
        isPig: false,
        useVrf: true,
        vrfStale: false,
        boxData: v,
      };
    }
    /* The verifier is not answering, so fall back to the on-device FOMO class. */
    const present = !!j.objectPresent && typeof j.classId === 'number' && j.classId >= 0;
    return {
      detected: present && j.classId === 0,
      conf: Number(j.lastConfidence || 0),
      present: present,
      isPig: present && j.classId === 1,
      useVrf: false,
      vrfStale: vrfStale,
      boxData: j.box,
    };
  }

  /* ----- alert sounds -----
   * WebAudio only, no asset files: two short oscillators per sound is enough
   * for a pen alarm and keeps the dashboard a single-script page.
   *
   * Browsers refuse to start audio until a user gesture, so the alerter starts
   * disarmed and is unlocked by the opt-in button. */
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
    const chime = () => {
      const t = ctx().currentTime;
      tone(880, 0.35, 'sine', 0.18, t);
      tone(1320, 0.5, 'sine', 0.14, t + 0.09);
    };
    const warnBuzzer = () => {
      const t = ctx().currentTime;
      tone(190, 0.16, 'square', 0.10, t);
      tone(150, 0.16, 'square', 0.10, t + 0.22);
    };

    let audioOn = false;
    /* Latch rather than plain edge detection: one detection episode is one
     * chime, however many polls span it. `armed` only returns true once the
     * signal has gone clear again. The gap floor covers the case where the
     * verifier flickers clear/hit faster than a human can reset it. */
    let armed = true;
    let lastChimeAt = 0;

    if (btn) {
      btn.addEventListener('click', (e) => {
        e.target.blur();
        ctx();
        audioOn = !audioOn;
        e.target.textContent = audioOn ? '🔇 Sound off' : '🔊 Sound on';
        e.target.title = audioOn ? 'Disable detection sounds' : 'Enable detection sounds';
      });
    }

    return {
      /* Returns the signal so a caller can reuse it instead of recomputing. */
      update: function (j, sig) {
        const s = sig || fecesSignal(j);
        /* The pig warning is a presence alarm, so it keys off `isPig` and not
         * off `detected`: a pig frame is never `detected` (detected means the
         * feces class), so gating it on that would make it unreachable. */
        if (opts.pigBuzzer && s.isPig && audioOn) warnBuzzer();
        if (!s.detected) {
          armed = true;
        } else {
          const now = Date.now();
          if (audioOn && armed && (now - lastChimeAt) >= RETRIGGER_GAP_MS) {
            chime();
            lastChimeAt = now;
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
    return '<tr>' +
      '<td class="num">' + r.id + '</td>' +
      '<td>' + esc(r.event_time || '') + '</td>' +
      '<td><div class="conf"><span class="conf-text">' + conf.toFixed(2) +
      '</span><span class="conf-bar"><i style="width:' + pct + '%"></i></span></div></td>' +
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
            tb.innerHTML = '<tr><td colspan="9" class="empty">No detections yet.</td></tr>';
          }
        }
      } catch (e) { /* keep last view */ }
    }
    tick();
    setInterval(tick, interval || 10000);
  }

  window.__dash = { toast, initStatusPoll, initDashboardRefresh, fecesSignal, createAlerter };

  document.addEventListener('DOMContentLoaded', () => {
    initClock();
    initSidebar();
    initFlash();
    initConfirmForms();
  });
})();
