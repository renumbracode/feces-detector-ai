<?php
defined('BASEPATH') OR exit('No direct script access allowed');

/**
 * Registry of ESP32-S3 camera units. One row per camera device; how many a
 * pen needs depends on its size and layout (see
 * docs/scaling-and-multi-camera.md). Groundwork for multi-pen deployments --
 * the firmware remains one-camera-per-device.
 */
class Camera_model extends CI_Model
{
    public function all($active_only = false)
    {
        if ($active_only) {
            $this->db->where('active', 1);
        }
        return $this->db->order_by('label', 'ASC')->get('cameras')->result_array();
    }

    public function find($id)
    {
        return $this->db->where('id', (int) $id)->get('cameras')->row_array();
    }

    public function insert($data)
    {
        $this->db->insert('cameras', $data);
        return $this->db->insert_id();
    }

    public function update($id, $data)
    {
        $this->db->where('id', (int) $id)->update('cameras', $data);
    }

    public function set_active($id, $active)
    {
        $this->update($id, ['active' => $active ? 1 : 0]);
    }
}
