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

  /* The detection rules live in main.js so the overlay here and the alert sound
   * on any other page read the same frame the same way. */
  const alerter = __dash.createAlerter({ button: '#btn-sound', pigBuzzer: true });

  function applyDetector(j) {
    const now = Date.now();
    /* `boxData` is the reported geometry; `box` (above) is the overlay div.
     * Keeping them distinct matters: writing classList/style onto the plain
     * JSON object throws, and that used to surface as "device unreachable". */
    const s = __dash.fecesSignal(j);
    const { detected, conf, present, isPig, useVrf, vrfStale, boxData } = s;

    if (detected) lastGreenAt = now;
    const isGreen = (now - lastGreenAt) < GREEN_HOLD_MS;

    // Fall back to a centred square so the red state is still a real frame.
    const hasGeom = boxData && (Number(boxData.w) > 0.001 || Number(boxData.h) > 0.001);
    const f = (n) => Math.round(Math.max(0, Math.min(1, Number(n) || 0)) * 100) + '%';
    const gx = hasGeom ? boxData.x : 0.30;
    const gy = hasGeom ? boxData.y : 0.30;
    const gw = hasGeom ? boxData.w : 0.40;
    const gh = hasGeom ? boxData.h : 0.40;

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
    return { isGreen, detected, isPig, present, useVrf, conf, vrfStale, boxData };
  }

  __dash.initStatusPoll({
    url: STATUS_URL,
    chipsEl: '#status-chips',
    bannerEl: '#conn-banner',
    interval: 1500,
    onUpdate: (j) => {
      const st = applyDetector(j);
      /* The alarm is keyed to the same detection that turns the box green, not
       * to sprayActive. sprayActive is gated by threshold and a 300 s cooldown,
       * so keying audio to it left the laptop silent for minutes at a time
       * while feces were plainly on screen -- the operator watching the laptop
       * was told nothing at all.
       *
       * This does mean the laptop can alarm when the breadboard stays quiet
       * during cooldown. That is the intended trade: the screen and the sound
       * agree with each other, and the buzzer keeps reporting what the hardware
       * actually did. The one alarm per detection episode is latched in
       * createAlerter, so a steady detection does not machine-gun -- which
       * matters more now that a single alarm runs ~5 s. */
      alerter.update(j, st);
    }
  });

  document.getElementById('btn-refresh').addEventListener('click', () => {
    const img = document.getElementById('live-img');
    const base = img.src.split('?')[0];
    img.src = base + '?t=' + Date.now();
  });
});
</script>
