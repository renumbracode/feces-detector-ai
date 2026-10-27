<?php
defined('BASEPATH') OR exit('No direct script access allowed');
?>
<section class="panel">
  <div class="panel-head">
    <h3 class="panel-title">Live view</h3>
    <span class="badge live">LIVE</span>
    <span class="badge" id="spray-off-badge" hidden>SPRAY OFF</span>
    <span class="chip" style="margin-left:auto"><?= htmlspecialchars($streamUrl) ?></span>
  </div>

  <div class="conn-banner" id="conn-banner" hidden>Device unreachable — check the stream URL or detector IP in Settings.</div>

  <div class="live-wrap">
    <div class="live-stage" id="live-stage">
      <img id="live-img" src="<?= htmlspecialchars($streamUrl) ?>" alt="Live stream">
      <div class="grid-overlay" aria-hidden="true"></div>
      <div class="axis-labels axis-x" id="axis-x" aria-hidden="true"></div>
      <div class="axis-labels axis-y" id="axis-y" aria-hidden="true"></div>
      <div id="coord-readout">X -- · Y --</div>
      <div id="detect-box"><span id="detect-label"></span></div>
      <!-- extra overlay boxes, one per additional feces instance (multi-instance) -->
    </div>
  </div>

  <div class="panel-body">
    <div class="row">
      <div class="status-chips" id="status-chips"></div>
      <button class="btn btn-ghost btn-sm" id="btn-refresh" type="button">Refresh stream</button>
      <button class="btn btn-ghost btn-sm" id="btn-sound" type="button" title="Disable detection sounds">🔊 Sound on</button>
    </div>

    <!-- Dispatch to an external actuator board ("main ESP32").
         This is the camera board's OWN half of the contract: it publishes the
         coordinates and the actuator board decides. The panel exists so
         "sent and refused" is visibly different from "never arrived". -->
    <div class="target-panel" id="target-panel" hidden>
      <div class="target-panel-head">
        <span class="target-panel-title">Actuator board (main ESP32)</span>
        <span class="chip" id="target-state">--</span>
      </div>
      <div class="target-panel-body" id="target-detail">No dispatch yet.</div>
    </div>
  </div>
</section>

