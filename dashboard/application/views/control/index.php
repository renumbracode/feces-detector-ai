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
      <div class="panel-head"><h3 class="panel-title">Spray switch</h3></div>
      <div class="panel-body">
        <div class="row" style="align-items:center;gap:14px">
          <?php if ($sprayEnabled): ?>
            <span class="badge badge-yes">AUTO-SPRAY: ON</span>
            <form method="post" data-confirm="Turn the sprayer OFF? Detections will still alert and log, but the pump will not run.">
              <input type="hidden" name="action" value="spray_toggle">
              <input type="hidden" name="enable" value="0">
              <button class="btn btn-danger" type="submit">Switch spray OFF</button>
            </form>
          <?php else: ?>
            <span class="badge badge-no">AUTO-SPRAY: OFF</span>
            <form method="post">
              <input type="hidden" name="action" value="spray_toggle">
              <input type="hidden" name="enable" value="1">
              <button class="btn" type="submit">Switch spray ON</button>
            </form>
          <?php endif; ?>
        </div>
        <p class="muted" style="margin-top:12px">While OFF: detections still light the LED, sound the buzzer and are recorded in history (noted <em>spray_off</em>); the pump stays quiet and any spray in progress is cut immediately. State is stored on the device and survives reboots.</p>
      </div>
    </section>

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
        </div>        <div class="field">
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

<section class="panel">
  <div class="panel-head">
    <h3 class="panel-title">Actuator board (main ESP32)</h3>
    <span class="muted">the board that owns the pump relay, the servos and the water sensor</span>
  </div>
  <div class="panel-body">
    <p class="muted" style="margin-bottom:14px">
      This detector board detects the feces and sends the coordinates to the actuator
      board, which decides whether it can act on them (water level, servo limits, a
      cycle already running). These settings are stored on the <strong>detector
      board</strong>, not here — it is the thing that makes the calls — and they are
      applied immediately, with no reboot.
      See <code>docs/target-protocol.md</code>.
    </p>
    <form method="post">
      <input type="hidden" name="action" value="target">
      <div class="form-grid">
        <div class="field">
          <label for="target_url">Actuator board target URL</label>
          <input id="target_url" type="text" name="target_url" value="<?= htmlspecialchars($settings['target_url'] ?? '') ?>"
                 placeholder="http://192.168.254.60:8080/target">
          <div class="hint">The <code>POST /target</code> endpoint on the main ESP32</div>
        </div>
        <div class="field">
          <label for="target_enabled">Publish detections</label>
          <select id="target_enabled" name="target_enabled">
            <option value="1"<?= (($settings['target_enabled'] ?? '0') === '1') ? ' selected' : '' ?>>Enabled — send coordinates</option>
            <option value="0"<?= (($settings['target_enabled'] ?? '0') === '0') ? ' selected' : '' ?>>Disabled — detect only</option>
          </select>
          <div class="hint">While the actuator board is connected, switch this board's own pump off so one detection does not spray twice</div>
        </div>
      </div>
      <div class="row" style="margin-top:16px">
        <button class="btn btn-primary" type="submit">Apply to the detector board</button>
      </div>
    </form>
  </div>
</section>

<section class="panel">
  <div class="panel-head">
    <h3 class="panel-title">Cameras</h3>
    <span class="muted">one ESP32-S3 per camera; how many a pen needs depends on its size and layout</span>
  </div>
  <div class="panel-body">
    <?php if (!$cameras): ?>
      <p class="muted">No cameras registered. Re-import <code>database.sql</code> (or add one below) to create the registry.</p>
    <?php else: ?>
      <div class="table-wrap">
        <table class="table">
          <thead><tr><th>Label</th><th>Device IP</th><th>Pen</th><th>Notes</th><th>Status</th><th></th></tr></thead>
          <tbody>
            <?php foreach ($cameras as $c): ?>
            <tr>
              <td><span class="chip"><?= htmlspecialchars($c['label']) ?></span></td>
              <td><?= htmlspecialchars($c['esp32_ip']) ?></td>
              <td><?= htmlspecialchars($c['pen_label']) ?></td>
              <td class="muted"><?= htmlspecialchars(str_replace('_', ' ', $c['location_notes'])) ?></td>
              <td><span class="badge <?= $c['active'] ? 'badge-yes' : 'badge-no' ?>"><?= $c['active'] ? 'Active' : 'Off' ?></span></td>
              <td>
                <form method="post" style="display:inline">
                  <input type="hidden" name="action" value="camera_toggle">
                  <input type="hidden" name="camera_id" value="<?= (int) $c['id'] ?>">
                  <input type="hidden" name="active" value="<?= $c['active'] ? 0 : 1 ?>">
                  <button class="btn btn-ghost btn-sm" type="submit"><?= $c['active'] ? 'Deactivate' : 'Activate' ?></button>
                </form>
              </td>
            </tr>
            <?php endforeach; ?>
          </tbody>
        </table>
      </div>
    <?php endif; ?>

    <form method="post" style="margin-top:18px">
      <input type="hidden" name="action" value="camera_add">
      <div class="form-grid">
        <div class="field">
          <label for="cam-label">Label</label>
          <input id="cam-label" type="text" name="label" placeholder="cam-02" required>
        </div>
        <div class="field">
          <label for="cam-ip">Device IP</label>
          <input id="cam-ip" type="text" name="ip" placeholder="192.168.1.40">
        </div>
        <div class="field">
          <label for="cam-pen">Pen</label>
          <input id="cam-pen" type="text" name="pen" placeholder="Pen A">
        </div>
        <div class="field">
          <label for="cam-notes">Mount notes</label>
          <input id="cam-notes" type="text" name="notes" placeholder="north_corner_overhead">
        </div>
      </div>
      <div class="row" style="margin-top:12px">
        <button class="btn btn-primary" type="submit">Add camera</button>
      </div>
    </form>
  </div>
</section>

<script>
const STATUS_URL = <?= json_encode($statusUrl) ?>;
document.addEventListener('DOMContentLoaded', () => {
  __dash.initStatusPoll({ url: STATUS_URL, chipsEl: '#status-chips', interval: 5000 });
});
</script>
