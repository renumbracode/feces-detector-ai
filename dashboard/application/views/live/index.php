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
      <!-- Uncalibrated fallback: a plain percentage grid. -->
      <div class="grid-overlay" aria-hidden="true"></div>
      <!-- Calibrated: the pen's own grid, drawn in centimetres. -->
      <canvas class="grid-canvas" id="grid-canvas" aria-hidden="true"></canvas>
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

    <!-- Pen calibration. The camera sees the pen from one end, so the floor
         is in perspective and a flat "pixels to centimetres" factor would be
         out by up to a factor of two across the pen. Clicking the pen's four
         corners fits an exact mapping instead, and needs no camera height,
         tilt or lens field of view to be guessed. -->
    <div class="calib-panel">
      <div class="row">
        <button class="btn btn-ghost btn-sm" id="btn-calibrate" type="button">Fine-tune the pen</button>
        <button class="btn btn-ghost btn-sm" id="btn-calib-clear" type="button" hidden>Back to the plain model</button>
        <button class="btn btn-primary btn-sm" id="btn-calib-save" type="button" hidden>Save calibration</button>
        <button class="btn btn-ghost btn-sm" id="btn-calib-cancel" type="button" hidden>Cancel</button>
        <button class="btn btn-ghost btn-sm" id="btn-push-calib" type="button" hidden>Send to the actuator board</button>
        <span class="muted" id="calib-state"></span>
      </div>
      <div class="calib-hint" id="calib-hint" hidden></div>
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
const STREAM_URL = <?= json_encode($streamUrl) ?>;
/* The pen calibration, as the server holds it. A null matrix means the pen
 * has never been calibrated, and the live view falls back to its plain
 * percentage grid. */
