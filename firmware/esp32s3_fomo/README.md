# ESP32-S3 Feces Detector firmware (ESP-IDF)

ESP-IDF application for an **ESP32-S3 + OV3660** camera board. It performs
**on-device** object detection with a lightweight **Edge Impulse FOMO** model
and controls a relay (water pump) locally. The Raspberry Pi is **removed** —
there is no Pi in this system.

Heavier **YOLOv8** verification runs on the Python server, fed frames uploaded
by this device (see `server/yolo_verify/`).

## Architecture

```
┌────────────────────────────────────────────┐
│ ESP32-S3 (this firmware)                   │
│  • OV3660 capture                          │
│  • Edge Impulse FOMO inference (on-device) │
│  • GPIO → relay → water pump (auto-spray)  │
│  • HTTP: /stream /status /spray /config    │
│  • reports + uploads frames to server      │
└──────────┬─────────────────────────────────┘
           │ HTTP
           ├── CodeIgniter dashboard (logging)
           └── Python YOLOv8 verifier (/verify)
```

Components:

| Component | Purpose |
|---|---|
| `components/app_config` | NVS-persisted settings (WiFi, URLs, threshold, cooldown, spray) |
| `components/wifi_service` | Connect to the configured Wi-Fi AP |
| `components/camera` | OV3660 init + frame access via `esp32-camera` |
| `components/inference` | FOMO inference wrapper + Edge Impulse integration point |
| `components/spray` | Relay/water-pump control (threshold, cooldown, duration) |
| `components/detection_service` | Continuous loop: infer → spray → report |
| `components/http_server` | Dashboard-facing endpoints |
| `main` | Boot orchestration |

## Getting a trained model running

1. **Train a FOMO model** (see `training/fomo/README.md`).
2. In Edge Impulse export the **ESP32 C++/Arduino library**.
3. Unzip the export under `components/inference/edge_impulse/`.
4. Wire the exported SDK into `components/inference/edge_impulse_invoke.c`
   (replace the stub `prv_run_stub` with a `run_classifier()` call — see the
   instructions at the top of that file).
5. Set the model input geometry in `edge_impulse_invoke.c`
   (`EI_INPUT_W` / `EI_INPUT_H`) to match your export.

Until a real model is present, a deterministic stub is compiled in so the
firmware still builds, boots, streams `/stream` and serves `/status`.

## Build & flash

Requires ESP-IDF **v5.x**. From this directory:

```bash
export IDF_PATH=/path/to/esp-idf
. $IDF_PATH/export.sh

idf.py set-target esp32s3
idf.py menuconfig        # set PSRAM + flash size if your board differs
idf.py build
idf.py -p COMx flash monitor
```

## Configuration

Writable on first boot via `components/app_config` (NVS). Defaults:

| Key | Default |
|---|---|
| `wifi_ssid` | `REDACTED_SSID` |
| `wifi_pass` | _(live network; change via `/setup`)_ |
| `verify_url` | `http://192.168.1.3:8000/verify` (Python YOLOv8 server) |
| `dash_url` | `http://192.168.1.3/feces-detector-ai/dashboard/.../insert_detection.php` |
| `threshold` | `0.60` (min FOMO score to auto-spray) |
| `cooldown_ms` | `300000` (5 min between auto sprays) |
| `spray_ms` | `5000` (pump-on duration) |

## HTTP endpoints (dashboard-compatible)

| Endpoint | Description |
|---|---|
| `GET /stream` | MJPEG with FOMO overlay |
| `GET /status` | JSON: heap, spray, confidence, uptime, model, etc. |
| `GET /spray?duration=` | Manual override (bypasses cooldown) |
| `GET /config` | Current device configuration JSON |
| `POST /config` | Save Wi-Fi/server/tuning settings to NVS and reboot (see `/setup`) |
| `GET /setup` | Browser form to switch Wi-Fi / verify / dashboard URLs without reflashing |

## PIN mapping note

Camera GPIO pins are defined in `components/camera/camera_service.c` and are
typical for ESP32-S3 CAM dev boards. Verify them against your board's
schematic, and confirm the relay/`APP_PIN_RELAY` (default GPIO 4) and LED
(GPIO 2) pins.
