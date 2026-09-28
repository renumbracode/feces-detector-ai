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
        $notes      = isset($_GET['notes']) ? trim(substr(strip_tags((string) $_GET['notes']), 0, 255)) : null;

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

        $id = $this->Detection_model->insert([
            'confidence'      => $confidence,
            'triggered'       => $triggered,
            'spray_duration'  => $sprayMs,
            'source_ip'       => $source,
            'model_version'   => $model,
            'notes'           => $notes,
            'yolo_conf'       => $yoloConf,
            'verified'        => $verified,
        ]);

        $this->output->set_output(json_encode(['ok' => true, 'id' => $id]));
    }

    /** GET /api/status — proxy the ESP32-S3 /status JSON to the dashboard JS. */
    public function status()
    {
        $body = $this->http_get($this->status_url());
        $this->output->set_output($body !== null ? $body : json_encode(['ok' => false, 'error' => 'unreachable']));
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
}
