<?php
defined('BASEPATH') OR exit('No direct script access allowed');

class Api extends MY_Controller
{
    public function __construct()
    {
        parent::__construct();
        $this->output->set_content_type('application/json');
        $this->load->helper('url');
    }

    /** GET /api/latest — dashboard auto-refresh payload. */
    public function latest()
    {
        $stats = $this->Detection_model->stats();
        $this->output->set_output(json_encode([
            'total'     => $stats['total'],
            'triggered' => $stats['triggered'],
            'today'     => $stats['today'],
            'avgConf'   => $stats['avgConf'],
            'latest'    => $this->Detection_model->latest(),
            'recent'    => $this->Detection_model->recent(10),
        ]));
    }

    /**
     * GET /api/insert — insert a detection (called by the ESP32-S3).
     * Accepts the same query params as the old insert_detection.php plus
     * optional yolo_conf / verified from the verify server.
     */
    public function insert()
    {
        $confidence = isset($_GET['confidence']) ? round((float) $_GET['confidence'], 4) : 0.0;
        $triggered  = isset($_GET['triggered']) && (int) $_GET['triggered'] === 1 ? 1 : 0;
        $sprayMs    = isset($_GET['spray_ms']) ? max(0, (int) $_GET['spray_ms']) : 0;
        $source     = isset($_GET['esp']) ? substr(preg_replace('/[^0-9a-zA-Z.:_\-]/', '', (string) $_GET['esp']), 0, 45) : 'esp32s3';
        $model      = isset($_GET['model']) ? substr(preg_replace('/[^0-9a-zA-Z0-9._\-]/', '', (string) $_GET['model']), 0, 20) : 'fomo:v1';
        // The device sends notes as a URL query value, where a raw space would
        // invalidate the URL, so it uses underscores and we restore them here.
        // PHP has already turned any '+' into a space by this point, which is
        // why the device must not rely on '+' for its own words.
        $notes = isset($_GET['notes'])
            ? trim(substr(strip_tags(str_replace('_', ' ', (string) $_GET['notes'])), 0, 255))
            : null;
        // Multi-instance: how many feces spots were in the frame. Older
        // firmware omits it; default 1 keeps history rows compatible.
        $instanceCount = isset($_GET['count']) ? max(1, (int) $_GET['count']) : 1;

        // Primary feces box (top-left normalized 0..1, the device /status
        // convention). All-or-nothing: pre-coordinate firmware leaves NULL.
        $box = ['x' => null, 'y' => null, 'w' => null, 'h' => null];
        if (isset($_GET['x'], $_GET['y'], $_GET['w'], $_GET['h'])
            && is_numeric($_GET['x']) && is_numeric($_GET['y'])
            && is_numeric($_GET['w']) && is_numeric($_GET['h'])) {
            $bx = (float) $_GET['x']; $by = (float) $_GET['y'];
            $bw = (float) $_GET['w']; $bh = (float) $_GET['h'];
            if ($bx >= 0 && $bx <= 1 && $by >= 0 && $by <= 1
                && $bw > 0 && $bw <= 1 && $bh > 0 && $bh <= 1) {
                $box = ['x' => round($bx, 4), 'y' => round($by, 4),
                        'w' => round($bw, 4), 'h' => round($bh, 4)];
            }
        }

        /* The same box in centimetres on the pen, when the pen has been
         * calibrated. The normalized columns stay the canonical record: this
         * is a snapshot taken now, so re-calibrating the pen later never
         * rewrites what a past detection meant.
         *
         * The device sends the frame size it published from, because the
         * sensor can clamp the framesize away from the 640x480 we asked for.
         * If the picture no longer has the shape the calibration was fitted
         * against, every centimetre would be wrong -- so the row keeps NULL
         * instead of carrying a number nobody should trust. */
        $cmBox = ['x' => null, 'y' => null, 'w' => null, 'h' => null];
        if ($box['x'] !== null) {
            $this->load->library('Pen_calibration');
            $pen = $this->pen_calibration;
            if ($pen->is_calibrated()) {
                $fw = (isset($_GET['fw']) && is_numeric($_GET['fw'])) ? (int) $_GET['fw'] : 0;
                $fh = (isset($_GET['fh']) && is_numeric($_GET['fh'])) ? (int) $_GET['fh'] : 0;
                if ($pen->aspect_matches($fw, $fh)) {
                    $c = $pen->box_to_cm($box['x'], $box['y'], $box['w'], $box['h']);
                    if ($c !== null) {
                        $cmBox = ['x' => round($c['x'], 2), 'y' => round($c['y'], 2),
                                  'w' => round($c['w'], 2), 'h' => round($c['h'], 2)];
                    }
                }
            }
        }

        if ($confidence < 0 || $confidence > 1) {
            $this->output->set_status_header(400);
            $this->output->set_output(json_encode(['ok' => false, 'error' => 'confidence out of range']));
            return;
        }

        // Optional verify fields (when the Python server inserts straight here)
        $yoloConf = null;
        $verified = 0;
        if (isset($_GET['yolo_conf']) && is_numeric($_GET['yolo_conf'])) {
            $yoloConf = round((float) $_GET['yolo_conf'], 4);
            $verified = 1;
        }

        $row = [
            'confidence'      => $confidence,
            'triggered'       => $triggered,
            'spray_duration'  => $sprayMs,
            'source_ip'       => $source,
            'model_version'   => $model,
            'notes'           => $notes,
            'instance_count'  => $instanceCount,
            'x'               => $box['x'],
            'y'               => $box['y'],
            'w'               => $box['w'],
            'h'               => $box['h'],
            'yolo_conf'       => $yoloConf,
            'verified'        => $verified,
        ];

        /* The centimetre columns only exist once the table has been
         * migrated. An install that has not run the ALTER yet still logs
         * detections normally -- it just does not get the centimetres. */
        if ($this->db->field_exists('x_cm', 'detections')) {
            $row['x_cm'] = $cmBox['x'];
            $row['y_cm'] = $cmBox['y'];
            $row['w_cm'] = $cmBox['w'];
            $row['h_cm'] = $cmBox['h'];
        }

        $id = $this->Detection_model->insert($row);

        $this->output->set_output(json_encode(['ok' => true, 'id' => $id]));
    }

