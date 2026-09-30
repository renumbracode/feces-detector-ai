<?php
defined('BASEPATH') OR exit('No direct script access allowed');

class Dashboard extends MY_Controller
{
    public function index()
    {
        $this->load->view('partials/header', ['pageTitle' => 'Dashboard', 'pageSlug' => 'dashboard', 'streamUrl' => $this->stream_url()]);

        $data = $this->Detection_model->stats();
        $data['latest'] = $this->Detection_model->latest();
        $data['recent'] = $this->Detection_model->recent(10);
        $data['settings'] = $this->Setting_model->all();
        /* The dashboard alerts on the device status even when the live view is
         * closed, so it needs the same status endpoint the live page polls. */
        $data['statusUrl'] = $this->status_url();

        $this->load->view('dashboard/index', $data);
        $this->load->view('partials/footer');
    }
}
