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

            if ($action === 'settings') {
                $streamUrl = trim((string) $this->input->post('stream_url'));
                if ($streamUrl !== '' && filter_var($streamUrl, FILTER_VALIDATE_URL) === false) {
                    $streamUrl = '';
                }
                $verifyUrl = trim((string) $this->input->post('verify_url'));
                if ($verifyUrl !== '' && filter_var($verifyUrl, FILTER_VALIDATE_URL) === false) {
                    $verifyUrl = $this->verify_url();
                }
                $vals = [
                    'esp32_ip'          => substr(preg_replace('/[^0-9a-zA-Z.:]/', '', (string) $this->input->post('esp32_ip')), 0, 45),
                    'esp_port'          => substr(preg_replace('/[^0-9]/', '', (string) $this->input->post('esp_port')), 0, 6),
                    'stream_url'        => substr($streamUrl, 0, 255),
                    'verify_url'        => substr($verifyUrl, 0, 255),
                    'spray_duration_ms' => max(1, (int) $this->input->post('spray_duration_ms') ?: 5000),
                    'cooldown_ms'       => max(0, (int) $this->input->post('cooldown_ms') ?: 300000),
                    'threshold'         => max(0, min(1, (float) $this->input->post('threshold'))),
                ];
                foreach ($vals as $k => $v) {
                    $this->Setting_model->set($k, (string) $v);
                }
                $flash = ['ok', 'Settings saved.'];
            }
        }

        $this->load->view('partials/header', ['pageTitle' => 'Control', 'pageSlug' => 'control', 'streamUrl' => $this->stream_url()]);

        $data = [
            'flash'   => $flash,
            'statusUrl' => $this->status_url(),
            'verifyUrl' => $this->verify_url(),
            'detectorBase' => $this->detector_url(''),
            'settings' => [
                'esp32_ip'          => $this->Setting_model->get('esp32_ip', '192.168.1.39'),
                'esp_port'          => $this->Setting_model->get('esp_port', '80'),
                'stream_url'        => $this->Setting_model->get('stream_url', ''),
                'verify_url'        => $this->Setting_model->get('verify_url', ''),
                'spray_duration_ms' => $this->Setting_model->get('spray_duration_ms', '5000'),
                'cooldown_ms'       => $this->Setting_model->get('cooldown_ms', '300000'),
                'threshold'         => $this->Setting_model->get('threshold', '0.60'),
            ],
        ];
        $this->load->view('control/index', $data);
        $this->load->view('partials/footer');
    }
}
