<?php
defined('BASEPATH') OR exit('No direct script access allowed');
?>
<section class="panel">
  <div class="panel-head">
    <h3 class="panel-title">Live view</h3>
    <span class="badge live">LIVE</span>
    <span class="chip" style="margin-left:auto"><?= htmlspecialchars($streamUrl) ?></span>
  </div>

  <div class="conn-banner" id="conn-banner" hidden>Device unreachable — check the stream URL or detector IP in Settings.</div>

  <div class="live-wrap">
    <div class="live-stage" id="live-stage">
      <img id="live-img" src="<?= htmlspecialchars($streamUrl) ?>" alt="Live stream">
      <div id="detect-box"><span id="detect-label"></span></div>
    </div>
  </div>

  <div class="panel-body">
    <div class="row">
      <div class="status-chips" id="status-chips"></div>
      <button class="btn btn-ghost btn-sm" id="btn-refresh" type="button">Refresh stream</button>
      <button class="btn btn-ghost btn-sm" id="btn-sound" type="button" title="Toggle detection sounds">🔊 Sound on</button>
    </div>
  </div>
</section>

<script>
const STATUS_URL = <?= json_encode($statusUrl) ?>;
document.addEventListener('DOMContentLoaded', () => {
  const stage = document.getElementById('live-stage');
  const box = document.getElementById('detect-box');
  const labelEl = document.getElementById('detect-label');

  // ----- overlay square + glow -----
  // The square is always visible: green when the model has just confirmed
  // feces, red when it has not. The poll runs every 2 s, so green is held for
  // a few seconds after a hit instead of strobing on and off.
  const GREEN_HOLD_MS = 4000;
  let lastGreenAt = 0;

  // A verifier answer is only trusted while it is fresh. The device polls the
  // verifier every 2 s and reports how old the answer is in vrf.ageMs, so an
  // answer older than this means the verifier stopped answering (crashed, PC
  // asleep, network down) rather than the pen actually being clean. Without
  // this the last "no feces" would stay on screen indefinitely.
  const VRF_STALE_MS = 6000;

  function applyDetector(j) {
    j = j || {};
    const now = Date.now();
    const v = j.vrf;
    /* Until a real FOMO model is exported the device reports fomo:stub, so the
     * server-side YOLOv8 answer (vrf) is the only real detection available. */
    /* An answer the device can no longer refresh is not evidence of anything,
     * so treat it as absent and let the on-device fallback take over. */
    const vrfStale = !!(v && v.valid && Number(v.ageMs) > VRF_STALE_MS);
    const vrfFresh = !!(v && v.valid && !vrfStale);
    const useVrf = vrfFresh;

    /* `boxData` is the reported geometry; `box` (above) is the overlay div.
     * Keeping them distinct matters: writing classList/style onto the plain
     * JSON object throws, and that used to surface as "device unreachable". */
    let detected, conf, boxData, present, isPig = false;
    if (useVrf) {
      detected = !!v.detected;
      conf = Number(v.conf || 0);
      boxData = v;
      present = true;
    } else {
      present = !!j.objectPresent && typeof j.classId === 'number' && j.classId >= 0;
      detected = present && j.classId === 0;
      isPig = present && j.classId === 1;
      conf = Number(j.lastConfidence || 0);
      boxData = j.box;
    }

    if (detected) lastGreenAt = now;
    const isGreen = (now - lastGreenAt) < GREEN_HOLD_MS;

    // Fall back to a centred square so the red state is still a real frame.
    const hasGeom = box && (Number(box.w) > 0.001 || Number(box.h) > 0.001);
    const f = (n) => Math.round(Math.max(0, Math.min(1, Number(n) || 0)) * 100) + '%';
    const gx = hasGeom ? box.x : 0.30;
    const gy = hasGeom ? box.y : 0.30;
    const gw = hasGeom ? box.w : 0.40;
    const gh = hasGeom ? box.h : 0.40;

    stage.classList.toggle('stage-feces', isGreen);
    stage.classList.toggle('stage-other', !isGreen);
    box.classList.add('on');
    box.classList.toggle('feces', isGreen);
    box.classList.toggle('other', !isGreen);
    box.style.left = f(gx);
    box.style.top = f(gy);
    box.style.width = f(gw);
    box.style.height = f(gh);
    /* Say "no answer" rather than "no feces" when the verifier is unreachable.
     * Those are different claims, and only the first one is true. */
    if (vrfStale) {
      labelEl.textContent = 'VERIFY OFFLINE';
    } else {
      labelEl.textContent = isGreen
        ? 'FECES ' + Math.round(conf * 100) + '%'
        : (isPig ? 'PIG' : 'NO FECES');
    }
    return { isGreen, detected, isPig, present, useVrf, conf, vrfStale };
  }

  // ----- WebAudio: green chime on onset, red beeps while non-feces -----
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
  let prevFeces = false;

  document.getElementById('btn-sound').addEventListener('click', (e) => {
    e.target.blur();
    ctx();
    audioOn = !audioOn;
    e.target.textContent = audioOn ? '🔇 Sound off' : '🔊 Sound on';
    e.target.title = audioOn ? 'Disable detection sounds' : 'Enable detection sounds';
  });

  __dash.initStatusPoll({
    url: STATUS_URL,
    chipsEl: '#status-chips',
    bannerEl: '#conn-banner',
    interval: 1500,
    onUpdate: (j) => {
      const st = applyDetector(j);
      if (!st.present) { prevFeces = false; return; }
      if (audioOn) {
        if (st.detected && !prevFeces) chime();
        /* warnBuzzer only for a positively identified pig, which needs a real
         * 2-class FOMO model. The interim red state stays silent. */
        if (st.isPig) warnBuzzer();
      }
      prevFeces = st.detected;
    }
  });

  document.getElementById('btn-refresh').addEventListener('click', () => {
    const img = document.getElementById('live-img');
    const base = img.src.split('?')[0];
    img.src = base + '?t=' + Date.now();
  });
});
</script>
