<?php
$pageTitle = 'Control';
$pageSlug = 'control';
require __DIR__ . '/config.php';
require __DIR__ . '/partials/header.php';

$flash = null;

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $action = $_POST['action'] ?? '';

    if ($action === 'spray') {
        $duration = max(1, (int)($_POST['duration'] ?? get_setting('spray_duration_ms', '5000')));
        $resp = http_get(esp_url('/spray?duration=' . $duration));
        $flash = $resp !== null
            ? ['ok', 'Spray command sent to device (duration ' . $duration . ' ms).']
            : ['err', 'Could not reach the device. Check its IP in Settings.'];
    }

    if ($action === 'settings') {
        $streamUrl = trim((string)($_POST['stream_url'] ?? ''));
        if ($streamUrl !== '' && filter_var($streamUrl, FILTER_VALIDATE_URL) === false) {
            $streamUrl = '';
        }
        $vals = [
            'esp32_ip'         => substr(preg_replace('/[^0-9a-zA-Z.:]/', '', $_POST['esp32_ip'] ?? ''), 0, 45),
            'stream_url'       => substr($streamUrl, 0, 255),
            'spray_duration_ms'=> max(1, (int)($_POST['spray_duration_ms'] ?? 5000)),
            'cooldown_ms'      => max(0, (int)($_POST['cooldown_ms'] ?? 300000)),
            'threshold'        => max(0, min(1, (float)($_POST['threshold'] ?? 0.60))),
        ];
        foreach ($vals as $k => $v) {
            set_setting($k, (string)$v);
        }
        $flash = ['ok', 'Settings saved.'];
    }
}
?>

<?php if ($flash): ?>
  <div class="flash <?= $flash[0] === 'ok' ? 'ok' : 'err' ?>">
    <span><?= htmlspecialchars($flash[1]) ?></span>
    <button class="dismiss" type="button" aria-label="Dismiss">&times;</button>
  </div>
<?php endif; ?>

<div class="two-col">
  <div class="stack">
    <section class="panel">
      <div class="panel-head"><h3 class="panel-title">Manual spray</h3></div>
      <div class="panel-body">
        <form method="post" data-confirm="Trigger water spray now?">
          <input type="hidden" name="action" value="spray">
          <div class="field">
            <label for="duration">Duration (ms)</label>
            <input id="duration" type="number" name="duration" min="100" step="100"
                   value="<?= (int)get_setting('spray_duration_ms', '5000') ?>">
            <div class="hint">Sends <code>GET /spray</code> to
              <code><?= htmlspecialchars(detector_url('')) ?></code>. Cooldown is bypassed for manual sprays.</div>
          </div>
          <div class="row" style="margin-top:16px">
            <button class="btn btn-danger" type="submit">Spray now</button>
          </div>
        </form>
        <p class="muted" style="margin-top:14px"><strong>Raspberry Pi:</strong> set detector IP to
          <code>raspberrypi.local:8081</code> or the Pi's LAN IP (e.g. <code>192.168.1.50:8081</code>).</p>
      </div>
    </section>
  </div>

  <div class="stack">
    <section class="panel">
      <div class="panel-head"><h3 class="panel-title">Device status</h3></div>
      <div class="panel-body">
        <div class="status-chips" id="status-chips"></div>
      </div>
    </section>
  </div>
</div>

<section class="panel">
  <div class="panel-head"><h3 class="panel-title">Settings</h3><span class="muted">targets the YOLOv8 detector</span></div>
  <div class="panel-body">
    <form method="post">
      <input type="hidden" name="action" value="settings">
      <div class="form-grid">
        <div class="field">
          <label for="esp32_ip">Detector IP</label>
          <input id="esp32_ip" type="text" name="esp32_ip" value="<?= htmlspecialchars(get_setting('esp32_ip', '192.168.1.39')) ?>">
          <div class="hint">Raspberry Pi IP (e.g. <code>192.168.1.50</code> or <code>raspberrypi.local</code>)</div>
        </div>
        <div class="field">
          <label for="stream_url">Stream URL</label>
          <input id="stream_url" type="text" name="stream_url" value="<?= htmlspecialchars(get_setting('stream_url', '')) ?>" placeholder="http://192.168.1.50:8081/stream">
          <div class="hint">RPi: <code>http://raspberrypi.local:8081/stream</code></div>
        </div>
        <div class="field">
          <label for="spray_duration_ms">Spray duration (ms)</label>
          <input id="spray_duration_ms" type="number" name="spray_duration_ms" min="100" step="100" value="<?= (int)get_setting('spray_duration_ms', '5000') ?>">
        </div>
        <div class="field">
          <label for="cooldown_ms">Cooldown between sprays (ms)</label>
          <input id="cooldown_ms" type="number" name="cooldown_ms" min="0" step="1000" value="<?= (int)get_setting('cooldown_ms', '300000') ?>">
        </div>
        <div class="field">
          <label for="threshold">Detection threshold (0.00 – 1.00)</label>
          <input id="threshold" type="number" name="threshold" min="0" max="1" step="0.05" value="<?= htmlspecialchars(get_setting('threshold', '0.60')) ?>">
        </div>
      </div>
      <div class="row" style="margin-top:16px">
        <button class="btn btn-primary" type="submit">Save settings</button>
      </div>
    </form>
    <p class="muted" style="margin-top:14px">These values must match the detector defaults for the automatic behavior;
      the dashboard settings are only used by manual controls and info display.</p>
  </div>
</section>

<script>
const STATUS_URL = <?= json_encode(detector_url('/status')) ?>;
document.addEventListener('DOMContentLoaded', () => {
  __dash.initStatusPoll({ url: STATUS_URL, chipsEl: '#status-chips', interval: 5000 });
});
</script>

<?php require __DIR__ . '/partials/footer.php'; ?>
