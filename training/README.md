# Training

This folder holds the YOLOv8 training pipeline for pig feces detection.

## Quick Start

```bash
# 1. Prepare dataset (see dataset structure below)
# 2. Train
python train.py --model yolov8n.pt --epochs 100

# 3. Evaluate (target: mAP50 >= 80%)
python evaluate.py --model runs/detect/weights/best.pt

# 4. Use best.pt on the verification server
#    set YOLO_MODEL to runs/detect/weights/best.pt in server/yolo_verify
```

## Dataset Structure

```
dataset/
  images/
    train/          # 80% of your images
      img_001.jpg
      img_002.jpg
    val/            # 20% of your images
      img_050.jpg
  labels/
    train/          # YOLO format .txt files (same names as images)
      img_001.txt
      img_002.txt
    val/
      img_050.txt
```

### YOLO Label Format

Each `.txt` file contains one row per object:
```
<class_id> <x_center> <y_center> <width> <height>
```
All values are normalized 0-1. Example for a single feces detection:
```
0 0.512 0.437 0.156 0.203
```

### Labeling Tools

Use any of these to annotate your images:
- [LabelImg](https://github.com/heartexlabs/labelImg) — classic, simple
- [Roboflow](https://roboflow.com) — web-based, exports YOLO format directly
- [CVAT](https://cvat.ai) — advanced, team collaboration

## Files

| File | Purpose |
|------|---------|
| `dataset.yaml` | YOLOv8 dataset config |
| `train.py` | Training script |
| `evaluate.py` | Evaluation + accuracy validation |
| `runs/` | Training output (auto-created, gitignored) |
| `notes/` | Evidence for capstone panel |

## Model Options

| Model | Size | mAP (COCO) | Best For |
|-------|------|------------|----------|
| `yolov8n` | 6.2 MB | 37.3 | Fast inference, good baseline |
| `yolov8s` | 22.5 MB | 44.9 | Better accuracy |
| `yolov8m` | 52 MB | 50.2 | Highest accuracy |

Start with `yolov8n` for quick iteration, then try `yolov8s` for better results.
These run on the **Python verification server** (not the ESP32).

## Exporting for the Python verification server

The trained `best.pt` is used directly by the server-side verification service:
```
server/yolo_verify/        (FastAPI + ultralytics -> POST /verify)
```
Point its `YOLO_MODEL` env var at `runs/detect/weights/best.pt`.

## On-device model (ESP32-S3)

The Raspberry Pi is **no longer part of this system**. For on-device
inference on the ESP32-S3, use the separate **FOMO** pipeline in `fomo/` —
a lightweight detector tuned for microcontrollers. YOLOv8 is too heavy to
run on the ESP32 and is used only on the server.
