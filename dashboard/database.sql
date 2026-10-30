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
    -- Multi-instance detection: how many feces spots were in the frame that
    -- produced this row (1 = the old single-spot behaviour).
    instance_count INT UNSIGNED NOT NULL DEFAULT 1,
    -- Primary feces box: top-left normalized 0..1, the same convention the
    -- device /status JSON and the live-view overlay grid use. NULL rows are
    -- from firmware that predates coordinate reporting.
    x DECIMAL(5,4) NULL,
    y DECIMAL(5,4) NULL,
    w DECIMAL(5,4) NULL,
    h DECIMAL(5,4) NULL,
    -- The same box in CENTIMETRES on the pen, when the pen has been
    -- calibrated (see the pen_homography setting). x_cm/y_cm place the
    -- box's top-left corner, the same corner the normalized columns
    -- describe; w_cm/h_cm are the box's extent along the pen's two axes.
    --
    -- These are a SNAPSHOT taken when the row was written, so re-calibrating
    -- the pen later never rewrites what a past detection meant. NULL means
    -- "not known": the pen was not calibrated, or the picture's shape had
    -- changed. A made-up number would be worse than none.
    x_cm DECIMAL(8,2) NULL,
    y_cm DECIMAL(8,2) NULL,
    w_cm DECIMAL(8,2) NULL,
    h_cm DECIMAL(8,2) NULL,
    -- YOLOv8 verification columns (filled by the Python verify server)
    yolo_conf DECIMAL(5,4) NULL,                        -- YOLOv8 max confidence
    verified TINYINT(1) NOT NULL DEFAULT 0,             -- 1 = cross-checked by YOLOv8
    KEY idx_event_time (event_time),
    KEY idx_triggered (triggered),
    KEY idx_verified (verified)
);

-- Multi-camera registry: one row per ESP32-S3 + camera unit. The number of
-- cameras a pen needs depends on the pen's size and layout (coverage angle,
-- blind spots) -- see docs/scaling-and-multi-camera.md. The firmware is
-- one-camera-per-device; adding a camera means adding a device row here.
CREATE TABLE IF NOT EXISTS cameras (
    id INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    label VARCHAR(32) NOT NULL,                         -- e.g. "cam-01"
    esp32_ip VARCHAR(45) NOT NULL DEFAULT '',           -- device address
    pen_label VARCHAR(32) NOT NULL DEFAULT '',          -- which pen it watches
    stream_url VARCHAR(255) NOT NULL DEFAULT '',        -- MJPEG URL override
    location_notes VARCHAR(255) NOT NULL DEFAULT '',    -- mount position etc.
    active TINYINT(1) NOT NULL DEFAULT 1,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY uk_label (label)
);

-- Dispatches to an external actuator board ("main ESP32"): one row per
-- detection the camera board published for someone else to aim at.
--
-- The camera board reports the outcome of the last publish in its /status
-- document, and this table is that stream written down. The dashboard polls
-- /status anyway, so the record costs one INSERT per NEW detection id --
-- the unique key makes the repeated polls harmless.
--
-- outcome is one of:
--   accepted   -- the actuator board started a cleaning cycle
--   refused    -- it understood the message and declined it (see `reason`)
--   unreachable-- the message never arrived (the board could not be reached)
CREATE TABLE IF NOT EXISTS target_dispatches (
    id INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    detection_id INT UNSIGNED NOT NULL,                  -- the board's counter
    dispatched_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    outcome VARCHAR(16) NOT NULL DEFAULT 'unknown',
    reason VARCHAR(32) NOT NULL DEFAULT '',               -- e.g. low_water, busy
    pan DECIMAL(6,2) NULL,                                -- servo angle, degrees
    tilt DECIMAL(6,2) NULL,
    source VARCHAR(8) NOT NULL DEFAULT '',                -- fomo | yolo
    -- What the camera board saw, for the record: the boxes it published.
    boxes_json TEXT NULL,
    UNIQUE KEY uk_detection (detection_id),
    KEY idx_dispatched (dispatched_at)
);

-- Existing deployments: run these once instead of re-importing database.sql.
-- ALTER TABLE detections ADD COLUMN instance_count INT UNSIGNED NOT NULL DEFAULT 1 AFTER notes;
-- ALTER TABLE detections ADD COLUMN x DECIMAL(5,4) NULL AFTER instance_count,
--     ADD COLUMN y DECIMAL(5,4) NULL AFTER x,
--     ADD COLUMN w DECIMAL(5,4) NULL AFTER y,
--     ADD COLUMN h DECIMAL(5,4) NULL AFTER w;
-- ALTER TABLE detections ADD COLUMN x_cm DECIMAL(8,2) NULL AFTER h,
--     ADD COLUMN y_cm DECIMAL(8,2) NULL AFTER x_cm,
--     ADD COLUMN w_cm DECIMAL(8,2) NULL AFTER y_cm,
--     ADD COLUMN h_cm DECIMAL(8,2) NULL AFTER w_cm;

CREATE TABLE IF NOT EXISTS settings (
    `key` VARCHAR(50) PRIMARY KEY,
    `value` VARCHAR(255) NOT NULL DEFAULT ''
);

INSERT INTO settings (`key`, `value`) VALUES
    ('esp32_ip', '192.168.1.39'),          -- ESP32-S3 address (was the old Pi)
    ('esp_port', '80'),
    ('stream_url', ''),
    ('verify_url', 'http://localhost:8000/verify'),  -- Python YOLOv8 server
    ('spray_duration_ms', '5000'),
    ('cooldown_ms', '300000'),
    ('threshold', '0.60'),
    ('target_url', ''),                    -- main ESP32 POST /target (empty = off)
    ('target_enabled', '0')                -- mirror of the device's own switch
    -- The pen, which is the calibration target. The camera looks straight
    -- down at it, so the picture IS the pen: every centimetre in the
    -- picture is the same size as every other one, and the pen's size is
    -- all that is needed to turn a position into centimetres.
    --   pen_width_cm  -- across the pen, cm
    --   pen_length_cm -- along the pen, cm
    --   pen_flip_y    -- 1 when the camera is at the TOP of the picture
    --   pen_homography -- a fit to the picture, for a camera that is not
    --                   square-on. Empty means the plain model applies.
    --   pen_calib_aspect -- width/height of the picture a fit was made
    --                      against, so a differently shaped picture is
    --                      refused its centimetres instead of mis-scaled
    --   grid_step_cm  -- how far apart the centimetre gridlines are
    ('pen_width_cm', '20'),
    ('pen_length_cm', '30'),
    ('pen_flip_y', '0'),
    ('pen_homography', ''),
    ('pen_calib_aspect', ''),
    ('grid_step_cm', '5')
ON DUPLICATE KEY UPDATE `key` = `key`;

-- Default single-camera deployment (Pen A, overhead mount). Extra cameras
-- are INSERTed here as the pen layout requires.
INSERT INTO cameras (label, esp32_ip, pen_label, stream_url, location_notes) VALUES
    ('cam-01', '192.168.1.39', 'Pen A', '',
     'Default overhead mount; covers the pen area visible from the final pen angle')
ON DUPLICATE KEY UPDATE `label` = `label`;
