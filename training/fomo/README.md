# Edge Impulse FOMO pipeline (ESP32-S3 on-device detection)

This sub-folder is the **on-device** half of the model pipeline. It produces a
small **FOMO (Faster Objects, More Objects)** object detector that actually
runs on the ESP32-S3 — unlike the full YOLOv8 pipeline (see `../train.py`,
`../evaluate.py`) which runs on the **server** and is too heavy for the MCU.

```
dataset/ (YOLO labels)  ──>  prepare_ei_dataset.py  ──>  Edge Impulse training
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

## Why a leakage-free pack in the first place

The old split could not be reused: 384 of its 488 validation images (78.7%)
came from a capture session that also appeared in training. Consecutive frames
share camera position, pen and lighting, so a model could score well by
memorising a session's background instead of learning what feces looks like.

`prepare_ei_dataset.py` fixes that by splitting on **whole capture sessions**
(filenames encode them, e.g. `IMG_20240322_...`), unioning sessions that share
near-duplicate frames (dHash), and hard-failing if any session or near-duplicate
cluster ever spans two splits.

## Steps

### 1. Collect & annotate data
Same dataset as the YOLOv8 pipeline (`../../dataset/images/{train,val}` +
`../../dataset/labels/{train,val}`). **Two classes: `feces` (0) and `pig` (1)**,
matching `../dataset.yaml`. Keeping the pig boxes is what stops the model from
learning "pig body = background", which caused false-positive sprays. Only
class 0 ever triggers a spray.

### 2. Pack for Edge Impulse (leakage-free)
```bash
python prepare_ei_dataset.py
```
Creates `out_ei/{train,val,test}.zip` in Edge Impulse's **YOLO TXT** layout,
each in the documented structure carried exactly as the uploader expects:

```
<split>.zip -> train/images/*.jpg  train/labels/*.txt  classes.txt
```

Class names live in `classes.txt` (`feces\npig\n`). There is intentionally **no
`data.yaml`** inside the zips: `classes.txt` is sufficient for Edge Impulse,
and `../dataset.yaml` remains the single source of class configuration for the
server-side YOLOv8 pipeline. Labels are copied byte-for-byte from source — no
coordinate math to get wrong, and the script hard-fails if a class id is out of
range or a box is malformed/out-of-frame.

Every run is deterministic (fixed seed 42). Re-running reproduces the exact
same split — box counts must not move. The script prints a `MANIFEST.txt`
recording per-split counts, sessions, near-duplicate clusters, and the leak-free
unit assignment for every split.

### 3. Edge Impulse project
1. Create a new project in [Edge Impulse](https://studio.edgeimpulse.com).
2. Set the project's **labeling method to Bounding boxes** and target the
   **ESP32-S3**.
3. **Enable the explicit validation set** advanced setting
   (Data acquisition → Dataset → Advanced): this is what makes the *validation*
   category appear in the uploader.
4. **Data acquisition → Upload**, format **YOLO TXT**, category per zip:
   - `train.zip` → **training**
   - `val.zip`  → **validation** (if the option is missing, the explicit
     validation set setting is not enabled yet — see step 3)
   - `test.zip` → **test**

   > **Never use the uploader's "automatically split" option.** It shuffles at
   > the image level and destroys the session-isolation guarantee this pack was
   > built to provide.

   After all three uploads, confirm the Data acquisition page shows **both**
   `feces` and `pig`, and image counts of 2,062 / 437 / 457.
5. **Impulse design**:
   - Image block: **160x160**, RGB.
   - Learning block: **Object Detection (FOMO)** — this is the key block.
6. **Generate features**, then **Train**:
   - Model: **FOMO-MobileNetV2 0.25** (start conservative; the free-tier budget
     is 60 CPU minutes per job with no GPU, 10 experiments per project, ~16 GB
     memory. A 0.35 variant is only worth trying if 0.25 underperforms).
   - Target quantization: **int8**.
   - Learning rate: **0.001**.
   - Training cycles: **~20**, with early stopping.
   - Batch size: **32**.
7. **Model testing** — check the F1 score for the `feces` class specifically on
   the 457 held-out test images.

Note on the dataset: there are **no true background images** — only 22
pig-only/no-feces frames act as natural negatives. If the field test shows
shadow/dirt/mud false positives, collect ~150–200 real empty-pen and dirty-pen
frames and retrain.

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
| `prepare_ei_dataset.py` | Leakage-free, session-grouped YOLO TXT packer → `out_ei/` |
| `prepare_dataset.py` | Legacy packer; no session-leak protection, not for EI |
| `README.md` | This guide |

## Relationship to YOLOv8

- **ESP32-S3**: FOMO (this folder) — lightweight, on-device, fast auto-spray.
- **Server**: YOLOv8 (`../train.py`, `../evaluate.py`) — full boxes, verified,
  analytics.

Both share the same annotated two-class pigpen dataset. The FOMO zips and the
YOLOv8 pipeline are independent: `prepare_ei_dataset.py` only reads `dataset/`
and writes `out_ei/`; nothing in this folder affects `../dataset.yaml`, which is
the YOLOv8 pipeline's config.

## A note on honest numbers

FOMO's F1 on the Roboflow frames is a measurement of phone-camera views of a
pen. The device sees an OV3660 at whatever angle it is mounted, in the pen's
own lighting. Those distributions differ. Before trusting a score in the field,
capture 40–60 frames from the real ESP32 in the real pen and treat those as the
real test set — see `../notes/` for the honest val-set discussion.