const PEN = {
  h: <?= json_encode($penMatrix) ?>,
  widthCm: <?= json_encode((float) $penWidthCm) ?>,
  lengthCm: <?= json_encode((float) $penLengthCm) ?>,
  stepCm: <?= json_encode((float) $penGridStepCm) ?>,
  fitted: <?= $penFitted ? 'true' : 'false' ?>,
};
document.addEventListener('DOMContentLoaded', () => {
  const stage = document.getElementById('live-stage');
  const box = document.getElementById('detect-box');
  const labelEl = document.getElementById('detect-label');
  const coordEl = document.getElementById('coord-readout');
  const sprayBadge = document.getElementById('spray-off-badge');
  const img = document.getElementById('live-img');
  const canvas = document.getElementById('grid-canvas');
  const ctx = canvas.getContext('2d');
  /* Overlay clones for feces instances 2..N in the same frame. */
  const extraBoxes = [];

  /* ----- the picture, for snapping to the pen's edges -----
   *
   * Clicking the corners by hand is the fiddly part of calibrating the pen:
   * a few pixels of slop and the fit is wrong. The pen's sides are straight
   * lines in the picture, so the clicks only have to say roughly WHICH side
   * each one is, and the sides can be re-found from the image itself.
   *
   * The picture is fetched as ONE frame rather than read off the live
   * element: the stream is cross-origin, so the element's pixels are not
   * readable, and a second connection would be needed anyway. The board
   * sends the frame's length in the multipart header, so the exact number
   * of bytes to read is known. */
  const EDGE_WORK_W = 360;      /* the analysis runs on a picture this wide */
  const EDGE_BAND = 10;         /* how far off the clicked line an edge may be */
  const EDGE_STEP = 1.5;        /* how closely the side is walked */
  const EDGE_MIN_SUPPORT = 0.15;/* how much of a side must carry an edge */
  const EDGE_MAX_MOVE = 25;     /* a corner moving further than this is odd */
  const EDGE_TRIM = 0.12;       /* the ends of a side are the corners, and
                                 * a corner bends: those points are left out
                                 * of the line fit */

  let edgeAnalysis = null;      /* the analysed frame, or null */
  let edgeNote = '';            /* why there is no analysis, if there isn't */
  let calibClicks = [];         /* the clicks, in fractions of the frame */
  let calibSides = [];          /* the refined sides, as the operator clicks */
  let calibRefined = null;      /* the finished refinement */

  function findHeaderEnd(buf) {
    for (let i = 0; i + 3 < buf.length; i++) {
      if (buf[i] === 13 && buf[i + 1] === 10 && buf[i + 2] === 13 && buf[i + 3] === 10) return i;
    }
    return -1;
  }

  /* One frame from the stream, as JPEG bytes. */
  async function fetchFrameBytes(streamUrl) {
    const ac = new AbortController();
    try {
      const res = await fetch(streamUrl, { signal: ac.signal, cache: 'no-store' });
      if (!res.ok || !res.body) throw new Error('the stream answered ' + res.status);
      const reader = res.body.getReader();
      let buf = new Uint8Array(0);
      let headerEnd = -1;
      let contentLen = -1;
      for (;;) {
        const chunk = await reader.read();
        if (chunk.done) throw new Error('the stream ended before a frame arrived');
        const value = chunk.value;
        const next = new Uint8Array(buf.length + value.length);
        next.set(buf, 0);
        next.set(value, buf.length);
        buf = next;
        if (headerEnd < 0) {
          headerEnd = findHeaderEnd(buf);
          if (headerEnd >= 0) {
            const header = new TextDecoder().decode(buf.subarray(0, headerEnd));
            const m = /content-length:\s*(\d+)/i.exec(header);
            if (!m) throw new Error('the stream did not say how long the frame is');
            contentLen = parseInt(m[1], 10);
          }
        }
        if (headerEnd >= 0 && contentLen > 0 && buf.length >= headerEnd + 4 + contentLen) {
          return buf.slice(headerEnd + 4, headerEnd + 4 + contentLen);
        }
      }
    } finally {
      ac.abort();
    }
  }

  /* JPEG bytes -> a pixel buffer, downscaled for the analysis. */
  function decodeFrame(jpegBytes, workW) {
    return new Promise((resolve, reject) => {
      /* The type is taken from the bytes rather than assumed: the board
       * sends JPEG, but nothing says it always will, and a blob labelled
       * with the wrong type is refused by the decoder. */
      let type = 'image/jpeg';
      if (jpegBytes.length > 4 && jpegBytes[0] === 0x3C) {
        type = 'image/svg+xml';
      } else if (jpegBytes.length > 8 && jpegBytes[0] === 0x89 && jpegBytes[1] === 0x50) {
        type = 'image/png';
      }
      const blob = new Blob([jpegBytes], { type: type });
      const url = URL.createObjectURL(blob);
      const im = new Image();
      im.onload = () => {
        try {
          if (!im.naturalWidth || !im.naturalHeight) {
            throw new Error('the frame has no size');
          }
          const w = Math.max(1, Math.round(workW));
          const h = Math.max(1, Math.round(im.naturalHeight * (w / im.naturalWidth)));
          const c = document.createElement('canvas');
          c.width = w;
          c.height = h;
          const cx = c.getContext('2d');
          cx.drawImage(im, 0, 0, w, h);
          const data = cx.getImageData(0, 0, w, h);
          URL.revokeObjectURL(url);
          resolve(data);
        } catch (err) {
          URL.revokeObjectURL(url);
          reject(err);
        }
      };
      im.onerror = () => {
        URL.revokeObjectURL(url);
        reject(new Error('the frame could not be decoded'));
      };
      im.src = url;
    });
  }

  /* Fetch a frame and work out where its edges are. A failure here is not
   * fatal: the operator can still click the corners, they just will not be
   * snapped. */
  async function loadEdges() {
    edgeAnalysis = null;
    edgeNote = '';
    try {
      const bytes = await fetchFrameBytes(STREAM_URL);
      const data = await decodeFrame(bytes, EDGE_WORK_W);
      edgeAnalysis = __dash.penEdges.analyse(data);
      if (!edgeAnalysis.peak) {
        edgeAnalysis = null;
        edgeNote = 'the picture has no edges to find';
      }
    } catch (err) {
      edgeAnalysis = null;
      edgeNote = 'no picture to snap to (' + (err && err.message ? err.message : 'unreachable') + ')';
    }
  }

  /* The sides the operator's clicks have determined so far, each re-found
   * against the image's edges. */
  function refineKnownSides() {
    if (!edgeAnalysis) return [];
    const W = edgeAnalysis.W;
    const H = edgeAnalysis.H;
    const sides = [];
    for (let k = 0; k < 4; k++) {
      const a = calibClicks[k];
      const b = calibClicks[(k + 1) % 4];
      if (!a || !b) { sides.push(null); continue; }
      sides.push(__dash.penEdges.refineSide(
        edgeAnalysis,
        [a[0] * W, a[1] * H],
        [b[0] * W, b[1] * H],
        EDGE_BAND, EDGE_STEP, EDGE_MIN_SUPPORT, EDGE_TRIM));
    }
    return sides;
  }

  /* ----- drawing the calibration ----- */
  function drawCalibOverlay() {
    if (!calibClicks.length) return;
    const w = stageW();
    const h = stageH();
    const px = (f) => [f[0] * w, f[1] * h];

    /* The refined sides, drawn where the image's edges actually are. */
    if (edgeAnalysis) {
      const W = edgeAnalysis.W;
      const H = edgeAnalysis.H;
      ctx.lineWidth = 1.5;
      calibSides.forEach((s) => {
        if (!s || !s.ok) return;
        const l = s.line;
        /* A long way along the line, so it spans the picture. */
        const a = px([(l.px - l.dx * 400) / W, (l.py - l.dy * 400) / H]);
        const b = px([(l.px + l.dx * 400) / W, (l.py + l.dy * 400) / H]);
        ctx.strokeStyle = 'rgba(56, 189, 248, 0.9)';
        ctx.beginPath();
        ctx.moveTo(a[0], a[1]);
        ctx.lineTo(b[0], b[1]);
        ctx.stroke();
      });
    }

    /* The operator's own clicks, so a snap can be seen to have happened. */
    ctx.strokeStyle = 'rgba(248, 250, 252, 0.85)';
    ctx.lineWidth = 1;
    calibClicks.forEach((c) => {
      const p = px(c);
      ctx.beginPath();
      ctx.moveTo(p[0] - 5, p[1]);
      ctx.lineTo(p[0] + 5, p[1]);
      ctx.moveTo(p[0], p[1] - 5);
      ctx.lineTo(p[0], p[1] + 5);
      ctx.stroke();
    });

    /* The corners the fit will actually use. */
    if (calibRefined) {
      ctx.fillStyle = 'rgba(56, 189, 248, 0.95)';
      calibRefined.corners.forEach((c) => {
        const p = px([c[0] / edgeAnalysis.W, c[1] / edgeAnalysis.H]);
        ctx.beginPath();
        ctx.arc(p[0], p[1], 3.5, 0, Math.PI * 2);
        ctx.fill();
      });
    }
  }

  /* ----- the pen's coordinate system -----
   *
   * PEN.h maps pen centimetres to a fraction of the image; PEN.inv is its
   * inverse, which is what turns a detection back into centimetres. The
   * pen's origin is its near-left corner as seen by the camera: X runs
   * across the pen, Y runs away from the camera toward the sprayer. */
  PEN.inv = PEN.h ? __dash.penCalibration.invert(PEN.h) : null;

  const penCalibrated = () => !!(PEN.h && PEN.inv);

  /* Graph axes: ticks 0..100 in steps of 10, centred on each gridline. The
   * y-axis skips its 0 because the x-axis already draws it in the corner.
   * This is the fallback grid, used until the pen is calibrated. */
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

  /* ----- drawing the pen's grid in centimetres -----
   *
   * Every line on the floor is a straight line, and a homography maps
   * straight lines to straight lines, so each gridline can simply be
   * projected and drawn. In perspective the lines converge, which is the
   * honest picture of the geometry: the camera is at one end of the pen
   * looking down it. */
  const stageW = () => stage.clientWidth;
  const stageH = () => stage.clientHeight;

  /* Pen centimetres -> pixels in the stage. */
  const penToPx = (X, Y) => {
    const p = __dash.penCalibration.cmToImage(PEN.h, X, Y);
    if (!p) return null;
    return [p[0] * stageW(), p[1] * stageH()];
  };

  const GRID_LINE = 'rgba(148, 163, 184, 0.45)';
  const GRID_EDGE = 'rgba(203, 213, 225, 0.75)';
  const PEN_EDGE = 'rgba(56, 189, 248, 0.85)';

  function drawPenGrid() {
    if (!penCalibrated()) return;
    const w = stageW();
    const h = stageH();
    if (!w || !h) return;

    const dpr = window.devicePixelRatio || 1;
    if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
      canvas.width = Math.round(w * dpr);
      canvas.height = Math.round(h * dpr);
    }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);

    const W = PEN.widthCm;
    const L = PEN.lengthCm;
    /* A step that does not divide the pen leaves a ragged last row, so fall
     * back to whatever divides both sides when the operator has not picked
     * one. */
    let step = PEN.stepCm;
    if (!step || step <= 0) step = 5;

    /* The pen's outline. */
    const quad = [penToPx(0, 0), penToPx(W, 0), penToPx(W, L), penToPx(0, L)];
    if (quad.some((p) => !p)) return;
    ctx.beginPath();
    ctx.moveTo(quad[0][0], quad[0][1]);
    for (let i = 1; i < quad.length; i++) ctx.lineTo(quad[i][0], quad[i][1]);
    ctx.closePath();
    ctx.strokeStyle = PEN_EDGE;
    ctx.lineWidth = 1.5;
    ctx.stroke();

    /* The gridlines: across the pen at every step, along it at every step. */
    ctx.lineWidth = 1;
    for (let x = step; x < W - 1e-6; x += step) {
      const a = penToPx(x, 0);
      const b = penToPx(x, L);
      if (!a || !b) continue;
      ctx.strokeStyle = GRID_LINE;
      ctx.beginPath();
      ctx.moveTo(a[0], a[1]);
      ctx.lineTo(b[0], b[1]);
      ctx.stroke();
    }
    for (let y = step; y < L - 1e-6; y += step) {
      const a = penToPx(0, y);
      const b = penToPx(W, y);
      if (!a || !b) continue;
      ctx.strokeStyle = GRID_LINE;
      ctx.beginPath();
      ctx.moveTo(a[0], a[1]);
      ctx.lineTo(b[0], b[1]);
      ctx.stroke();
    }

    /* Tick labels, set just inside the pen's edges.
     *
     * The label for X sits on the near edge (Y = 0) and the label for Y on
     * the left edge (X = 0). Each is pushed a little way towards the middle
     * of the pen so it never falls off the picture. */
    const centre = penToPx(W / 2, L / 2);
    const inward = (from, along, toCentre) => {
      /* A vector perpendicular to `along`, pointing towards the pen. */
      let nx = -along[1];
      let ny = along[0];
      const len = Math.hypot(nx, ny) || 1;
      nx /= len; ny /= len;
      const dot = (toCentre[0] - from[0]) * nx + (toCentre[1] - from[1]) * ny;
      if (dot < 0) { nx = -nx; ny = -ny; }
      return [nx, ny];
    };

    const drawLabel = (text, at, dir, colour) => {
      ctx.font = '700 9px ui-monospace, "Cascadia Mono", Consolas, monospace';
      const tw = ctx.measureText(text).width;
      const px = at[0] + dir[0] * 12 - tw / 2;
      const py = at[1] + dir[1] * 12 - 4;
      ctx.fillStyle = 'rgba(2, 6, 23, 0.72)';
      ctx.fillRect(px - 3, py - 1, tw + 6, 11);
      ctx.fillStyle = colour;
      ctx.fillText(text, px, py + 7);
    };

    const nearEdge = [penToPx(0, 0), penToPx(W, 0)];
    const alongX = [nearEdge[1][0] - nearEdge[0][0], nearEdge[1][1] - nearEdge[0][1]];
    const dirX = inward(nearEdge[0], alongX, centre);
    for (let x = 0; x <= W + 1e-6; x += step) {
      const at = penToPx(x, 0);
      if (!at) continue;
      drawLabel(String(Math.round(x)), at, dirX, '#cbd5e1');
    }

    const leftEdge = [penToPx(0, 0), penToPx(0, L)];
    const alongY = [leftEdge[1][0] - leftEdge[0][0], leftEdge[1][1] - leftEdge[0][1]];
    const dirY = inward(leftEdge[0], alongY, centre);
    for (let y = step; y <= L + 1e-6; y += step) {
      const at = penToPx(0, y);
      if (!at) continue;
      drawLabel(String(Math.round(y)), at, dirY, '#cbd5e1');
    }

    /* One word for the unit, so the numbers are not ambiguous. */
    const unitAt = penToPx(W, 0);
    if (unitAt) {
      ctx.font = '700 9px ui-monospace, "Cascadia Mono", Consolas, monospace';
      ctx.fillStyle = 'rgba(2, 6, 23, 0.72)';
      ctx.fillRect(unitAt[0] - 26, unitAt[1] - 12, 30, 12);
      ctx.fillStyle = '#7dd3fc';
      ctx.fillText('cm', unitAt[0] - 22, unitAt[1] - 3);
    }
  }

  function drawAll() {
    if (penCalibrated()) {
      stage.classList.add('pen-calibrated');
      drawPenGrid();
    } else {
      stage.classList.remove('pen-calibrated');
    }
    drawCalibOverlay();
  }

  /* ----- calibration ----- */
  const btnCalibrate = document.getElementById('btn-calibrate');
  const btnCalibClear = document.getElementById('btn-calib-clear');
  const btnCalibSave = document.getElementById('btn-calib-save');
  const btnCalibCancel = document.getElementById('btn-calib-cancel');
  const btnPushCalib = document.getElementById('btn-push-calib');
  const calibHint = document.getElementById('calib-hint');
  const calibState = document.getElementById('calib-state');
  const ACTUATOR_CONFIG_URL = <?= json_encode($actuatorConfigUrl) ?>;

  /* The order the corners are clicked in, and the pen corner each one is.
   * "Near" means the end of the pen closest to the camera, which is the
   * bottom of the picture. */
  let calibPts = [];
  let calibMode = false;

  const setCalibState = (text) => { calibState.textContent = text; };

  function calibCorners() {
    const W = PEN.widthCm;
    const L = PEN.lengthCm;
    return [
      { key: 'near-left',  pos: [0, 0] },
      { key: 'near-right', pos: [W, 0] },
      { key: 'far-right',  pos: [W, L] },
      { key: 'far-left',   pos: [0, L] },
    ];
  }

  function startCalibration() {
    calibMode = true;
    calibPts = [];
    calibClicks = [];
    calibSides = [];
    calibRefined = null;
    stage.classList.add('calibrating');
    btnCalibSave.hidden = true;
    btnCalibCancel.hidden = false;
    calibHint.hidden = false;
    calibHint.textContent = 'Click the pen\'s corners in this order: near-left, near-right, far-right, far-left. ' +
      '"Near" is the end closest to the camera — the bottom of the picture. ' +
      'A rough click is fine: the sides are re-found on the picture itself. ' +
      'This is only needed if the camera is not looking straight down.';
    setCalibState('Looking for the pen in the picture…');
    drawAll();

    /* Get a frame to snap against. A failure is not fatal -- the corners
     * can still be clicked by hand. */
    loadEdges().then(() => {
      if (!calibMode) return;
      if (edgeAnalysis) {
        setCalibState('Ready — click the pen\'s corners. They will snap to the pen\'s edges.');
      } else {
        setCalibState('Ready — click the pen\'s corners precisely: ' + edgeNote + '.');
      }
    });
  }

  function endCalibration(cancelled) {
    calibMode = false;
    calibPts = [];
    calibClicks = [];
    calibSides = [];
    calibRefined = null;
    stage.classList.remove('calibrating');
    btnCalibSave.hidden = true;
    btnCalibCancel.hidden = true;
    calibHint.hidden = true;
    if (cancelled) {
      setCalibState(penCalibrated() ? 'Calibration unchanged.' : '');
      drawAll();
    }
  }

  /* A click on the picture, as a fraction of the image. */
  function clickFraction(e) {
    const r = img.getBoundingClientRect();
    if (!r.width || !r.height) return null;
    return [(e.clientX - r.left) / r.width, (e.clientY - r.top) / r.height];
  }

  stage.addEventListener('click', (e) => {
    if (!calibMode) return;
    const p = clickFraction(e);
    if (!p) return;
    calibPts.push(p);
    calibClicks = calibPts.slice();
    const corners = calibCorners();
    const which = corners[calibPts.length - 1];

    /* Re-find the sides the clicks have determined, so each one visibly
     * snaps onto the pen's edge as it goes. */
    calibSides = refineKnownSides();

    if (calibPts.length < 4) {
      const found = calibSides.filter((s) => s && s.ok).length;
      setCalibState('Corner ' + calibPts.length + ' of 4 (' + which.key + ') placed'
        + (edgeAnalysis ? ' — ' + found + ' side(s) snapped to the edges' : '') + '.');
      drawAll();
      return;
    }

    /* The four clicks, refined against the picture's edges. */
    let cornersForFit = calibPts;
    let note = '';
    if (edgeAnalysis) {
      const W = edgeAnalysis.W;
      const H = edgeAnalysis.H;
      const working = calibPts.map((c) => [c[0] * W, c[1] * H]);
      const r = __dash.penEdges.refine(edgeAnalysis, working, {
        band: EDGE_BAND,
        step: EDGE_STEP,
        minSupport: EDGE_MIN_SUPPORT,
        maxMove: EDGE_MAX_MOVE,
        trim: EDGE_TRIM,
      });
      calibRefined = r;
      const snapped = r.sides.filter((s) => s.ok).length;
      cornersForFit = r.corners.map((c) => [c[0] / W, c[1] / H]);
      const lost = 4 - snapped;
      if (lost) {
        note = lost + ' side(s) had no edge and were left as clicked';
      } else {
        note = 'all four sides snapped to the pen\'s edges';
      }
      if (r.suspicious.some((s) => s)) {
        note += ' — but a corner moved a long way, so check the preview';
      }
    } else {
      note = 'corners taken from your clicks (' + edgeNote + ')';
    }

    /* Fit, then check the fit puts the corners back where they were. A
     * mis-click would otherwise be stored as a confident wrong answer. */
    const pairs = corners.map((c, i) => ({ from: c.pos, to: cornersForFit[i] }));
    const h = __dash.penCalibration.solve(pairs);
    const inv = h ? __dash.penCalibration.invert(h) : null;
    if (!h || !inv) {
      setCalibState('Those four points do not describe a pen. Click again, more carefully.');
      calibPts = [];
      calibClicks = [];
      calibSides = [];
      calibRefined = null;
      drawAll();
      return;
    }
    let worst = 0;
    pairs.forEach((pr) => {
      const back = __dash.penCalibration.cmToImage(h, pr.from[0], pr.from[1]);
      if (!back) { worst = Infinity; return; }
      worst = Math.max(worst, Math.abs(back[0] - pr.to[0]), Math.abs(back[1] - pr.to[1]));
    });
    if (worst > 0.01) {
      setCalibState('Those points do not fit a rectangle well enough. Click again, more carefully.');
      calibPts = [];
      calibClicks = [];
      calibSides = [];
      calibRefined = null;
      drawAll();
      return;
    }
    /* Preview it before committing. */
    PEN.h = h;
    PEN.inv = inv;
    drawAll();
    btnCalibSave.hidden = false;
    setCalibState('Looks right? ' + note.charAt(0).toUpperCase() + note.slice(1) + '.');
  });

  btnCalibrate.addEventListener('click', () => {
    startCalibration();
  });

  btnCalibCancel.addEventListener('click', () => endCalibration(true));

  btnCalibSave.addEventListener('click', async () => {
    if (!PEN.h) return;
    btnCalibSave.disabled = true;
    setCalibState('Saving…');
    const body = new URLSearchParams();
    body.set('h', PEN.h.join(','));
    body.set('width_cm', String(PEN.widthCm));
    body.set('length_cm', String(PEN.lengthCm));
    const nw = img.naturalWidth || 640;
    const nh = img.naturalHeight || 480;
    body.set('aspect', String(nw / nh));
    try {
      const res = await fetch('<?= site_url('api/calibrate') ?>', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: body.toString(),
      });
      const j = await res.json();
      if (!res.ok || !j.ok) {
        setCalibState('Not saved: ' + (j.error || 'the server said no'));
        btnCalibSave.disabled = false;
        return;
      }
      endCalibration(false);
      btnCalibClear.hidden = false;
      btnPushCalib.hidden = false;
      setCalibState('Pen fitted to the picture — the grid is in centimetres. Send it to the actuator board so the sprayer aims from the same numbers.');
      drawAll();
    } catch (err) {
      setCalibState('Not saved: the dashboard could not be reached');
      btnCalibSave.disabled = false;
    }
  });

  btnCalibClear.addEventListener('click', async () => {
    btnCalibClear.disabled = true;
    try {
      await fetch('<?= site_url('api/calibrate') ?>', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'h=',
      });
    } catch (err) { /* the reload below is the real answer either way */ }
    window.location.reload();
  });

  /* Send the calibration to the actuator board.
   *
   * The board that owns the servos is the one that has to aim, and it never
   * sees the picture, so it cannot work the pen's corners out for itself.
   * The board's own settings -- the nozzle's pose and the servo's offsets --
   * are left alone: those are physical facts the operator set on its setup
   * page, and overwriting them from here would quietly undo an adjustment.
   */
  btnPushCalib.addEventListener('click', async () => {
    btnPushCalib.disabled = true;
    const was = calibState.textContent;
    setCalibState('Sending the calibration to the actuator board…');
    try {
      const res = await fetch('<?= site_url('api/push_calibration') ?>', { method: 'POST' });
      const j = await res.json();
      if (!res.ok || !j.ok) {
        setCalibState('Not sent: ' + (j.error || 'the dashboard said no'));
      } else {
        setCalibState('Sent to ' + j.url + '. ' + (j.note || ''));
      }
    } catch (err) {
      setCalibState('Not sent: the dashboard could not be reached');
    }
    btnPushCalib.disabled = false;
    if (!calibState.textContent) setCalibState(was);
  });

  /* The pen's size and grid step are settings; keep the page's copy honest
   * about them.
   *
   * The plain model needs no calibration at all: the camera looks straight
   * down, so the picture IS the pen and every centimetre is the same size as
   * every other one. Fine-tuning it fits the picture instead, which is what
   * a camera that is not square-on needs. */
  setCalibState(penCalibrated()
    ? (PEN.fitted
        ? 'Pen fitted to the picture — ' + PEN.widthCm + ' cm x ' + PEN.lengthCm + ' cm.'
        : 'Pen ' + PEN.widthCm + ' cm x ' + PEN.lengthCm + ' cm — the picture is the pen.')
    : 'The pen\'s size is not set, so the grid is showing percentages.');
  btnCalibClear.hidden = !PEN.fitted;
  btnPushCalib.hidden = !penCalibrated();

  /* Redraw when the picture's size changes. */
  window.addEventListener('resize', drawAll);
  img.addEventListener('load', drawAll);
  if (img.complete) drawAll();

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

    /* Live coordinate readout of the primary box -- three scales.
     *
     * Pixels use the stream's ACTUAL frame size (naturalWidth/Height, live
     * per MJPEG frame), not a hardcoded 640x480: camera_service warns the
     * sensor can clamp the framesize, and a stale constant would silently
     * mis-report every row.
     *
     * Percentages stay because they line up with the on-screen 0-100 grid.
     *
     * Centimetres are the pen's own measurements, and they only exist once
     * the pen has been calibrated -- otherwise there is no honest number to
     * print, and a made-up one would be worse than none.
     *
     * Same fallback the square uses when no detection geometry exists. */
    const geo = (list[0] && (Number(list[0].w) > 0.001 || Number(list[0].h) > 0.001))
      ? list[0] : { x: 0.30, y: 0.30, w: 0.40, h: 0.40 };
    const fw = img.naturalWidth || 640;
    const fh = img.naturalHeight || 480;
    const clamp = (n) => Math.max(0, Math.min(1, Number(n) || 0));
    const pct = (n) => Math.round(clamp(n) * 100);

    const detail = 'X ' + Math.round(clamp(geo.x) * fw) + ' · Y ' + Math.round(clamp(geo.y) * fh)
      + ' px | ' + pct(geo.x) + '% · ' + pct(geo.y) + '% | W ' + pct(geo.w) + '% · H ' + pct(geo.h) + '%';

    /* Two lines when the pen is calibrated: the pen's own centimetres first,
     * because that is what the operator is reading, and the raw picture
     * scales underneath for anyone who needs to check them.
     *
     * With no real geometry there is no position to report: the centred
     * square is a placeholder, and its centimetres would be a made-up
     * number dressed up as a measurement. */
    coordEl.textContent = '';
    const cmLine = (penCalibrated() && hasGeom)
      ? __dash.penCalibration.boxToCm(PEN.inv, clamp(geo.x), clamp(geo.y),
                                      clamp(geo.w), clamp(geo.h))
      : null;
    if (cmLine) {
      const main = document.createElement('div');
      main.textContent = 'X ' + cmLine.x.toFixed(1) + ' cm · Y ' + cmLine.y.toFixed(1) + ' cm'
        + ' | W ' + cmLine.w.toFixed(1) + ' cm · H ' + cmLine.h.toFixed(1) + ' cm';
      const sub = document.createElement('div');
      sub.className = 'sub';
      sub.textContent = detail;
      coordEl.appendChild(main);
      coordEl.appendChild(sub);
    } else {
      coordEl.textContent = detail;
    }

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
      /* Only a board that works the angles out reports them. One that aims
       * internally sends none, and printing "pan 0.0" for a spray that
       * really happened would be a lie. */
      const angles = t.lastAngles
        ? ' — nozzle aimed at pan ' + Number(t.lastPan || 0).toFixed(1)
          + '°, tilt ' + Number(t.lastTilt || 0).toFixed(1) + '°'
        : '';
      detail = 'Detection #' + t.lastDetectionId + ' accepted' + angles
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
    const base = img.src.split('?')[0];
    img.src = base + '?t=' + Date.now();
  });
});
</script>