<script>
const STATUS_URL = <?= json_encode($statusUrl) ?>;
document.addEventListener('DOMContentLoaded', () => {
  const stage = document.getElementById('live-stage');
  const box = document.getElementById('detect-box');
  const labelEl = document.getElementById('detect-label');
  const coordEl = document.getElementById('coord-readout');
  const sprayBadge = document.getElementById('spray-off-badge');
  /* Overlay clones for feces instances 2..N in the same frame. */
  const extraBoxes = [];

  /* Graph axes: ticks 0..100 in steps of 10, centred on each gridline. The
   * y-axis skips its 0 because the x-axis already draws it in the corner. */
  const addTicks = (el, axis) => {
    for (let v = 0; v <= 100; v += 10) {
      if (axis === 'y' && v === 0) continue;
      const t = document.createElement('span');
      t.textContent = v;
      if (axis === 'x') t.style.left = v + '%';
      else t.style.top = v + '%';
      el.appendChild(t);
    }
  };
  addTicks(document.getElementById('axis-x'), 'x');
  addTicks(document.getElementById('axis-y'), 'y');

  // ----- overlay square + glow -----
  // The square is always visible: green when the model has just confirmed
  // feces, red when it has not. The poll runs every 2 s, so green is held for
  // a few seconds after a hit instead of strobing on and off.
  const GREEN_HOLD_MS = 4000;
  let lastGreenAt = 0;
  /* Geometry memory: the last real boxes seen, with their timestamp. A single
   * poll that returns no geometry while a detection is still held green must
   * not snap the overlay back to the centre -- the feces are plainly still in
   * frame. The centre dashed square means "nothing here", nothing else. */
  let lastBoxes = [];
  let lastGeomAt = 0;

  /* The detection rules live in main.js so the overlay here and the alert sound
   * on any other page read the same frame the same way. */
  const alerter = __dash.createAlerter({ button: '#btn-sound', pigBuzzer: true });

  function applyDetector(j) {
    const now = Date.now();
    /* `boxes` is the reported multi-instance geometry; `box` (above) is the
     * primary overlay div, with extra `.detect-box` clones for spots 2..N.
     * Keeping them distinct matters: writing classList/style onto the plain
     * JSON object throws, and that used to surface as "device unreachable". */
    const s = __dash.fecesSignal(j);
    let { detected, conf, present, isPig, useVrf, vrfStale, boxData, boxes, instanceCount } = s;

    if (detected) lastGreenAt = now;
    const isGreen = (now - lastGreenAt) < GREEN_HOLD_MS;

    /* Update/apply the geometry hold (see lastBoxes above): fresh geometry is
     * memorised; while a detection is live or held green, the last positions
     * are kept so boxes stay on the feces instead of jumping to the centre. */
    if (boxes && boxes.length) {
      lastBoxes = boxes.slice(0, 8);
      lastGeomAt = now;
    } else if ((detected || isGreen) && lastBoxes.length &&
               (now - lastGeomAt) < GREEN_HOLD_MS) {
      boxes = lastBoxes;
      boxData = lastBoxes[0];
      if (!instanceCount) instanceCount = lastBoxes.length;
    }

    // Fall back to a centred square so the red state is still a real frame.
    const list = (boxes && boxes.length) ? boxes : [boxData];
    const hasGeom = boxData && (Number(boxData.w) > 0.001 || Number(boxData.h) > 0.001);
    const f = (n) => Math.round(Math.max(0, Math.min(1, Number(n) || 0)) * 100) + '%';
    const place = (el, b) => {
      const g = b && (Number(b.w) > 0.001 || Number(b.h) > 0.001);
      el.style.left = f(g ? b.x : 0.30);
      el.style.top = f(g ? b.y : 0.30);
      el.style.width = f(g ? b.w : 0.40);
      el.style.height = f(g ? b.h : 0.40);
    };

    stage.classList.toggle('stage-feces', isGreen);
    stage.classList.toggle('stage-other', !isGreen);

    /* Primary box: always present, carries the label (count when > 1).
     * With no real geometry (nothing detected anywhere in the frame) it draws
     * as a dashed neutral "no contact" outline instead of pretending to be a
     * detection -- every solid green/red box now sits on an actual feces. */
    box.classList.add('on');
    box.classList.toggle('no-contact', !hasGeom);
    box.classList.toggle('feces', hasGeom && isGreen);
    box.classList.toggle('other', hasGeom && !isGreen);
    place(box, list[0]);

    /* Live coordinate readout of the primary box -- dual scale. Pixels use the
     * stream's ACTUAL frame size (naturalWidth/Height, live per MJPEG frame),
     * not a hardcoded 640x480: camera_service warns the sensor can clamp the
     * framesize, and a stale constant would silently mis-report every row.
     * Percentages stay because they line up with the on-screen 0-100 grid.
     * Same fallback the square uses when no detection geometry exists. */
    const geo = (list[0] && (Number(list[0].w) > 0.001 || Number(list[0].h) > 0.001))
      ? list[0] : { x: 0.30, y: 0.30, w: 0.40, h: 0.40 };
    const img = document.getElementById('live-img');
    const fw = img.naturalWidth || 640;
    const fh = img.naturalHeight || 480;
    const clamp = (n) => Math.max(0, Math.min(1, Number(n) || 0));
    const pct = (n) => Math.round(clamp(n) * 100);
    coordEl.textContent =
      'X ' + Math.round(clamp(geo.x) * fw) + ' · Y ' + Math.round(clamp(geo.y) * fh) + ' px | '
      + pct(geo.x) + '% · ' + pct(geo.y) + '% | W ' + pct(geo.w) + '% · H ' + pct(geo.h) + '%';

    /* Say "no answer" rather than "no feces" when the verifier is unreachable.
     * Those are different claims, and only the first one is true. */
    if (vrfStale) {
      labelEl.textContent = 'VERIFY OFFLINE';
    } else {
      const countTxt = instanceCount > 1 ? ' ×' + instanceCount : '';
      labelEl.textContent = isGreen
        ? 'FECES' + countTxt + ' ' + Math.round(conf * 100) + '%'
        : (isPig ? 'PIG' : 'NO FECES');
    }

    /* Extra boxes for instances 2..N: plain .detect-box divs, no labels. */
    const extras = list.slice(1, 8);
    while (extraBoxes.length > extras.length) extraBoxes.pop().remove();
    while (extraBoxes.length < extras.length) {
      const el = document.createElement('div');
      el.className = 'detect-box';
      stage.appendChild(el);
      extraBoxes.push(el);
    }
    extraBoxes.forEach((el, i) => {
      el.classList.add('on');
      el.classList.toggle('feces', isGreen);
      el.classList.toggle('other', !isGreen);
      place(el, extras[i]);
    });

    return { isGreen, detected, isPig, present, useVrf, conf, vrfStale, boxData };
  }

  /* The dispatch panel.
   *
   * The camera board publishes coordinates for an external actuator board to
   * aim at, and reports the outcome. Three states matter and they look the
   * same from the outside unless they are spelled out:
   *   - publishing switched off (nothing is being sent at all)
   *   - sent, and the actuator board REFUSED it (with the reason it gave)
   *   - the actuator board could not be reached at all
   */
  const targetPanel = document.getElementById('target-panel');
  const targetState = document.getElementById('target-state');
  const targetDetail = document.getElementById('target-detail');

  const REASON_TEXT = {
    disabled: 'the actuator board is switched off',
    low_water: 'not enough water',
    sensor_invalid: 'the water sensor is not readable',
    busy: 'it was already cleaning',
    duplicate: 'already handled',
    stale_id: 'the message was too old',
    no_target: 'no usable box',
    solve_failed: 'the coordinates did not solve to an angle',
    out_of_range: 'outside the servo limits',
  };

  function renderTarget(j) {
    const t = (j && j.target) ? j.target : null;
    if (!t || !t.enabled) {
      targetPanel.hidden = true;
      return;
    }
    targetPanel.hidden = false;

    let state, detail;
    if (t.lastDetectionId === 0 || t.lastOkAgeMs === -1) {
      state = 'WAITING';
      detail = 'Publishing to ' + (t.url || '(no url)') + ' — no dispatch yet.';
    } else if (!t.lastOk) {
      state = 'UNREACHABLE';
      detail = 'The actuator board did not answer. It is at ' + (t.url || '(no url)')
             + '. Check that it is powered and on this network.';
    } else if (t.lastAccepted) {
      state = 'ACCEPTED';
      detail = 'Detection #' + t.lastDetectionId
             + ' accepted — nozzle aimed at pan ' + Number(t.lastPan || 0).toFixed(1)
             + '°, tilt ' + Number(t.lastTilt || 0).toFixed(1) + '°'
             + (t.source ? ' (from ' + t.source + ')' : '') + '.';
    } else {
      state = 'REFUSED';
      const why = REASON_TEXT[t.lastReason] || t.lastReason || 'no reason given';
      detail = 'Detection #' + t.lastDetectionId + ' refused: ' + why + '.';
    }

    targetState.textContent = state;
    targetState.className = 'chip ' + (
      state === 'ACCEPTED' ? 'chip-verified' :
      state === 'WAITING' ? '' : 'chip-off');
    targetDetail.textContent = detail;
  }

  __dash.initStatusPoll({
    url: STATUS_URL,
    chipsEl: '#status-chips',
    bannerEl: '#conn-banner',
    interval: 1500,
    onUpdate: (j) => {
      /* The device is the authority on the pump switch: show the badge only
       * when it reports sprayEnabled === false (missing key on old firmware
       * means "enabled", not "off"). A null status means the device did not
       * answer at all, which says nothing about the switch -- keep the badge
       * hidden rather than guessing. */
      sprayBadge.hidden = !(j && j.sprayEnabled === false);
      renderTarget(j);
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
