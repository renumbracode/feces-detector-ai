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
    <img id="live-img" src="<?= htmlspecialchars($streamUrl) ?>" alt="Live stream">
  </div>

  <div class="panel-body">
    <div class="row">
      <div class="status-chips" id="status-chips"></div>
      <button class="btn btn-ghost btn-sm" id="btn-refresh" type="button">Refresh stream</button>
    </div>
  </div>
</section>

<script>
const STATUS_URL = <?= json_encode($statusUrl) ?>;
document.addEventListener('DOMContentLoaded', () => {
  __dash.initStatusPoll({ url: STATUS_URL, chipsEl: '#status-chips', bannerEl: '#conn-banner', interval: 5000 });
  document.getElementById('btn-refresh').addEventListener('click', () => {
    const img = document.getElementById('live-img');
    const base = img.src.split('?')[0];
    img.src = base + '?t=' + Date.now();
  });
});
</script>
