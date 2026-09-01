<?php
defined('BASEPATH') OR exit('No direct script access allowed');

class Detection_model extends CI_Model
{
    public function stats()
    {
        $total = (int) $this->db->count_all('detections');
        $this->db->where('triggered', 1);
        $triggered = (int) $this->db->count_all_results('detections');
        $this->db->where('DATE(event_time)', 'CURDATE()', false);
        $today = (int) $this->db->count_all_results('detections');
        $avg = $this->db->query('SELECT AVG(confidence) AS a FROM detections')->row()->a;

        return [
            'total'     => $total,
            'triggered' => $triggered,
            'today'     => $today,
            'avgConf'   => round((float) $avg, 4),
        ];
    }

    public function latest()
    {
        return $this->db->order_by('id', 'DESC')->limit(1)->get('detections')->row_array();
    }

    public function recent($limit = 10)
    {
        return $this->db->order_by('id', 'DESC')->limit($limit)->get('detections')->result_array();
    }

    public function total_rows($filter = -1)
    {
        if ($filter === 0 || $filter === 1) {
            $this->db->where('triggered', $filter);
        }
        return $this->db->count_all_results('detections');
    }

    public function page($filter, $per_page, $offset)
    {
        if ($filter === 0 || $filter === 1) {
            $this->db->where('triggered', $filter);
        }
        return $this->db->order_by('id', 'DESC')
                        ->limit($per_page, $offset)
                        ->get('detections')
                        ->result_array();
    }

    /**
     * Insert a detection (called by the ESP32-S3 dashboard hook and by the
     * verify server when it writes a cross-checked record).
     */
    public function insert($data)
    {
        $this->db->insert('detections', $data);
        return $this->db->insert_id();
    }

    /** Mark a detection as verified by YOLOv8 with its confidence. */
    public function mark_verified($id, $yolo_conf)
    {
        $this->db->where('id', $id)->update('detections', [
            'yolo_conf' => $yolo_conf,
            'verified'  => 1,
        ]);
    }
}
