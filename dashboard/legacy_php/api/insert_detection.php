<?php
require __DIR__ . '/../config.php';

header('Content-Type: application/json');

$confidence = isset($_GET['confidence']) ? round((float)$_GET['confidence'], 4) : 0.0;
$triggered  = isset($_GET['triggered']) && (int)$_GET['triggered'] === 1 ? 1 : 0;
$sprayMs    = isset($_GET['spray_ms']) ? max(0, (int)$_GET['spray_ms']) : 0;
$source     = isset($_GET['esp']) ? substr(preg_replace('/[^0-9a-zA-Z.:_\-]/', '', $_GET['esp']), 0, 45) : 'yolov8';
$model      = isset($_GET['model']) ? substr(preg_replace('/[^0-9a-zA-Z0-9._\-]/', '', $_GET['model']), 0, 20) : 'yolov8';
$notes      = isset($_GET['notes']) ? trim(substr(strip_tags((string)$_GET['notes']), 0, 255)) : '';
if ($notes === '') {
    $notes = null;
}

if ($confidence < 0 || $confidence > 1) {
    http_response_code(400);
    echo json_encode(['ok' => false, 'error' => 'confidence out of range']);
    exit;
}

try {
    $st = db()->prepare('INSERT INTO detections (confidence, triggered, spray_duration, source_ip, model_version, notes)
                         VALUES (?, ?, ?, ?, ?, ?)');
    $st->execute([$confidence, $triggered, $sprayMs, $source, $model, $notes]);
    echo json_encode(['ok' => true, 'id' => (int)db()->lastInsertId()]);
} catch (Throwable $e) {
    http_response_code(500);
    echo json_encode(['ok' => false, 'error' => $e->getMessage()]);
}
