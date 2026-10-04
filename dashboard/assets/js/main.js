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
     * from a server hit. */
    {
      const sig = fecesSignal(j);
      if (sig.detected) {
        const label = sig.source === 'both' ? 'FOMO+YOLO'
                    : (sig.source === 'server' ? 'via YOLOv8 server' : 'via FOMO device');
        parts.push('<span class="chip chip-verified">Detected ' + esc(label) + '</span>');
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

    /* Draw the box from whichever source produced the detection, preferring the
     * device since its box is the one the on-device path used. */
    const boxData = deviceDetected ? j.box : (serverDetected ? v : null);

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
