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
network, quantized to int8. It does **not** produce full bounding boxes — that
trade-off is what lets it fit on the ESP32-S3's memory and run fast enough for
on-device spray decisions. Full, accurate boxes are recomputed on the server by
YOLOv8.

## Why 160x160 and not 96x96

FOMO emits a per-cell score map at 1/8 the input resolution, and it learns
*object centroids* per cell. That means small objects are only learnable while
they are comfortably larger than a single heat-map cell.

Measured on this dataset (normalized box sizes from the YOLO labels):

| input | heat map | 10th-pct feces blob (longest side) |
|-------|----------|-----------------------------------|
| 96x96 | 12x12    | ~5.2 px  (under one cell)           |
| **160x160** | **20x20** | **~8.8 px** (fits a cell)     |

96x96 is the smallest footprint but many feces boxes fall below one cell, so
they are effectively unsupervised. **160x160 is the intended input.** The S3 has
8 MB PSRAM, so the extra ~4x pixel count is comfortable.

Also set the **learning rate to 0.001** in the FOMO learning block — the docs
call this out as a required step, and the default is too high for this block.

## Steps

### 1. Collect & annotate data
Same dataset as the YOLOv8 pipeline (`../../dataset/images/{train,val}` +
`../../dataset/labels/{train,val}`). **Two classes: `feces` (0) and `pig` (1)**,
matching `../dataset.yaml`. Keeping the pig boxes is what stops the model from
learning "pig body = background", which caused false-positive sprays. Only
class 0 ever triggers a spray.

### 2. Pack for Edge Impulse
```bash
python prepare_dataset.py --format yolo-txt
```
Creates `out_fomo/train.zip` and `out_fomo/test.zip` in Edge Impulse's
**YOLO TXT** layout, each carrying its own `classes.txt` + `data.yaml`:

```
train.zip -> train/images/*.jpg  train/labels/*.txt  classes.txt  data.yaml
test.zip  -> test/images/*.jpg   test/labels/*.txt   classes.txt  data.yaml
```

This format is already how our YOLO labels are stored
(`class_id center_x center_y width height`, normalized), so the files are copied
byte-for-byte — no coordinate math to get wrong. The script prints per-class box
counts and hard-fails if a class id is out of range, so a single-class zip can
never be produced silently.

Use `--format csv` only to reproduce older runs; that path needs the CSV
Wizard's manual column mapping and is not the documented Plain CSV schema.

### 3. Edge Impulse project
1. Create a new project in [Edge Impulse](https://studio.edgeimpulse.com).
2. Set the project's **labeling method to Bounding boxes** and target the
   **ESP32-S3**.
3. **Data acquisition → Upload**:
   - `out_fomo/train.zip` → format **YOLO TXT** → category **training**
   - `out_fomo/test.zip`  → format **YOLO TXT** → category **testing**
   Each zip already contains `classes.txt`, so no separate label file or CSV
   Wizard is needed. After both uploads, confirm the Data acquisition page shows
   **both** `feces` and `pig` and that image counts are 2,468 / 488.
4. **Impulse design**:
   - Image block: **160x160**, RGB.
   - Learning block: **Object Detection (FOMO)** — this is the key block.
5. **Generate features**, then **Train**:
   - Model: **FOMO-MobileNetV2 0.35**.
   - Target quantization: **int8**.
   - Learning rate: **0.001**.
6. **Model testing** — check the F1 score for the `feces` class specifically on
   the 488 held-out images.

### 4. Export for the ESP32-S3
- Go to **Deployment** → **C++ library** → **Build**. Prefer the **EON
  Compiler** target (lower RAM/ROM); **TensorFlow Lite int8** is a fine fallback.
- Download and unzip into:
  ```
  firmware/esp32s3_fomo/components/inference/edge_impulse/
  ```
  The folder currently holds only `.gitkeep` + `README.md`, and is git-ignored
  so the generated SDK is not committed.
- Wire it in — see the instructions at the top of
  `../../firmware/esp32s3_fomo/components/inference/edge_impulse_invoke.c`.
  The top-level CMakeLists adds the exported component automatically once the
  folder is populated; the file then compiles the real `run_classifier()` path
  instead of the pre-export luma-brightness stub.

### 5. Verify on hardware
Two tests decide whether this is finished:
- **Feces in frame → relay fires.**
- **Pig in frame, no feces → relay does not fire.**

The brightness stub could never pass the second one; a real FOMO model can.

## Files

| File | Purpose |
|------|---------|
| `prepare_dataset.py` | Packs YOLO dataset → Edge Impulse YOLO TXT zips (or legacy CSV) |
| `README.md` | This guide |

## Relationship to YOLOv8

- **ESP32-S3**: FOMO (this folder) — lightweight, on-device, fast auto-spray.
- **Server**: YOLOv8 (`../train.py`, `../evaluate.py`) — full boxes, verified,
  analytics.

Both share the same annotated two-class pigpen dataset.

## A note on honest numbers

FOMO's F1 on the Roboflow frames is a measurement of phone-camera views of a
pen. The device sees an OV3660 at whatever angle it is mounted, in the pen's
own lighting. Those distributions differ. Before trusting a score in the field,
capture 40–60 frames from the real ESP32 in the real pen and treat those as the
real test set — see `../notes/` for the honest val-set discussion.
