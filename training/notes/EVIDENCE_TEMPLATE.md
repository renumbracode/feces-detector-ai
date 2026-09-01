# Panel evidence template

Fill this in as each milestone completes. Pair every claim with a screenshot in this folder. Duplicate this file per round (e.g. mock-pen round, real-pen round).

## 1. Dataset

- Total images: ______
- With feces (positive): ______
- Without feces (background): ______
- Capture source: [ ] USB UVC webcam   [ ] other: ______
- Labeling tool: [ ] LabelImg   [ ] Roboflow   [ ] CVAT
- Train/val split: 80% / 20%
- Screenshot: dataset folder structure.

## 2. Trained model

- Model: YOLOv8 ______ (n/s/m)
- Epochs: ______   Learning rate: ______   Image size: ______
- mAP50: ______   mAP50-95: ______
- Precision: ______   Recall: ______   F1: ______
- Confusion matrix screenshot: `confusion-matrix.png`
- Training curves screenshot: `training-curves.png`
- Notes on retraining rounds (what was changed and the effect).

## 3. On-device (Raspberry Pi)

- Raspberry Pi model: ______
- Camera: USB UVC webcam (brand/model: ______)
- Model export: [ ] PyTorch   [ ] NCNN   [ ] ONNX
- Inference speed: ______ FPS
- Detection-to-spray latency: ______ ms
- Screenshot: live detection on the annotated stream.

## 4. IoT trigger

- Relay GPIO: 13 (BCM)   spray duration: ______ ms   cooldown: ______ ms
- Pump type / voltage: ______
- Manual spray test passed: [ ] yes   [ ] no
- Screenshot/video: spray firing, or dashboard History showing `triggered=Yes`.

## 5. Dashboard

- Screenshot: Dashboard stats page
- Screenshot: History log with a triggered detection
- Screenshot: Live view with annotated stream.

## 6. Field results (if measured)

- False positives / hour: ______   main cause: ______
- False negatives / hour: ______   main cause: ______
- Water used per event: ______

## 7. Retrain log

Shows the panel that the AI was iteratively trained. Add a row per training round.

| Round | Change (dataset / params) | Epochs | mAP50 | Precision | Recall | Notes |
|-------|---------------------------|--------|-------|-----------|--------|-------|
| 1 | initial dataset, yolov8n | | | | | |
| 2 | added X images / fixed labels | | | | | |
| 3 | real-pen capture | | | | | |
| ... | | | | | | |
