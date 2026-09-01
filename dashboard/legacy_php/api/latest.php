<?php
require __DIR__ . '/../config.php';

header('Content-Type: application/json');

$db = db();

$out = [
    'total'     => (int)$db->query('SELECT COUNT(*) FROM detections')->fetchColumn(),
    'triggered' => (int)$db->query('SELECT COUNT(*) FROM detections WHERE triggered = 1')->fetchColumn(),
    'today'     => (int)$db->query('SELECT COUNT(*) FROM detections WHERE DATE(event_time) = CURDATE()')->fetchColumn(),
    'avgConf'   => round((float)($db->query('SELECT AVG(confidence) FROM detections')->fetchColumn() ?? 0), 4),
    'latest'    => $db->query('SELECT * FROM detections ORDER BY id DESC LIMIT 1')->fetch(),
    'recent'    => $db->query('SELECT * FROM detections ORDER BY id DESC LIMIT 10')->fetchAll(),
];

echo json_encode($out);
