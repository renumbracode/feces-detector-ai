<?php
defined('BASEPATH') OR exit('No direct script access allowed');

class Live extends MY_Controller
{
    public function index()
    {
        $this->load->view('partials/header', ['pageTitle' => 'Live View', 'pageSlug' => 'live', 'streamUrl' => $this->stream_url()]);

        $data = [
            'streamUrl' => $this->stream_url(),
            'statusUrl' => $this->status_url(),
        ];
        $this->load->view('live/index', $data);
        $this->load->view('partials/footer');
    }
}
