<?php
defined('BASEPATH') OR exit('No direct script access allowed');
?>
  </main>

  <footer class="foot">
    <span>Pigpen Feces Detector · capstone demo</span>
    <span>ESP32-S3 FOMO + server YOLOv8</span>
  </footer>
</div>

<div class="toasts" id="toasts"></div>
<?php /* Versioned for the same reason as the CSS in header.php, and it matters
       * more here: main.js gains exports (fecesSignal, createAlerter) that the
       * views call at load time, so a cached older copy makes every view throw
       * before it draws. See the cache-busting note in header.php. */
$jsPath = FCPATH . 'assets/js/main.js';
$jsVer  = @filemtime($jsPath) ?: '0';
?>
<script src="<?= base_url('assets/js/main.js') ?>?v=<?= $jsVer ?>"></script>
</body>
</html>
