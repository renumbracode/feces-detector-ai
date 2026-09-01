<?php
defined('BASEPATH') OR exit('No direct script access allowed');

class History extends MY_Controller
{
    public function index()
    {
        $filter = isset($_GET['triggered']) ? (int) $_GET['triggered'] : -1;
        $perPage = 25;

        $totalRows = $this->Detection_model->total_rows($filter);
        $pages = max(1, (int) ceil($totalRows / $perPage));
        $page = isset($_GET['page']) ? max(1, min($pages, (int) $_GET['page'])) : 1;
        $offset = ($page - 1) * $perPage;

        $this->load->view('partials/header', ['pageTitle' => 'Detection History', 'pageSlug' => 'history', 'streamUrl' => $this->stream_url()]);

        $this->load->view('history/index', [
            'rows'      => $this->Detection_model->page($filter, $perPage, $offset),
            'totalRows' => $totalRows,
            'filter'    => $filter,
            'pages'     => $pages,
            'page'      => $page,
        ]);
        $this->load->view('partials/footer');
    }
}