    /**
     * POST /api/calibrate — store, or clear, the pen calibration.
     *
     * The operator clicks the four corners of the pen in the live view; the
     * browser fits the homography to them and sends the eight numbers. The
     * server only validates and stores them: solving it here would mean
     * solving it twice, and the browser can show the operator what the fit
     * looks like BEFORE it is committed.
     *
     * An empty matrix clears the calibration, which puts the live view back
     * on its plain percentage grid.
     */
    public function calibrate()
    {
        $this->load->library('Pen_calibration');

        $raw = trim((string) $this->input->post('h'));
        if ($raw === '') {
            $this->Setting_model->set('pen_homography', '');
            $this->output->set_output(json_encode(['ok' => true, 'cleared' => true]));
            return;
        }

        $h = Pen_calibration::parse_matrix($raw);
        if ($h === null) {
            $this->output->set_status_header(400);
            $this->output->set_output(json_encode(
                ['ok' => false, 'error' => 'the calibration needs eight numbers']));
            return;
        }
        /* A matrix that cannot be inverted describes points that are in a
         * line, or two corners on the same spot. */
        if (Pen_calibration::invert($h) === null) {
            $this->output->set_status_header(400);
            $this->output->set_output(json_encode(
                ['ok' => false, 'error' => 'those four points do not describe a pen -- click the corners again']));
            return;
        }

        $widthCm = (float) $this->input->post('width_cm');
        $lengthCm = (float) $this->input->post('length_cm');
        if ($widthCm < 1 || $widthCm > 500 || $lengthCm < 1 || $lengthCm > 500) {
            $this->output->set_status_header(400);
            $this->output->set_output(json_encode(
                ['ok' => false, 'error' => 'the pen size must be between 1 and 500 cm']));
            return;
        }

        $this->Setting_model->set('pen_homography', Pen_calibration::format_matrix($h));
        $this->Setting_model->set('pen_width_cm', (string) round($widthCm, 2));
        $this->Setting_model->set('pen_length_cm', (string) round($lengthCm, 2));

        /* The shape of the picture the fit was made against. Rows published
         * from a differently shaped frame are refused their centimetres. */
        $aspect = (float) $this->input->post('aspect');
        $this->Setting_model->set('pen_calib_aspect', $aspect > 0 ? (string) round($aspect, 6) : '');

        $this->output->set_output(json_encode(['ok' => true, 'h' => $h]));
    }

