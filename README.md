# Pigpen Feces Detector AI (Capstone)

Automated pigpen cleaning with a **hybrid edge + server** architecture and **no Raspberry Pi**:

- An **ESP32-S3 + OV3660** camera runs a lightweight **Edge Impulse FOMO**
  model **on-device**, so fast, low-latency auto-cleaning works fully offline.
- On detection it drives a relay to run a **water pump** that sprays the pen.
- Detected frames are uploaded to a **Python YOLOv8 verification server**
  that re-checks them with a full detection model (unmodified YOLOv8 cannot
  feasibly run on the ESP32-S3).
- A **CodeIgniter 3 + MySQL** dashboard (XAMPP) logs events, shows the live
  stream, and provides manual override.

## Objectives

1. Automate detection of pig feces — on-device (FOMO) + server (YOLOv8)
2. Automate the cleaning process upon detection of pig feces
3. Achieve at least 80% pig feces detection accuracy

## System overview

```
ESP32-S3 + OV3660 (ESP-IDF firmware)
  - captures frame
  - Edge Impulse FOMO INT8 inference (on-device, lightweight)
  - detection above threshold + cooldown -> GPIO relay -> water pump -> spray
  - logs event to dashboard API
  - uploads detected frames to YOLOv8 verify server
  - serves /stream, /status, /spray
           |
           |  Wi-Fi (same LAN)
           v
XAMPP server:
  - Python YOLOv8 verify API  (server/yolo_verify)  -> POST /verify
  - CodeIgniter 3 + MySQL dashboard (detection log, stats, live view, manual spray)
```

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
