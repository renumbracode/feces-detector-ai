# Execution checklist — Pigpen Feces Detector

Tick items off as they complete. Workflows for each step are in `docs/setup-guide.md` and `docs/panel-demo-script.md`.

**This is the hybrid ESP32-S3 + server YOLOv8 system — there is NO Raspberry Pi.**

## Phase 1 — Dataset

- [ ] Captured ~300-800 images, ~50% with feces, ~50% background
- [ ] Images labeled in YOLO format, 2 classes (`feces`, `pig`) using LabelImg or Roboflow
- [ ] Dataset split: 80% train, 20% val
- [ ] Labels placed in `dataset/labels/train/` and `dataset/labels/val/`
- [ ] Colab zip rebuilt as 2-class: `python training/pack_colab_zip.py`
- [ ] Dataset packed for FOMO: `python training/fomo/prepare_dataset.py --format yolo-txt`

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
- [x] Camera init OK (OV3660), `/stream` shows MJPEG (~7 fps confirmed)
- [x] `/status` returns JSON (heap, spray, confidence, model)
- [ ] Relay wired: `APP_PIN_RELAY` (GPIO14) -> relay -> water pump; set
      `relay_active_low` to match the module (see docs/wiring-guide.md)
- [ ] Pump on its **own supply**, ground shared only, 1N4007 flyback diode fitted
      (cathode to +). Sharing the board's 5 V rail caused a brownout in testing.
- [ ] Pump on `NO`, not `NC`, and confirmed **off while the board is idle**
- [ ] Active buzzer on GPIO21 and LED on GPIO1 — each load on its **own** pin.
      Two GPIO outputs on one pad fight each other, so the buzzer must not share
      GPIO14: the relay then never latches cleanly while the log still says
      `Spray ON`. Driven by a steady DC level, so this suits an **active** buzzer
      only; a passive one would need a tone (LEDC).
- [x] On-device auto-spray triggers on detection (respects cooldown;
      E2E row id 7 "auto spray" + 8/9 "cooldown detected")

## Phase 4 — Server YOLOv8 verification

- [ ] Python env: `pip install -r server/yolo_verify/requirements.txt`
- [ ] Server model trained: `python training/train.py`
- [ ] Evaluation passed: `python training/evaluate.py` mAP50 >= 80%
- [x] Verify server runs: `uvicorn app:app --port 8000` (PID 14360, file logs)
- [x] `GET /health` returns model info
- [x] ESP32 uploads detected frames to `POST /verify` (200s in uvicorn log)

## Phase 5 — Dashboard + field test

- [ ] CodeIgniter 3 `system/` added to `dashboard/`
- [ ] Dashboard imported `database.sql` and is accessible
- [ ] Control settings: device IP/port + verify URL set correctly
- [x] Live page shows ESP32 stream (CORS + stall fixes; `src=http://192.168.1.39/stream`)
- [x] Manual spray works (Spray now button -> `/spray?duration=N`)
- [ ] Auto-detection works: feces -> on-device spray -> history logs it
      (`/status` reports `"model":"fomo:2class"`. **Verified** for both paths:
      FOMO `Spray ON` at `score=0.164`, and server-gated at `YOLOv8 conf=0.367
      (>= 0.28)` writing a `yolo server spray` row. This model's FOMO scores
      average ~0.25 (n=44, range 0.11-0.72), so the dial is set to 0.20;
      tune with `POST /config threshold=`)
- [ ] Server-gated spray reliable end to end — depends on the verify leg staying
      reachable. Check `verifyOk` / `verifyFailures` on `/status`; a climbing
      `verifyFailures` with `verifyOk:false` is a network problem, not a model one
- [ ] YOLOv8 verified badge appears on logged detections
- [ ] Threshold / cooldown / spray duration tuned
- [ ] AP set to 2.4 GHz / **20 MHz** channel width (see docs/wiring-guide.md —
      a 40 MHz channel gave 3-53% loss at good RSSI)
- [ ] Laptop/servers on **static IPs** (board is pinned to `192.168.1.39`)
- [ ] Detection metrics recorded in `training/notes/`

## Phase 6 — Panel

- [ ] `training/notes/EVIDENCE_TEMPLATE.md` completed
- [ ] Panel demo script rehearsed
- [ ] Dashboard runs on the demo machine
