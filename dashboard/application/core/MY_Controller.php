<?php
defined('BASEPATH') OR exit('No direct script access allowed');

/**
 * Base controller for the Feces Detector dashboard.
 * Provides ESP32-S3 + verify-server URL/HTTP helpers to derived controllers.
 */
class MY_Controller extends CI_Controller
{
    protected function detector_url($path = '')
    {
        $ip = $this->Setting_model->get('esp32_ip', '192.168.1.100');
        $port = $this->Setting_model->get('esp_port', '80');
        $base = 'http://' . $ip . ($port !== '80' ? ':' . $port : '');
        return $base . $path;
    }

    protected function stream_url()
    {
        $u = $this->Setting_model->get('stream_url', '');
        return $u !== '' ? $u : $this->detector_url('/stream');
    }

    protected function status_url()
    {
        $u = $this->Setting_model->get('stream_url', '');
        if ($u !== '') {
            $path = parse_url($u, PHP_URL_PATH);
            if (substr($path, -7) === '/stream') {
                return str_replace('/stream', '/status', $u);
            }
            return $u . '/status';
        }
        return $this->detector_url('/status');
    }

    protected function verify_url()
    {
        return $this->Setting_model->get('verify_url', 'http://localhost:8000/verify');
    }

    /** Simple GET with timeout. Returns response body or null on failure. */
    protected function http_get($url, $timeout = 3)
    {
        $ctx = stream_context_create([
            'http' => ['timeout' => $timeout, 'ignore_errors' => true],
        ]);
        $body = @file_get_contents($url, false, $ctx);
        return $body === false ? null : $body;
    }

    /** POST raw bytes (e.g. JPEG) with a header. Returns decoded JSON or null. */
    protected function http_post_jpeg($url, $bytes, $header)
    {
        $ctx = stream_context_create([
            'http' => [
                'method'        => 'POST',
                'header'        => "Content-Type: image/jpeg\r\nX-Fomo-Conf: " . $header . "\r\n",
                'content'       => $bytes,
                'timeout'       => 10,
                'ignore_errors' => true,
            ],
        ]);
        $body = @file_get_contents($url, false, $ctx);
        if ($body === false) return null;
        $json = json_decode($body, true);
        return is_array($json) ? $json : null;
    }
}
