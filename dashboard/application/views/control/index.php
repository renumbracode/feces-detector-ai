<?php
defined('BASEPATH') OR exit('No direct script access allowed');
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
                   value="<?= (int) $settings['spray_duration_ms'] ?>">
            <div class="hint">Sends <code>GET /spray</code> to
              <code><?= htmlspecialchars($detectorBase) ?></code>. Cooldown is bypassed for manual sprays.</div>
          </div>
          <div class="row" style="margin-top:16px">
            <button class="btn btn-danger" type="submit">Spray now</button>
          </div>
        </form>
        <p class="muted" style="margin-top:14px"><strong>ESP32-S3:</strong> set detector IP to the ESP32's LAN address
          (e.g. <code><?= htmlspecialchars($settings['esp32_ip']) ?>:<?= htmlspecialchars($settings['esp_port']) ?></code>).</p>
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
  <div class="panel-head"><h3 class="panel-title">Settings</h3><span class="muted">targets the ESP32-S3 detector</span></div>
  <div class="panel-body">
    <form method="post">
      <input type="hidden" name="action" value="settings">
      <div class="form-grid">
        <div class="field">
          <label for="esp32_ip">Detector IP</label>
          <input id="esp32_ip" type="text" name="esp32_ip" value="<?= htmlspecialchars($settings['esp32_ip']) ?>">
          <div class="hint">ESP32-S3 LAN IP (e.g. <code>192.168.1.39</code>)</div>
        </div>
        <div class="field">
          <label for="esp_port">Detector port</label>
          <input id="esp_port" type="text" name="esp_port" value="<?= htmlspecialchars($settings['esp_port']) ?>">
          <div class="hint">HTTP port on the ESP32 (default <code>80</code>)</div>
        </div>
        <div class="field">
          <label for="stream_url">Stream URL</label>
          <input id="stream_url" type="text" name="stream_url" value="<?= htmlspecialchars($settings['stream_url']) ?>" placeholder="http://192.168.1.39/stream">
          <div class="hint">Leave blank to use <code>http://&lt;ip&gt;/stream</code></div>
        </div>
        <div class="field">
          <label for="verify_url">YOLOv8 verify server URL</label>
          <input id="verify_url" type="text" name="verify_url" value="<?= htmlspecialchars($settings['verify_url']) ?>" placeholder="http://localhost:8000/verify">
          <div class="hint">Python FastAPI server (<code><?= htmlspecialchars($verifyUrl) ?></code>)</div>
        </div>
        <div class="field">
          <label for="spray_duration_ms">Spray duration (ms)</label>
          <input id="spray_duration_ms" type="number" name="spray_duration_ms" min="100" step="100" value="<?= (int) $settings['spray_duration_ms'] ?>">
        </div>
        <div class="field">
          <label for="cooldown_ms">Cooldown between sprays (ms)</label>
          <input id="cooldown_ms" type="number" name="cooldown_ms" min="0" step="1000" value="<?= (int) $settings['cooldown_ms'] ?>">
        </div>
        <div class="field">
          <label for="threshold">Detection threshold (0.00 – 1.00)</label>
          <input id="threshold" type="number" name="threshold" min="0" max="1" step="0.05" value="<?= htmlspecialchars($settings['threshold']) ?>">
        </div>
      </div>
      <div class="row" style="margin-top:16px">
        <button class="btn btn-primary" type="submit">Save settings</button>
      </div>
    </form>
    <p class="muted" style="margin-top:14px">On-device auto-spray is handled locally by the ESP32-S3 using its own
      NVS-stored threshold/cooldown/spray values. These dashboard settings are used for the manual controls and info display.</p>
  </div>
</section>

<script>
const STATUS_URL = <?= json_encode($statusUrl) ?>;
document.addEventListener('DOMContentLoaded', () => {
  __dash.initStatusPoll({ url: STATUS_URL, chipsEl: '#status-chips', interval: 5000 });
});
</script>
