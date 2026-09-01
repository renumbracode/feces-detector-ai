<?php
defined('BASEPATH') OR exit('No direct script access allowed');

class Setting_model extends CI_Model
{
    public function get($key, $default = '')
    {
        $this->db->where('`key`', $key);
        $row = $this->db->get('settings')->row_array();
        return $row ? $row['value'] : $default;
    }

    public function all()
    {
        return $this->db->get('settings')->result_array();
    }

    public function set($key, $value)
    {
        $this->db->where('`key`', $key);
        if ($this->db->count_all_results('settings') > 0) {
            $this->db->where('`key`', $key)->update('settings', ['value' => $value]);
        } else {
            $this->db->insert('settings', ['key' => $key, 'value' => $value]);
        }
    }
}