    /**
     * POST /api/push_calibration — send the pen calibration to the actuator
     * board.
     *
     * The board that owns the servos is the one that has to aim, so it
     * needs the calibration the dashboard fitted to the pen's corners: it
     * cannot work that out itself, because it never sees the picture.
     *
     * Only the matrix and the pen's size are sent. The nozzle's pose and
     * the servo's offsets are the board's own business and stay where the
     * operator set them on its setup page -- overwriting them from here
     * would quietly undo a physical adjustment.
     */
    public function push_calibration()
    {
        $this->load->library('Pen_calibration');
        $pen = $this->pen_calibration;

        if (!$pen->is_calibrated()) {
            $this->output->set_status_header(400);
            $this->output->set_output(json_encode(
                ['ok' => false, 'error' => 'the pen\'s size is not set, so there is nothing to tell the board']));
            return;
        }

        $url = $this->actuator_config_url();
        if ($url === '') {
            $this->output->set_status_header(400);
            $this->output->set_output(json_encode(
                ['ok' => false, 'error' => 'the actuator board\'s address is not known -- set it on the Control page']));
            return;
        }

        $body = http_build_query([
            'pen_ok' => '1',
            'pen_w'  => $pen->width_cm(),
            'pen_l'  => $pen->length_cm(),
            'pen_flip_y' => $pen->flip_y() ? '1' : '0',
        ]);

        /* The matrix goes too, but only when there is one. Without it the
         * board uses the plain model, which is what the pen's size alone
         * describes. */
        if ($pen->is_fitted()) {
            $body .= '&' . http_build_query([
                'pen_h' => Pen_calibration::format_matrix($pen->matrix()),
            ]);
        }

        $reply = $this->http_post_form($url, $body);
        if ($reply === null) {
            $this->output->set_status_header(502);
            $this->output->set_output(json_encode(
                ['ok' => false, 'error' => 'the actuator board at ' . $url . ' did not answer']));
            return;
        }

        /* The board answers with JSON, and says whether it saved. */
        $j = json_decode($reply, true);
        if (is_array($j) && isset($j['ok']) && !$j['ok']) {
            $this->output->set_status_header(502);
            $this->output->set_output(json_encode(
                ['ok' => false, 'error' => 'the actuator board refused it: ' . (isset($j['error']) ? $j['error'] : 'no reason given')]));
            return;
        }

        $this->output->set_output(json_encode([
            'ok' => true,
            'url' => $url,
            'note' => 'The actuator board restarts when its settings change.',
        ]));
    }

    /** GET /api/status — proxy the ESP32-S3 /status JSON to the dashboard JS. */
    public function status()
    {
        $body = $this->http_get($this->status_url());
        if ($body !== null) {
            $this->record_dispatch($body);
        }
        $this->output->set_output($body !== null ? $body : json_encode(['ok' => false, 'error' => 'unreachable']));
    }

    /**
     * Write the camera board's latest dispatch to the actuator board down.
     *
     * The board reports the outcome of its last publish in the /status
     * document. Several pages poll /status at once, so the same dispatch is
     * seen many times; the model's unique key on detection_id makes the
     * repeats harmless.
     *
     * Best effort: if the table does not exist yet (a deployment that has
     * not re-imported database.sql) the dispatch is simply not recorded. That
     * must never break the status proxy -- the live view depends on it.
     */
    private function record_dispatch($body)
    {
        $j = json_decode($body, true);
        if (!is_array($j) || empty($j['target']) || !is_array($j['target'])) {
            return;
        }
        $t = $j['target'];
        $id = isset($t['lastDetectionId']) ? (int) $t['lastDetectionId'] : 0;
        if ($id <= 0) {
            return;
        }
        if (!$this->db->table_exists('target_dispatches')) {
            return;
        }

        $lastOk = !empty($t['lastOk']);
        $accepted = !empty($t['lastAccepted']);
        $reason = isset($t['reason']) ? (string) $t['reason'] : '';
        if (!$lastOk) {
            $outcome = 'unreachable';
        } elseif ($accepted) {
            $outcome = 'accepted';
        } else {
            $outcome = 'refused';
        }

        $this->load->model('Target_model');
        $this->Target_model->record(
            $id,
            $outcome,
            $reason,
            isset($t['lastPan']) ? $t['lastPan'] : null,
            isset($t['lastTilt']) ? $t['lastTilt'] : null,
            isset($t['source']) ? $t['source'] : ''
        );
    }

