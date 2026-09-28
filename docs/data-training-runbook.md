# Data collection & training runbook

End-to-end, step-by-step guide to go from **zero images** to **two deployed
models**:

1. **Server model (YOLOv8)** — full boxes, runs on the Python verify server.
2. **On-device model (FOMO)** — lightweight, runs on the ESP32-S3.

Both share the same annotated dataset. This is the part **you** must do (it
needs real pigpen photos); everything downstream is already scripted.

---

## Phase A — Collect images

Target: **~300–800 images**, roughly 50% containing feces and 50% clean
background (to avoid false positives).

**Shooting tips**
- Use the ESP32-S3 camera at the *final* mounted angle (put a bench test first).
- Capture varied conditions: day/night, wet/dry pen, different lighting angles,
  shadows, mud, bedding — anything the model must distinguish from feces.
- Save as `.jpg` or `.png`. Keep the same size/resolution per set (e.g. 640x480).

**Where to put them**
```
dataset/images/train/   (≈80%)
dataset/images/val/     (≈20%)
```
(Leave `dataset/images/val/` images + labels out of training so evaluation is
honest.)

---

## Phase B — Label in YOLO format

One class: **`feces`** (class id `0`).

Create one `.txt` per image with the same base name:

```
dataset/labels/train/IMG_001.txt
dataset/labels/val/IMG_050.txt
```

Each line: `<class_id> <x_center> <y_center> <width> <height>` (normalized 0–1).
Example for one feces blob:
```
0 0.512 0.437 0.156 0.203
```

**Tooling**
- [LabelImg](https://github.com/heartexlabs/labelImg) — desktop, exports YOLO txt directly.
- [Roboflow](https://roboflow.com) — web; export to **YOLO v8** format, download, unzip into `dataset/`.

**Check** that every labelled image has a matching `.txt` and every `.txt` has
≥1 line of `0 ...`.

---

## Phase C — Train the server model (YOLOv8)

The training scripts live in `training/` and only need Python + the deps in
`requirements.txt` (already installed).

```bash
cd training

# 1. Train (yolov8n -> fast baseline; yolov8s for more accuracy)
python train.py --model yolov8n.pt --epochs 100

# 2. Evaluate (target mAP50 >= 80%; exits with code 1 if below target)
python evaluate.py --model runs/detect/weights/best.pt
```

**Results** land in `training/runs/detect/weights/best.pt` and
`evaluation_summary.txt`. Record metrics into `training/notes/` for panel
evidence.

> **On CPUs this is slow** (~25 min/epoch @ 640 → 100 epochs ≈ 40 h). If you
> don't have a GPU locally, train on **Google Colab** (free T4) instead — see
> [Train on Google Colab (GPU)](###-train-on-google-colab-gpu) below.

### Train on Google Colab (GPU)

Recommended when local training on CPU is too slow. The dataset is pre-packed.

1. **Upload the dataset** (2,449 train + 485 val images+labels, ~92 MB, git-ignored):
   ```
   dataset/pigfeces-yolo.zip
   ```
   Upload it to **your Google Drive** root (or edit `DRIVE_ZIP` in the notebook).

2. **Open the notebook** at `docs/train_yolov8_colab.ipynb` in Colab
   (File → Open notebook → Upload → select the file).

3. **Set `REPO_URL`** in cell 2 to your repo URL so Colab can `git clone` the
   `training/` scripts + `dataset.yaml`.

4. **Runtime → Change runtime type → GPU (T4)**, then **Run all**.

   The notebook mounts Drive, clones the scripts, unzips the dataset, rewrites
   `dataset.yaml` `path:` to the Colab path, installs `ultralytics`, trains
   (yolov8n, 100 epochs, imgsz 640, batch 16, GPU — **~40–90 min**), evaluates
   against the 0.80 target, and writes **`training-outputs.zip` back to Drive**.

5. **Download `training-outputs.zip`** from Drive and unzip it into the repo so
   that:
   ```
   training/runs/detect/weights/best.pt     <- used by verify server + evaluate.py
   training/runs/detect/evaluation_summary.txt
   training/runs/detect/results.csv
   ```

   Rather than unzipping by hand, use the installer — it finds the weights
   inside the zip no matter which layout the run used, and refuses to clobber
   an existing `best.pt` without `--force`:

   ```bash
   python training/install_colab_weights.py /path/to/training-outputs.zip
   ```

   Both weight layouts are supported everywhere:
   `runs/detect/weights/best.pt` (what `--exist-ok` produces) and
   `runs/detect/train/weights/best.pt` (what an incremented run name produces).
   The verify server probes both automatically.

**Point the verify server at it**
```bash
cd server/yolo_verify
export YOLO_MODEL="/abs/path/to/training/runs/detect/weights/best.pt"
uvicorn app:app --host 0.0.0.0 --port 8000
```
`curl http://localhost:8000/health` should report `model_loaded: true`.

---

## Phase D — Train the on-device model (FOMO, ESP32-S3)

### D1. Convert the dataset for Edge Impulse

```bash
cd training/fomo
python prepare_dataset.py --target-size 96
```
This creates `training/fomo/out_fomo/` with `train.zip`, `test.zip` and
`train.csv`/`test.csv` in Edge Impulse's bounding-box schema.

### D2. Edge Impulse project
1. Create a project at https://studio.edgeimpulse.com.
2. **Data acquisition** → upload `train.zip` (images) and `train.csv`
   (annotations) → assign label **`feces`**. Repeat for `test.zip`/`test.csv`.
3. **Impulse design**:
   - Image block: **96×96**.
   - Processing block: **Image** (grayscale shrinks footprint further).
   - Learning block: **Object Detection (FOMO)**.
4. **Generate features**, then **Train**:
   - Network: **FOMO-MobileNetV2 0.35** (0.25 = smallest footprint).
   - Quantization: **int8**.
5. **Model testing / Live classification** to confirm it generalizes.

### D3. Export to the ESP32 firmware
1. **Deployment** → **ESP32 / ESP32-S3 C++ library** → **Build** → download.
2. Unzip **directly into**:
   ```
   firmware/esp32s3_fomo/components/inference/edge_impulse/
   ```
3. Wire it in — see the top-of-file instructions in
   `firmware/esp32s3_fomo/components/inference/edge_impulse_invoke.c`
   (replace the stub, set input size, ensure includes).
4. Rebuild & flash the firmware (see `firmware/esp32s3_fomo/README.md`):
   ```bash
   idf.py build
   idf.py -p COMx flash monitor
   ```

---

## Phase E — End-to-end validation

1. ESP32 `/status` returns live JSON; `/stream` shows the feed.
2. Dashboard (CodeIgniter) shows detections; **History** rows get a
   **YOLOv8 ✓** badge when the verify server agrees.
3. Auto-spray fires on-device; manual `Spray now` works from **Control**.
4. Track objectives: mAP50 ≥ 80% (server), on-device FPS, detect→spray latency.

---

## Failure / tuning quick reference

| Symptom | Action |
|---|---|
| mAP50 < 80% | Add images, fix mislabels, more epochs, try `yolov8s` |
| FOMO misses | Use 160×160, RGB, add angle-specific images |
| False positives (spray on mud) | Raise `threshold` on device / add background images |
| Verify badge never appears | Python server not running or wrong `verify_url` |

Set the device threshold/cooldown/spray in `firmware/.../components/app_config`
(defaults: 0.60 / 300000 ms / 5000 ms).
