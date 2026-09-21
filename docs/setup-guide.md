# Setup guide

End-to-end setup for the Pigpen Feces Detector — **hybrid ESP32-S3 + server
YOLOv8**. There is **no Raspberry Pi**.

Components:

1. **ESP32-S3 + OV3660 edge device** (ESP-IDF) — on-device FOMO inference + auto-spray.
2. **Python YOLOv8 verification server** (FastAPI + ultralytics).
3. **CodeIgniter 3 + MySQL dashboard** (XAMPP) — monitoring + manual override.

## 1. Web dashboard (XAMPP + CodeIgniter 3)

The dashboard lives at `C:\xampp\htdocs\feces-detector-ai\dashboard`:

```
http://localhost/feces-detector-ai/dashboard/
```

Steps:

1. Start **Apache** and **MySQL** from the XAMPP control panel.
2. Import the schema:
   - phpMyAdmin -> Import -> `dashboard/database.sql`, or
   - `C:\xampp\mysql\bin\mysql.exe -u root < dashboard\database.sql`
3. **Add the CodeIgniter 3 `system/` folder** — download CodeIgniter 3 from
   https://codeigniter.com/download and extract `system/` into `dashboard/`
   (see `dashboard/README.md`).
4. Check `dashboard/application/config/database.php` matches your MySQL
   user (XAMPP default: `root`, empty password).
5. Open the dashboard — you should see zero detections.

## 2. Python YOLOv8 verification server

Runs unmodified YOLOv8 on the server (feasible there, not on the MCU).

```bash
cd server/yolo_verify
pip install -r requirements.txt
# set the model path (optional):
#   export YOLO_MODEL=/abs/path/to/training/runs/detect/train/weights/best.pt
uvicorn app:app --host 0.0.0.0 --port 8000
```

Verify: `curl http://localhost:8000/health` returns model info.

## 3. ESP32-S3 firmware

### Hardware

- ESP32-S3 board with **OV3660** camera (Wi-Fi + BT, PSRAM)
- Relay module wired to `APP_PIN_RELAY` (default GPIO 4) for water pump
- Optional status LED on GPIO 2

### Build & flash

Requires ESP-IDF v5.x. From `firmware/esp32s3_fomo`:

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

See `firmware/esp32s3_fomo/README.md` for the full workflow, including how to
insert your trained FOMO model.

> **Switching Wi-Fi / server URLs at runtime (no reflash):** the ESP serves
> `GET /setup` — open `http://<esp-ip>/setup` in a browser, change SSID,
> password, verify URL, or dashboard URL, and the board saves them to NVS and
> reboots into the new network. Find the board's new IP in the router's
> connected-device list.

## 4. Train the models

### On-device FOMO (ESP32-S3)

```bash
cd training/fomo
python prepare_dataset.py --target-size 96
# then follow training/fomo/README.md: upload to Edge Impulse, train FOMO,
# export the ESP32 C++ library into firmware/.../components/inference/edge_impulse/
```

### Server YOLOv8

```bash
cd training
python train.py --model yolov8n --epochs 100
python evaluate.py --model runs/detect/train/weights/best.pt   # target mAP50 >= 80%
```

## 5. End-to-end test

1. Set the device (`esp32_ip` / `esp32_port`) and `verify_url` in the dashboard **Control** page.
2. Open **Live** and confirm the ESP32 MJPEG stream appears.
3. Click **Spray now** — the pump should run for the chosen duration (manual override).
4. Place a test object where feces would be; verify:
   - Detection is logged in **History** (with a YOLOv8-verified badge when the server agrees),
   - Pump triggers automatically on-device (respecting cooldown),
   - Confidence appears in the **Dashboard** stats.

## 6. Tuning

- **False positives** (sprays on mud/shadow): raise `threshold` on the device
  (e.g. 0.60 → 0.75) or add those images to the dataset and retrain.
- **False negatives**: lower threshold, improve lighting, add more training images.
- **Cooldown**: device NVS `cooldown_ms` (default 300000 = 5 min).
- **FPS**: FOMO runs fastest at 96x96 grayscale; use 160x160 for accuracy.

## Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| No stream | Verify camera GPIO pins in `components/camera/camera_service.c`, check ESP32 is flashed |
| Dashboard "Device unreachable" | Wrong `esp32_ip`/`esp32_port`, or ESP32 + PC on different LAN |
| Ping/`/status` time out but ESP is online | **Router wireless/client (L2) isolation** blocks station↔station. Fix: connect the PC to the router via **Ethernet**, or disable "AP/client isolation" in the router admin at `http://192.168.1.1` (Wireless → Advanced/Professional → Isolate). The ESP can still POST to the server; only PC→ESP pull is blocked |
| Need to change Wi-Fi/URLs | Open `http://<esp-ip>/setup` (no reflash), save, board reboots into the new network |
| Verification empty | Python verify server not running; check `verify_url` in dashboard settings |
| CI3 `system folder path` error | Drop the CodeIgniter 3 `system/` dir into `dashboard/` |
| Database "table not found" | Run `dashboard/database.sql` import |
| Sprays nonstop | Threshold too low; raise it on the device |
| Doesn't build | Install ESP-IDF v5.x, run `idf.py set-target esp32s3` |
