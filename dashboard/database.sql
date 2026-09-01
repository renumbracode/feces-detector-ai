-- Pigpen Feces Detector - database schema
-- Hybrid: ESP32-S3 (on-device FOMO) + server YOLOv8 verification
-- Import this into phpMyAdmin (or: mysql -u root < database.sql)

CREATE DATABASE IF NOT EXISTS feces_detector CHARACTER SET utf8mb4;
USE feces_detector;

CREATE TABLE IF NOT EXISTS detections (
    id INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    event_time DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    confidence DECIMAL(5,4) NOT NULL DEFAULT 0,        -- on-device FOMO score
    triggered TINYINT(1) NOT NULL DEFAULT 0,             -- local pump triggered?
    spray_duration INT UNSIGNED NOT NULL DEFAULT 0,
    source_ip VARCHAR(45) NULL,
    model_version VARCHAR(20) NULL DEFAULT 'fomo:v1',   -- which model reported
    notes VARCHAR(255) NULL,
    -- YOLOv8 verification columns (filled by the Python verify server)
    yolo_conf DECIMAL(5,4) NULL,                        -- YOLOv8 max confidence
    verified TINYINT(1) NOT NULL DEFAULT 0,             -- 1 = cross-checked by YOLOv8
    KEY idx_event_time (event_time),
    KEY idx_triggered (triggered),
    KEY idx_verified (verified)
);

CREATE TABLE IF NOT EXISTS settings (
    `key` VARCHAR(50) PRIMARY KEY,
    `value` VARCHAR(255) NOT NULL DEFAULT ''
);

INSERT INTO settings (`key`, `value`) VALUES
    ('esp32_ip', '192.168.1.100'),          -- ESP32-S3 address (was the old Pi)
    ('esp_port', '80'),
    ('stream_url', ''),
    ('verify_url', 'http://localhost:8000/verify'),  -- Python YOLOv8 server
    ('spray_duration_ms', '5000'),
    ('cooldown_ms', '300000'),
    ('threshold', '0.60')
ON DUPLICATE KEY UPDATE `key` = `key`;
