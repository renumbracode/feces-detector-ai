# Panel demo script

A ~10 minute live demo that shows the full system: **on-device AI detection ->
IoT trigger -> water spray -> server YOLOv8 verification -> dashboard logging**.
There is **no Raspberry Pi** — the edge device is an **ESP32-S3**.

## Before the demo (day of)

- [ ] ESP32-S3 + OV3660 mounted at the final pen angle, powered, streaming
- [ ] ESP32 connected to Wi-Fi; `/stream` and `/status` reachable from the laptop
- [ ] Python YOLOv8 verify server running (`uvicorn app:app --port 8000`)
- [ ] Water supply / pump filled and tested manually via `Spray now`
- [ ] Dashboard open on `http://localhost/feces-detector-ai/dashboard/`
- [ ] ESP32 and demo laptop on same Wi-Fi network
- [ ] Battery backup for laptop (don't run on mains if the pen area is wet)

## Script

1. **Problem** (1 min)
   - Pigpen cleanliness affects pig health; manual cleaning is labor-intensive.
   - Goal: automated detection of feces + auto water-spray.

2. **Architecture** (2 min)
   - **ESP32-S3 + OV3660** runs a lightweight **Edge Impulse FOMO** model
     **on-device** for fast, offline auto-cleaning.
   - On detection -> GPIO relay -> pump -> spray; event logged to dashboard.
   - Detected frames are uploaded to a **Python YOLOv8** server that verifies
     them with full bounding boxes (YOLOv8 can't run on the MCU).
   - Show the block diagram from `README.md`.

3. **Trained AI** (3 min)
   - **On-device**: FOMO model (96x96, int8) trained in Edge Impulse — small
     and fast enough for the ESP32-S3.
   - **Server**: YOLOv8n model, mAP50 >= 80%, precision/recall from
     `training/notes/`.
   - Emphasize the "lightweight model for the MCU" + "full YOLOv8 on server"
     split.

4. **Live demo** (3 min)
   - Open Dashboard -> **Live**: show the stream.
   - Trigger a manual spray: Dashboard -> **Control** -> Spray now (pump runs).
   - Real detection: place the prepared test object in the pen's view; wait for
     cooldown; pump fires automatically (decided on-device).
   - Open **History**: show the new row (time, FOMO confidence,
     source=esp32s3, model=FOMO, triggered=Yes) and the **YOLOv8 ✓** verified badge.

5. **Closing** (1 min)
   - Summarize results: FOMO on-device speed, YOLOv8 mAP50, detection-to-spray latency.
   - Next steps: larger dataset, multi-pen scaling, FOMO model refinement.
   - Q&A.

## Key numbers to have ready

- Dataset size (# images, # annotations)
- FOMO on-device inference time and FPS on the ESP32-S3
- YOLOv8 mAP50, precision, recall, F1 score
- Spray delay: latency from detection to pump on
- Water saved vs. fixed-interval cleaning (if measured)
