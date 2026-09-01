<?php
$pageTitle = 'Detection History';
$pageSlug = 'history';
require __DIR__ . '/config.php';
require __DIR__ . '/partials/header.php';

$db = db();
$perPage = 25;

$filter = isset($_GET['triggered']) ? (int)$_GET['triggered'] : -1;

$where = '';
$params = [];
if ($filter === 0 || $filter === 1) {
    $where = 'WHERE triggered = ?';
    $params[] = $filter;
}

$stmt = $db->prepare('SELECT COUNT(*) FROM detections ' . $where);
$stmt->execute($params);
$totalRows = (int)$stmt->fetchColumn();

$pages = max(1, (int)ceil($totalRows / $perPage));
$page  = isset($_GET['page']) ? max(1, min($pages, (int)$_GET['page'])) : 1;
$off   = ($page - 1) * $perPage;

$st = $db->prepare('SELECT * FROM detections ' . $where . ' ORDER BY id DESC LIMIT ' . $perPage . ' OFFSET ' . $off);
$st->execute($params);
$rows = $st->fetchAll();

$qs = fn($over) => http_build_query(array_merge(['triggered' => $filter], $over));
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
          <tr><th>#</th><th>Time</th><th>Confidence</th><th>Triggered</th><th>Spray (ms)</th><th>Source</th><th>Model</th><th>Notes</th></tr>
        </thead>
        <tbody>
          <?php foreach ($rows as $r): ?>
          <tr>
            <td class="num"><?= (int)$r['id'] ?></td>
            <td><?= htmlspecialchars($r['event_time']) ?></td>
            <td>
              <div class="conf">
                <span class="conf-text"><?= number_format((float)$r['confidence'], 2) ?></span>
                <span class="conf-bar"><i style="width:<?= (int)round((float)$r['confidence'] * 100) ?>%"></i></span>
              </div>
            </td>
            <td><span class="badge <?= $r['triggered'] ? 'badge-yes' : 'badge-no' ?>"><?= $r['triggered'] ? 'Yes' : 'No' ?></span></td>
            <td class="num"><?= (int)$r['spray_duration'] ?></td>
            <td><span class="chip"><?= htmlspecialchars($r['source_ip'] ?? '') ?></span></td>
            <td><span class="chip"><?= htmlspecialchars($r['model_version'] ?? 'yolov8') ?></span></td>
            <td class="muted"><?= htmlspecialchars($r['notes'] ?? '') ?></td>
          </tr>
          <?php endforeach; ?>
          <?php if (!$rows): ?>
          <tr><td colspan="8" class="empty">No records match this filter.</td></tr>
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

<?php require __DIR__ . '/partials/footer.php'; ?>
