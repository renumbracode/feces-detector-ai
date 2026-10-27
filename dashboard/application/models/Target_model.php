<?php
defined('BASEPATH') OR exit('No direct script access allowed');

/**
 * Dispatches to an external actuator board ("main ESP32").
 *
 * The camera board publishes detected coordinates for another board to aim at,
 * and reports the outcome in its /status document. This model writes that
 * stream down so there is a cleaning log on this side too.
 *
 * The camera board's detection id is the key. The dashboard polls /status
 * from several pages at once, so the same dispatch is seen many times; the
 * unique key on detection_id makes the repeats harmless, and the outcome is
 * updated in place so a dispatch first seen as "unreachable" and later seen
 * as "accepted" ends up recorded correctly.
 */
class Target_model extends CI_Model
{
    /** Record (or update) one dispatch. */
    public function record($detection_id, $outcome, $reason = '',
                           $pan = null, $tilt = null, $source = '',
                           $boxes_json = null)
    {
        if ((int) $detection_id <= 0) {
            return false;
        }
        $row = [
            'detection_id' => (int) $detection_id,
            'outcome'      => substr((string) $outcome, 0, 16),
            'reason'       => substr((string) $reason, 0, 32),
            'pan'          => ($pan === null) ? null : (float) $pan,
            'tilt'         => ($tilt === null) ? null : (float) $tilt,
            'source'       => substr((string) $source, 0, 8),
            'boxes_json'   => $boxes_json,
        ];
        /* ON DUPLICATE KEY so the outcome can be corrected as it becomes
         * final: a dispatch first seen while the board was unreachable is
         * later seen as accepted, and the accepted answer must win. */
        $sql = 'INSERT INTO target_dispatches '
             . '(detection_id, dispatched_at, outcome, reason, pan, tilt, source, boxes_json) '
             . 'VALUES (?, NOW(), ?, ?, ?, ?, ?, ?) '
             . 'ON DUPLICATE KEY UPDATE outcome = VALUES(outcome), '
             . 'reason = VALUES(reason), pan = VALUES(pan), tilt = VALUES(tilt), '
             . 'source = VALUES(source), boxes_json = VALUES(boxes_json)';
        return $this->db->query($sql, [
            $row['detection_id'], $row['outcome'], $row['reason'],
            $row['pan'], $row['tilt'], $row['source'], $row['boxes_json'],
        ]);
    }

    /** The most recent dispatches, newest first. */
    public function recent($limit = 20)
    {
        $this->db->order_by('id', 'DESC')->limit($limit);
        return $this->db->get('target_dispatches')->result_array();
    }

    /** Counts per outcome, for a summary line. */
    public function summary()
    {
        $rows = $this->db->select('outcome, COUNT(*) AS n', false)
                        ->group_by('outcome')->get('target_dispatches')
                        ->result_array();
        $out = [];
        foreach ($rows as $r) {
            $out[$r['outcome']] = (int) $r['n'];
        }
        return $out;
    }
}
