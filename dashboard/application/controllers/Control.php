<?php
defined('BASEPATH') OR exit('No direct script access allowed');

class Control extends MY_Controller
{
    public function index()
    {
        $flash = null;

        if ($this->input->method() === 'post') {
            $action = $this->input->post('action');

            if ($action === 'spray') {
                $duration = max(1, (int) $this->input->post('duration')
                        ?: (int) $this->Setting_model->get('spray_duration_ms', '5000'));
                $resp = $this->http_get($this->detector_url('/spray?duration=' . $duration));
                $flash = $resp !== null
                    ? ['ok', 'Spray command sent to ESP32-S3 (duration ' . $duration . ' ms).']
                    : ['err', 'Could not reach the device. Check its IP in Settings.'];
            }

            /* Spray kill switch. The AUTHORITATIVE state lives on the device
             * (it drives the relay); the dashboard only sends the act and
             * mirrors the answer into settings for display when offline. */
            if ($action === 'spray_toggle') {
                $enable = $this->input->post('enable') === '1' ? '1' : '0';
                $resp = $this->http_get($this->detector_url('/spray?enable=' . $enable));
                $j = $resp !== null ? json_decode($resp, true) : null;
                if (is_array($j) && array_key_exists('sprayEnabled', $j)) {
                    $on = !empty($j['sprayEnabled']);
                    $this->Setting_model->set('spray_enabled', $on ? '1' : '0');
                    $flash = ['ok', $on
                        ? 'Spray switched ON: automatic sprays resumed on the device.'
                        : 'Spray switched OFF: pump disabled on the device. Detections still alert and log. "Spray now" still works as a manual override.'];
                } else {
                    $flash = ['err', 'Could not reach the device to change the spray switch. Check its IP in Settings.'];
                }
            }

            /* Point the camera board at an external actuator board ("main
             * ESP32"). This is a DEVICE setting, not a dashboard one: the
             * address and the on/off switch live in the camera board's NVS,
             * because that is the thing that makes the calls.
             *
             * GET /target on the device applies it live -- no reboot, so the
             * detection loop is not interrupted. */
            if ($action === 'target') {
                $url = trim((string) $this->input->post('target_url'));
                $enable = $this->input->post('target_enabled') === '1' ? '1' : '0';
                /* The device sanitises the URL too, but reject an obviously
                 * broken one here rather than storing it. */
                if ($url !== '' && filter_var($url, FILTER_VALIDATE_URL) === false) {
                    $url = '';
                }
                $query = 'url=' . rawurlencode($url) . '&enable=' . $enable;
                $resp = $this->http_get($this->detector_url('/target?' . $query), 5);
                $j = $resp !== null ? json_decode($resp, true) : null;
                if (is_array($j) && array_key_exists('ok', $j) && !empty($j['ok'])) {
                    $this->Setting_model->set('target_url', $url);
                    $this->Setting_model->set('target_enabled', $enable);
                    $flash = ['ok', $enable === '1'
                        ? 'Publishing to the actuator board is ON (' . ($url ?: 'no URL set') . ').'
                        : 'Publishing to the actuator board is OFF.'];
                } else {
                    $flash = ['err', 'Could not reach the device to change the actuator-board settings. Check its IP in Settings.'];
                }
            }

            if ($action === 'settings') {
                $streamUrl = trim((string) $this->input->post('stream_url'));
                if ($streamUrl !== '' && filter_var($streamUrl, FILTER_VALIDATE_URL) === false) {
                    $streamUrl = '';
                }
                $verifyUrl = trim((string) $this->input->post('verify_url'));
                if ($verifyUrl !== '' && filter_var($verifyUrl, FILTER_VALIDATE_URL) === false) {
                    $verifyUrl = $this->verify_url();
                }
                /* The pen's size, and how far apart its centimetre gridlines
                 * sit. The size is part of the calibration, not a free-standing
                 * number: the fit that maps the picture onto the pen was made
                 * for a particular size, so changing the size invalidates it
                 * and the operator is told to calibrate again rather than
                 * being left with numbers that describe a pen that no longer
                 * exists. */
                $penWidth = (float) $this->input->post('pen_width_cm');
                $penLength = (float) $this->input->post('pen_length_cm');
                $penWidth = max(1.0, min(500.0, $penWidth > 0 ? $penWidth : 20.0));
                $penLength = max(1.0, min(500.0, $penLength > 0 ? $penLength : 30.0));
                $oldWidth = (float) $this->Setting_model->get('pen_width_cm', '20');
                $oldLength = (float) $this->Setting_model->get('pen_length_cm', '30');
                $sizeChanged = (abs($penWidth - $oldWidth) > 0.01 || abs($penLength - $oldLength) > 0.01);

                $gridStep = (float) $this->input->post('grid_step_cm');
                $gridStep = max(0.5, min(50.0, $gridStep > 0 ? $gridStep : 5.0));

                /* Which end of the picture the camera is at. The picture's
                 * bottom is the pen's near end by default; a camera
                 * mounted the other way round flips it. */
                $flipY = ((string) $this->input->post('pen_flip_y')) === '1' ? '1' : '0';

                /* Where the pen calibration can be pushed to. Blank means
                 * "work it out from the actuator board's target URL", which
                 * is the same host with a different path. */
                $actuatorConfig = trim((string) $this->input->post('actuator_config_url'));
                if ($actuatorConfig !== '' && filter_var($actuatorConfig, FILTER_VALIDATE_URL) === false) {
                    $actuatorConfig = '';
                }

                $vals = [
                    'esp32_ip'          => substr(preg_replace('/[^0-9a-zA-Z.:]/', '', (string) $this->input->post('esp32_ip')), 0, 45),
                    'esp_port'          => substr(preg_replace('/[^0-9]/', '', (string) $this->input->post('esp_port')), 0, 6),
                    'stream_url'        => substr($streamUrl, 0, 255),
                    'verify_url'        => substr($verifyUrl, 0, 255),
                    'spray_duration_ms' => max(1, (int) $this->input->post('spray_duration_ms') ?: 5000),
                    'cooldown_ms'       => max(0, (int) $this->input->post('cooldown_ms') ?: 300000),
                    'threshold'         => max(0, min(1, (float) $this->input->post('threshold'))),
                    'pen_width_cm'      => (string) round($penWidth, 2),
                    'pen_length_cm'     => (string) round($penLength, 2),
                    'grid_step_cm'      => (string) round($gridStep, 2),
                    'pen_flip_y'        => $flipY,
                    'actuator_config_url' => substr($actuatorConfig, 0, 255),
                ];
                foreach ($vals as $k => $v) {
                    $this->Setting_model->set($k, (string) $v);
                }
                if ($sizeChanged) {
                    /* A fit was made for a particular size, so it no longer
                     * describes the pen. Drop it: the plain model takes over,
                     * and it follows the new size by itself. */
                    $hadFit = trim($this->Setting_model->get('pen_homography', '')) !== '';
                    $this->Setting_model->set('pen_homography', '');
                    $flash = $hadFit
                        ? ['ok', 'Settings saved. The pen size changed, so the fit to the picture was cleared — the plain model applies again, which needs no fitting.']
                        : ['ok', 'Settings saved. The grid follows the pen\'s new size.'];
                } else {
                    $flash = ['ok', 'Settings saved.'];
                }
            }

            /* Camera registry (multi-camera groundwork): activate/deactivate or
             * add a unit. How many a pen needs depends on its size/layout -- see
             * docs/scaling-and-multi-camera.md. */
            if ($action === 'camera_toggle') {
                $this->load->model('Camera_model');
                if (!$this->db->table_exists('cameras')) {
                    $flash = ['err', 'Cameras table missing — re-import database.sql.'];
                } else {
                    $id = (int) $this->input->post('camera_id');
                    $active = (int) $this->input->post('active') === 1 ? 1 : 0;
                    if ($this->Camera_model->find($id)) {
                        $this->Camera_model->set_active($id, $active);
                        $flash = ['ok', 'Camera ' . ($active ? 'activated' : 'deactivated') . '.'];
                    } else {
                        $flash = ['err', 'Unknown camera.'];
                    }
                }
            }

            if ($action === 'camera_add') {
                $this->load->model('Camera_model');
                if (!$this->db->table_exists('cameras')) {
                    $flash = ['err', 'Cameras table missing — re-import database.sql.'];
                } else {
                    $label = substr(preg_replace('/[^0-9a-zA-Z._\-]/', '', (string) $this->input->post('label')), 0, 32);
                    if ($label === '') {
                        $flash = ['err', 'Camera label is required.'];
                    } else {
                        $this->Camera_model->insert([
                            'label'          => $label,
                            'esp32_ip'       => substr(preg_replace('/[^0-9a-fA-F.:]/', '', (string) $this->input->post('ip')), 0, 45),
                            'pen_label'      => substr(strip_tags((string) $this->input->post('pen')), 0, 32),
                            'stream_url'     => '',
                            'location_notes' => substr(strip_tags(str_replace(' ', '_', (string) $this->input->post('notes'))), 0, 255),
                            'active'         => 1,
                        ]);
                        $flash = ['ok', 'Camera ' . $label . ' added.'];
                    }
                }
            }
        }

        $this->load->model('Camera_model');
        /* Older installs may not have imported the cameras table yet; show an
         * empty registry instead of a 500. */
        $cameras = $this->db->table_exists('cameras') ? $this->Camera_model->all() : [];
        $this->load->view('partials/header', ['pageTitle' => 'Control', 'pageSlug' => 'control', 'streamUrl' => $this->stream_url()]);

        $data = [
            'flash'   => $flash,
            'statusUrl' => $this->status_url(),
            'verifyUrl' => $this->verify_url(),
            'detectorBase' => $this->detector_url(''),
            'cameras' => $cameras,
            /* Last known switch state (mirrored on every toggle). The live
             * page shows the device's real state from /status; this drives
             * the Control panel's button direction even when the device is
             * offline, which is exactly when the operator needs to see it. */
            'sprayEnabled' => $this->Setting_model->get('spray_enabled', '1') !== '0',
            'settings' => [
                'esp32_ip'          => $this->Setting_model->get('esp32_ip', '192.168.1.39'),
                'esp_port'          => $this->Setting_model->get('esp_port', '80'),
                'stream_url'        => $this->Setting_model->get('stream_url', ''),
                'verify_url'        => $this->Setting_model->get('verify_url', ''),
                'spray_duration_ms' => $this->Setting_model->get('spray_duration_ms', '5000'),
                'cooldown_ms'       => $this->Setting_model->get('cooldown_ms', '300000'),
                'threshold'         => $this->Setting_model->get('threshold', '0.60'),
                /* The actuator-board settings live on the DETECTOR board; this
                 * is only a mirror of the last value applied, so the form
                 * shows something sensible when the board is offline. */
                'target_url'        => $this->Setting_model->get('target_url', ''),
                'target_enabled'    => $this->Setting_model->get('target_enabled', '0'),
                /* The pen, which is the calibration target for the live view's
                 * centimetre grid. */
                'pen_width_cm'      => $this->Setting_model->get('pen_width_cm', '20'),
                'pen_length_cm'     => $this->Setting_model->get('pen_length_cm', '30'),
                'grid_step_cm'      => $this->Setting_model->get('grid_step_cm', '5'),
                'pen_calibrated'    => trim($this->Setting_model->get('pen_homography', '')) !== '',
                'pen_flip_y'        => $this->Setting_model->get('pen_flip_y', '0'),
                'actuator_config_url' => $this->Setting_model->get('actuator_config_url', ''),
                'actuator_config_effective' => $this->actuator_config_url(),
            ],
        ];
        $this->load->view('control/index', $data);
        $this->load->view('partials/footer');
    }
}
