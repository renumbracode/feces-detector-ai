# Execution checklist — Pigpen Feces Detector

Tick items off as they complete. Workflows for each step are in `docs/setup-guide.md` and `docs/panel-demo-script.md`.

**This is the hybrid ESP32-S3 + server YOLOv8 system — there is NO Raspberry Pi.**

## Phase 1 — Dataset

- [ ] Captured ~300-800 images, ~50% with feces, ~50% background
- [ ] Images labeled in YOLO format (class: `feces`) using LabelImg or Roboflow
- [ ] Dataset split: 80% train, 20% val
- [ ] Labels placed in `dataset/labels/train/` and `dataset/labels/val/`
- [ ] Dataset converted for FOMO: `python training/fomo/prepare_dataset.py --target-size 96`

## Phase 2 — On-device model (Edge Impulse FOMO)

- [ ] Dataset uploaded to Edge Impulse FOMO project
- [ ] Trained a small FOMO-MobileNet model (int8)
- [ ] Exported ESP32 C++ library and unzipped into
      `firmware/esp32s3_fomo/components/inference/edge_impulse/`
- [ ] `edge_impulse_invoke.c` wired to run the real model
- [ ] Validation accuracy acceptable (server YOLOv8 covers the rest)

## Phase 3 — ESP32-S3 firmware

- [ ] ESP-IDF v5.x installed; `idf.py set-target esp32s3` succeeds
- [ ] Firmware builds: `idf.py build`
- [ ] Flashed: `idf.py -p COMx flash monitor`
- [ ] Camera init OK (OV3660), `/stream` shows MJPEG
- [ ] `/status` returns JSON (heap, spray, confidence, model)
- [ ] Relay wired: `APP_PIN_RELAY` (GPIO 4) -> relay -> water pump
- [ ] On-device auto-spray triggers on detection (respects cooldown)

## Phase 4 — Server YOLOv8 verification

- [ ] Python env: `pip install -r server/yolo_verify/requirements.txt`
- [ ] Server model trained: `python training/train.py`
- [ ] Evaluation passed: `python training/evaluate.py` mAP50 >= 80%
- [ ] Verify server runs: `uvicorn app:app --port 8000`
- [ ] `GET /health` returns model info
- [ ] ESP32 uploads detected frames to `POST /verify`

## Phase 5 — Dashboard + field test

- [ ] CodeIgniter 3 `system/` added to `dashboard/`
- [ ] Dashboard imported `database.sql` and is accessible
- [ ] Control settings: device IP/port + verify URL set correctly
- [ ] Live page shows ESP32 stream
- [ ] Manual spray works (Spray now button)
- [ ] Auto-detection works: feces -> on-device spray -> history logs it
- [ ] YOLOv8 verified badge appears on logged detections
- [ ] Threshold / cooldown / spray duration tuned
- [ ] Detection metrics recorded in `training/notes/`

## Phase 6 — Panel

- [ ] `training/notes/EVIDENCE_TEMPLATE.md` completed
- [ ] Panel demo script rehearsed
- [ ] Dashboard runs on the demo machine
