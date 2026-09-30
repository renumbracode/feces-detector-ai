<?php
defined('BASEPATH') OR exit('No direct script access allowed');
?>
<section class="panel">
  <div class="panel-head">
    <h3 class="panel-title">Device alert</h3>
    <span class="muted" style="margin-left:auto">alarms on detection, even with the live view closed</span>
    <button class="btn btn-ghost btn-sm" id="btn-sound" type="button" title="Toggle detection sounds">🔊 Sound on</button>
  </div>
  <div class="panel-body">
    <div class="row">
      <div class="status-chips" id="status-chips"></div>
    </div>
  </div>
</section>

<section class="cards">
  <div class="card">
    <div class="card-icon green">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><circle cx="12" cy="12" r="7.5"/><circle cx="12" cy="12" r="11"/></svg>
    </div>
    <div class="card-meta">
      <div class="card-value" id="stat-total"><?= $total ?></div>
      <div class="card-label">Total detections</div>
      <div class="card-sub">all time</div>
    </div>
  </div>
  <div class="card">
    <div class="card-icon blue">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 2.7C12 2.7 5.5 9.6 5.5 14.5a6.5 6.5 0 0 0 13 0C18.5 9.6 12 2.7 12 2.7z"/></svg>
    </div>
    <div class="card-meta">
      <div class="card-value" id="stat-triggered"><?= $triggered ?></div>
      <div class="card-label">Sprays triggered</div>
      <div class="card-sub">pump activations</div>
    </div>
  </div>
  <div class="card">
    <div class="card-icon amber">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="5" width="18" height="16" rx="2"/><line x1="3" y1="10" x2="21" y2="10"/><line x1="8" y1="3" x2="8" y2="7"/><line x1="16" y1="3" x2="16" y2="7"/></svg>
    </div>
    <div class="card-meta">
      <div class="card-value" id="stat-today"><?= $today ?></div>
      <div class="card-label">Detected today</div>
      <div class="card-sub">since midnight</div>
    </div>
  </div>
  <div class="card">
    <div class="card-icon red">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="9"/><path d="M12 12l5-5"/></svg>
    </div>
    <div class="card-meta">
      <div class="card-value" id="stat-avg"><?= number_format((float) $avgConf, 2) ?></div>
      <div class="card-label">Avg confidence</div>
      <div class="card-sub">mean of all detections</div>
    </div>
  </div>
</section>

<div class="two-col">
  <div class="stack">
    <?php if ($latest): ?>
    <section class="panel">
      <div class="panel-head">
        <h3 class="panel-title">Latest detection</h3>
        <span class="muted"><?= htmlspecialchars($latest['event_time']) ?></span>
      </div>
      <div class="panel-body">
        <div class="row">
          <div class="conf conf-lg">
            <span class="conf-text"><?= number_format((float) $latest['confidence'], 2) ?></span>
            <span class="conf-bar"><i style="width:<?= (int) round((float) $latest['confidence'] * 100) ?>%"></i></span>
          </div>
          <span class="badge <?= $latest['triggered'] ? 'badge-yes' : 'badge-no' ?>">
            <?= $latest['triggered'] ? 'Triggered' : 'Not triggered' ?>
          </span>
          <span class="chip">Spray <?= (int) $latest['spray_duration'] ?> ms</span>
          <span class="chip"><?= htmlspecialchars($latest['source_ip'] ?? '') ?></span>
          <?php if ($latest['verified']): ?>
          <span class="chip chip-verified">YOLOv8 ✓ <?= number_format((float) $latest['yolo_conf'], 2) ?></span>
          <?php endif; ?>
        </div>
        <?php if ($latest['notes']): ?>
        <p class="muted" style="margin:12px 0 0"><?= htmlspecialchars($latest['notes']) ?></p>
        <?php endif; ?>
      </div>
    </section>
    <?php endif; ?>

    <section class="panel">
      <div class="panel-head">
        <h3 class="panel-title">Recent activity</h3>
        <span class="muted" style="margin-left:auto">last 10</span>
        <a class="btn btn-ghost btn-sm" href="<?= site_url('history') ?>">View all</a>
      </div>
      <div class="panel-body">
        <div class="table-wrap">
          <table class="table">
            <thead>
              <tr><th>#</th><th>Time</th><th>Confidence</th><th>Triggered</th><th>Spray (ms)</th><th>Source</th><th>Model</th><th>Verified</th><th>Notes</th></tr>
            </thead>
            <tbody id="recent-tbody">
              <?php foreach ($recent as $r): ?>
              <tr>
                <td class="num"><?= (int) $r['id'] ?></td>
                <td><?= htmlspecialchars($r['event_time']) ?></td>
                <td>
                  <div class="conf">
                    <span class="conf-text"><?= number_format((float) $r['confidence'], 2) ?></span>
                    <span class="conf-bar"><i style="width:<?= (int) round((float) $r['confidence'] * 100) ?>%"></i></span>
                  </div>
                </td>
                <td><span class="badge <?= $r['triggered'] ? 'badge-yes' : 'badge-no' ?>"><?= $r['triggered'] ? 'Yes' : 'No' ?></span></td>
                <td class="num"><?= (int) $r['spray_duration'] ?></td>
                <td><span class="chip"><?= htmlspecialchars($r['source_ip'] ?? '') ?></span></td>
                <td><span class="chip"><?= htmlspecialchars($r['model_version'] ?? 'fomo:v1') ?></span></td>
                <td><?php if ($r['verified']): ?><span class="chip chip-verified">✓</span><?php else: ?><span class="muted">—</span><?php endif; ?></td>
                <td class="muted"><?= htmlspecialchars($r['notes'] ?? '') ?></td>
              </tr>
              <?php endforeach; ?>
              <?php if (!$recent): ?>
              <tr><td colspan="9" class="empty">No detections yet — waiting for the device.</td></tr>
              <?php endif; ?>
            </tbody>
          </table>
        </div>
      </div>
    </section>
  </div>

  <div class="stack">
    <section class="panel">
      <div class="panel-head"><h3 class="panel-title">System</h3></div>
      <div class="panel-body">
        <div class="table-wrap">
          <table class="table">
            <tbody>
              <?php foreach ($settings as $s): ?>
              <tr>
                <td class="muted"><?= htmlspecialchars($s['key']) ?></td>
                <td class="num"><?= htmlspecialchars($s['value']) ?></td>
              </tr>
              <?php endforeach; ?>
            </tbody>
          </table>
        </div>
      </div>
    </section>

    <section class="panel">
      <div class="panel-head"><h3 class="panel-title">Quick links</h3></div>
      <div class="panel-body">
        <div class="row">
          <a class="btn btn-ghost" href="<?= site_url('live') ?>">Open live view</a>
          <a class="btn btn-primary" href="<?= site_url('control') ?>">Manual spray</a>
        </div>
      </div>
    </section>
  </div>
</div>

<script>
const LATEST_URL = <?= json_encode(site_url('api/latest')) ?>;
const STATUS_URL = <?= json_encode($statusUrl) ?>;
document.addEventListener('DOMContentLoaded', () => {
  __dash.initDashboardRefresh(LATEST_URL, 10000);

  /* Same alarm as the live view, driven by the same detection rules from
   * main.js, so a detection alerts whether or not anyone is watching the
   * stream. No pig buzzer here: this page has no live frame to point at. */
  const alerter = __dash.createAlerter({ button: '#btn-sound' });
  __dash.initStatusPoll({
    url: STATUS_URL,
    chipsEl: '#status-chips',
    interval: 2000,
    onUpdate: (j) => alerter.update(j),
  });
});
</script>
