<?php
defined('BASEPATH') OR exit('No direct script access allowed');

$qs = function ($over) use ($filter) {
    $params = ['triggered' => $filter];
    foreach ($over as $k => $v) { $params[$k] = $v; }
    return http_build_query($params);
};
?>
<section class="panel">
  <div class="panel-head">
    <h3 class="panel-title">Detection history</h3>
    <span class="muted"><?= $totalRows ?> records</span>
  </div>
  <div class="panel-body">
    <div class="filter-pills">
      <a class="pill <?= $filter === -1 ? 'active' : '' ?>" href="?<?= $qs(['triggered' => -1]) ?>">All</a>
      <a class="pill <?= $filter === 1 ? 'active' : '' ?>" href="?<?= $qs(['triggered' => 1]) ?>">Sprayed</a>
      <a class="pill <?= $filter === 0 ? 'active' : '' ?>" href="?<?= $qs(['triggered' => 0]) ?>">Not sprayed</a>
    </div>

    <div class="table-wrap">
      <table class="table">
        <thead>
          <tr><th>#</th><th>Time</th><th>Confidence</th><th>Triggered</th><th>Spray (ms)</th><th>Source</th><th>Model</th><th>Verified</th><th>Notes</th></tr>
        </thead>
        <tbody>
          <?php foreach ($rows as $r): ?>
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
            <td><?php if ($r['verified']): ?><span class="chip chip-verified">✓ <?= number_format((float) $r['yolo_conf'], 2) ?></span><?php else: ?><span class="muted">—</span><?php endif; ?></td>
            <td class="muted"><?= htmlspecialchars($r['notes'] ?? '') ?></td>
          </tr>
          <?php endforeach; ?>
          <?php if (!$rows): ?>
          <tr><td colspan="9" class="empty">No records match this filter.</td></tr>
          <?php endif; ?>
        </tbody>
      </table>
    </div>

    <div class="pager">
      <span class="muted">Page <?= $page ?> of <?= $pages ?></span>
      <a class="pg <?= $page <= 1 ? 'disabled' : '' ?>" href="?<?= $qs(['page' => max(1, $page - 1)]) ?>" <?= $page <= 1 ? 'disabled' : '' ?>>&laquo; Prev</a>
      <span class="pg cur"><?= $page ?></span>
      <a class="pg <?= $page >= $pages ? 'disabled' : '' ?>" href="?<?= $qs(['page' => min($pages, $page + 1)]) ?>" <?= $page >= $pages ? 'disabled' : '' ?>>Next &raquo;</a>
    </div>
  </div>
</section>
