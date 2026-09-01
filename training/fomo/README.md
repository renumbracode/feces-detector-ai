# Edge Impulse FOMO pipeline (ESP32-S3 on-device detection)

This sub-folder is the **on-device** half of the model pipeline. It produces a
small **FOMO (Faster Objects, More Objects)** object detector that actually
runs on the ESP32-S3 — unlike the full YOLOv8 pipeline (see `../train.py`,
`../evaluate.py`) which runs on the **server** and is too heavy for the MCU.

```
dataset/ (YOLO labels)  ──>  prepare_dataset.py  ──>  Edge Impulse training
                                                        │
                                                        v
                                          FOMO INT8 model  ──>  ESP32 firmware
                                                                (components/inference)
```

## Why FOMO?

FOMO is designed for microcontrollers: it detects the *presence and location*
of objects (like "is there feces, and roughly where") using a very small
network at e.g. 96x96 input, quantized to int8. It does **not** produce full
bounding boxes — that trade-off is what lets it fit on the ESP32-S3's memory
and run fast enough for on-device spray decisions. Full, accurate boxes are
recomputed on the server by YOLOv8.

## Steps

### 1. Collect & annotate data
Same dataset as the YOLOv8 pipeline (`../dataset/images/{train,val}` +
`../dataset/labels/{train,val}`). One class: `feces`.

### 2. Convert for Edge Impulse
```bash
python prepare_dataset.py --target-size 96
```
Creates `out_fomo/` with `train.zip`, `test.zip` and `train.csv`/`test.csv`
in the Edge Impulse bounding-box schema.

### 3. Edge Impulse project
1. Create a new project in [Edge Impulse](https://studio.edgeimpulse.com).
2. **Data acquisition** → upload `train.zip` (images) and `train.csv`
   (annotations). Upload `test.csv`/`test.zip` as the test set.
   Assign all samples the label `feces`.
3. **Impulse design**:
   - Image block: 96x96.
   - Processing block: **Image** (grayscale or RGB — smaller footprint for
     grayscale).
   - Learning block: **Object Detection (FOMO)** — this is the key block.
4. **Generate features**, then **Train**:
   - Use a small neural network, e.g. **FOMO-MobileNetV2 0.35** (or 0.25 for
     the smallest footprint).
   - Target quantization: **int8**.
   - Aim for high accuracy; FOMO is tolerant of some false positives because
     the server YOLOv8 re-verifies.
5. **Model testing / Live classification** to confirm it generalizes.

### 4. Export for the ESP32-S3
- Go to **Deployment** → choose the **ESP32 / ESP32-S3 C++ library**
  (or "Arduino library") target → **Build**.
- Download and unzip into:
  ```
  firmware/esp32s3_fomo/components/inference/edge_impulse/
  ```
- Wire it in — see the instructions at the top of
  `firmware/esp32s3_fomo/components/inference/edge_impulse_invoke.c`
  (replace the stub with your `run_classifier()` call and set the input
  geometry to match 96 or 160).

## Files

| File | Purpose |
|------|---------|
| `prepare_dataset.py` | Converts YOLO dataset → Edge Impulse ZIP + CSV |
| `README.md` | This guide |

## Relationship to YOLOv8

- **ESP32-S3**: FOMO (this folder) — lightweight, on-device, fast auto-spray.
- **Server**: YOLOv8 (`../train.py`, `../evaluate.py`) — full boxes, verified,
  analytics.

Both share the same annotated pigpen dataset.
