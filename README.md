# Pigpen Feces Detector AI (Capstone)

Automated pigpen cleaning with a **hybrid edge + server** architecture and **no Raspberry Pi**:

- An **ESP32-S3 + OV3660** camera runs a lightweight **Edge Impulse FOMO**
  model **on-device**, so auto-cleaning works offline. A second, *additional*
  trigger uses the server-side YOLOv8 verdict, which is better at low confidence but
  needs the PC reachable; the on-device path is what keeps working when it isn't.
- On detection it drives a relay to run a **water pump** that sprays the pen.
- Detected frames are uploaded to a **Python YOLOv8 verification server**
  that re-checks them with a full detection model (unmodified YOLOv8 cannot
  feasibly run on the ESP32-S3).
- **Multi-instance detection**: one frame can contain several feces spots —
  every spot above the confidence floor is counted and drawn (live view shows
  one box per instance; history rows show the count). Spray fires once per
  frame regardless of instance count. See
  [docs/scaling-and-multi-camera.md](docs/scaling-and-multi-camera.md).
- A **CodeIgniter 3 + MySQL** dashboard (XAMPP) logs events, shows the live
  stream, and provides manual override.
- **Scalable camera count**: one ESP32-S3 per camera; how many cameras a pen
  needs depends on its size and layout (coverage, blind spots). The dashboard
  has a `cameras` registry for multi-pen deployments.

## Objectives

1. Automate detection of pig feces — on-device (FOMO) + server (YOLOv8)
2. Automate the cleaning process upon detection of pig feces
3. Achieve at least 80% pig feces detection accuracy

## System overview

```
ESP32-S3 + OV3660 (ESP-IDF firmware)
  - captures frame
  - Edge Impulse FOMO INT8 inference (on-device, lightweight)
  - detection above threshold -> GPIO relay -> water pump -> spray
- detection above threshold, cooldown -> buzzer + LED (pump held back)
  - logs event to dashboard API
  - uploads detected frames to YOLOv8 verify server
  - serves /stream, /status, /spray
           |
           |  Wi-Fi (same LAN, or a phone hotspot)
           v
XAMPP server:
  - Python YOLOv8 verify API  (server/yolo_verify)  -> POST /verify
  - CodeIgniter 3 + MySQL dashboard (detection log, stats, live view, manual spray)
```

No router is required — a phone hotspot works as the shared network. See
[setup-guide.md §7](docs/setup-guide.md#7-mobile-hotspot-no-router).

## Folder layout

| Path | Purpose |
|------|---------|
| `dataset/` | Training images + YOLO labels (train/val split) |
| `training/` | YOLOv8 training (server) + FOMO pipeline (`training/fomo`) |
| `firmware/esp32s3_fomo/` | ESP-IDF firmware for the ESP32-S3 edge device |
| `server/yolo_verify/` | Python FastAPI server running YOLOv8 verification |
| `dashboard/` | CodeIgniter 3 + MySQL web app |
| `docs/` | Setup guide, mock-pen guide, panel demo script, checklist |

## Quick start

1. **Collect & label dataset** — then label with [LabelImg](https://github.com/heartexlabs/labelImg) or [Roboflow](https://roboflow.com).
2. **Train on-device model** — `cd training/fomo` → Edge Impulse FOMO (see `training/fomo/README.md`).
3. **Train server model** — `cd training && python train.py --model yolov8n --epochs 100`.
4. **Flash ESP32-S3** — `cd firmware/esp32s3_fomo && idf.py build flash` (see its README).
5. **Run verify server** — `cd server/yolo_verify && uvicorn app:app --port 8000`.
6. **Dashboard** — copy `dashboard/` to XAMPP htdocs, drop in CI3 `system/`, import `database.sql` (see `dashboard/README.md`).
7. **Test** — see `docs/panel-demo-script.md`.

Full instructions in `docs/setup-guide.md`.