    /**
     * GET /api/verify — third leg of the detection pipeline. Called by the
     * ESP32-S3 after it POSTed a frame to the YOLOv8 verifier: records the
     * verifier's confidence on the row it inserted in leg 1 so the history
     * view can show the "YOLOv8 verified" badge.
     */
    public function verify()
    {
        $id = isset($_GET['detection_id']) ? (int) $_GET['detection_id'] : 0;
        $yoloConf = isset($_GET['yolo_conf']) && is_numeric($_GET['yolo_conf'])
            ? round((float) $_GET['yolo_conf'], 4) : 0.0;

        if ($id <= 0) {
            $this->output->set_status_header(400);
            $this->output->set_output(json_encode(['ok' => false, 'error' => 'bad detection_id']));
            return;
        }
        if ($yoloConf < 0 || $yoloConf > 1) {
            $this->output->set_status_header(400);
            $this->output->set_output(json_encode(['ok' => false, 'error' => 'yolo_conf out of range']));
            return;
        }

        $this->Detection_model->mark_verified($id, $yoloConf);
        $this->output->set_output(json_encode(['ok' => true, 'id' => $id, 'yolo_conf' => $yoloConf]));
    }

    /**
     * GET /api/cameras — camera registry (multi-camera groundwork: one row
     * per ESP32-S3 unit; how many a pen needs depends on its size/layout).
     * POST /api/cameras?id=N&active=0|1 — activate/deactivate a camera.
     * POST /api/cameras?label=cam-02&ip=...&pen=...&notes=... — add one.
     */
    public function cameras()
    {
        $this->load->model('Camera_model');
        if (!$this->db->table_exists('cameras')) {
            $this->output->set_output(json_encode(['ok' => false, 'error' => 'cameras table missing', 'cameras' => []]));
            return;
        }

        if ($_SERVER['REQUEST_METHOD'] === 'POST') {
            if (isset($_GET['id'])) {
                $id = (int) $_GET['id'];
                if (!$this->Camera_model->find($id)) {
                    $this->output->set_status_header(404);
                    $this->output->set_output(json_encode(['ok' => false, 'error' => 'unknown camera id']));
                    return;
                }
                if (isset($_GET['active'])) {
                    $this->Camera_model->set_active($id, (int) $_GET['active'] === 1);
                }
                $this->output->set_output(json_encode(['ok' => true, 'camera' => $this->Camera_model->find($id)]));
                return;
            }

            $label = isset($_GET['label'])
                ? substr(preg_replace('/[^0-9a-zA-Z._\-]/', '', (string) $_GET['label']), 0, 32)
                : '';
            if ($label === '') {
                $this->output->set_status_header(400);
                $this->output->set_output(json_encode(['ok' => false, 'error' => 'label required']));
                return;
            }
            $id = $this->Camera_model->insert([
                'label'          => $label,
                'esp32_ip'       => isset($_GET['ip']) ? substr(preg_replace('/[^0-9a-fA-F.:]/', '', (string) $_GET['ip']), 0, 45) : '',
                'pen_label'      => isset($_GET['pen']) ? substr(strip_tags((string) $_GET['pen']), 0, 32) : '',
                'stream_url'     => isset($_GET['stream']) ? substr(strip_tags((string) $_GET['stream']), 0, 255) : '',
                'location_notes' => isset($_GET['notes']) ? substr(strip_tags(str_replace('_', ' ', (string) $_GET['notes'])), 0, 255) : '',
                'active'         => 1,
            ]);
            $this->output->set_output(json_encode(['ok' => true, 'id' => $id]));
            return;
        }

        $this->output->set_output(json_encode([
            'ok'      => true,
            'cameras' => $this->Camera_model->all(),
        ]));
    }
}
