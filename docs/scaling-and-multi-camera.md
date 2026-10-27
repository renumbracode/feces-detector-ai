# Scaling: multi-instance detection and multi-camera pens

This document answers the post-mock panel feedback: the system must detect
**multiple feces instances in one image**, and the **number of cameras is not
fixed** — it depends on the pigpen's size and layout.

## 1. Multi-instance detection (implemented)

### What changed

Earlier, every layer of the pipeline collapsed a frame to the single
best-scoring feces box. Both models could already emit several boxes per
frame, but the firmware, verify server, database, and UI discarded all but
one. After the mock, the full count is carried end-to-end:

| Layer | Before | After |
|---|---|---|
| Edge Impulse FOMO (firmware) | best feces box only | all boxes ≥ floor, sorted by score, up to 8 (`fomo_result_t.feces_boxes`, `feces_count`) |
| YOLOv8 verify server | best box in `box_norm` | `feces_count` + `boxes_norm` (all feces boxes, up to 4) |
| Dashboard DB | no count | `detections.instance_count` (default 1) |
| `/status` + live view | one overlay box | `boxes[]` arrays; one overlay per instance, labelled `FECES ×N` |
| History / dashboard tables | no count | **Instances** column (`×N` badge when > 1) |

### Spray policy

Multiple instances in one frame trigger **one spray per frame**, not one per
spot. Rationale:

- The pump/nozzle has no per-spot aiming; spraying N times for N spots in the
  same frame just wastes water.
- The existing spray cooldown already prevents spray spam across frames.
- When more spots remain after the cooldown, the next detection frame sprays
  again — so a dirty pen with many spots still gets cleaned over successive
  frames, without flooding one frame's worth of water instantly.

### Redefense talking points

- "Why only one box before?" — the models were multi-instance capable from the
  start; the reduction to one box was a UI/pipeline choice, now lifted
  everywhere.
- "Does the count match the physical spots?" — the count is the number of
  *model detections above the confidence floor* in one frame. Near-touching
  spots can merge into one box; very small spots below the FOMO 1/8-scale heat
  map cell size may be missed on-device. The server-side YOLOv8 count is the
  higher-accuracy reference.

## 2. Cameras depend on pigpen size and layout

### Principle

**There is no fixed camera count.** The number of ESP32-S3 camera units a pen
(or barn) needs is a function of:

- **Pen floor area** — one camera covers roughly what its mounted field of
  view can resolve at feces-blob scale. FOMO needs a blob to cover at least
  one 1/8-scale heat-map cell (~8 px at the 160×160 input); the farther the
  camera, the smaller blobs appear, and the more of the floor falls below
  that limit.
- **Layout and occlusions** — feeders, drinkers, partitions, and standing pigs
  create blind spots. A long narrow pen may need one camera per section; a
  square pen may need one overhead camera plus one angled unit for the corners.
- **Coverage geometry** — the current mount is a single overhead unit at the
  "final pen angle", which is adequate for the demo mock pen. Larger pens need
  overlapping coverage so every square of floor is visible to at least one
  camera.

Rule of thumb for planning: **add one camera unit per additional covered
zone**, where a zone is any region of pen floor that (a) a single mount cannot
resolve at feces scale and (b) is not already covered by an adjacent unit's
overlap.

### How the system scales (implemented groundwork)

The architecture is already **one camera per ESP32-S3** — each device runs its
own FOMO inference, spray relay, and verify uploads. Scaling out is additive:

1. **Flash + mount another ESP32-S3 unit** for each additional zone (same
   firmware; set its Wi-Fi/server config via `/setup`).
2. **Register it in the dashboard** — Control → Cameras (backed by the
   `cameras` table: label, device IP, pen, mount notes). Each unit has a row;
   deactivating a row marks a zone as intentionally offline.
3. **Verify server** accepts an optional `X-Camera-Id` header on `/verify`, so
   per-camera attribution is already on the wire (logged; the DB row's
   `source_ip` field carries the device label).
4. **Dashboard history** rows are per-device; the **Instances** column tells
   how many spots each single frame held, per camera.

What is intentionally *not* built yet (and should be stated as future work if
asked): a single combined multi-stream live view, per-zone spray hardware
(aimed nozzles/servos), and automatic camera-to-zone mapping. The DB and API
groundwork exists; the firmware remains one-camera-per-device by design, which
is also what makes the units independently serviceable.

### Redefense talking points

- "How many cameras do we need?" — it depends on the pen size and layout; the
  system is designed so the count is a deployment decision, not a system
  limit. The demo uses one camera because one covers the mock pen.
- "Can the panel see all cameras?" — each unit's stream is reachable at its
  own `/stream`; the cameras registry tracks them. A combined multi-view is a
  dashboard enhancement, not an architecture change.
- "Does detection degrade with more cameras?" — no: each unit is an
  independent edge device with its own inference and relay; they share only
  the LAN, the verify server, and the dashboard DB.

## 3. Where the code lives

- Firmware multi-instance: `firmware/esp32s3_fomo/components/inference/include/inference_service.h`,
  `edge_impulse_invoke.cpp`, `detection_service.c`, `http_server/http_server_service.c` (`/status`)
- Verify server: `server/yolo_verify/app.py` (`feces_count`, `boxes_norm`, `X-Camera-Id`)
- DB: `dashboard/database.sql` (`detections.instance_count`, `cameras` table)
- API: `dashboard/application/controllers/Api.php` (`count` param, `/api/cameras`)
- UI: `dashboard/application/views/live/index.php`, `assets/js/main.js`,
  `views/history/index.php`, `views/control/index.php`
