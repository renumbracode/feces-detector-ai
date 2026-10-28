# YOLOv8 Verification Server (Python + FastAPI)

Runs a full **Ultralytics YOLOv8** detection model on the server to verify the
lightweight on-device FOMO detections made by the ESP32-S3.

**Why a server?** Unmodified YOLOv8 cannot run on an ESP32-S3 (model size, op
support, and compute budget are far beyond the MCU). The ESP32 does fast local
FOMO inference + auto-spray; YOLOv8 runs here for high-accuracy
verification/analytics on uploaded frames.

## Run

```bash
pip install -r requirements.txt

# optional: point at a different trained weights file
export YOLO_MODEL="../../../training/runs/detect/weights/best.pt"

uvicorn app:app --host 0.0.0.0 --port 8000
```

## Endpoints

### `GET /health`
Model path + load state.

### `POST /verify`
- **body**: raw JPEG bytes
- **header** `X-Fomo-Conf`: the on-device FOMO score (optional, for joining)

Returns:

```json
{
  "ok": true,
  "yolo_conf": 0.8712,
  "uclass": "feces",
  "fomo_conf": 0.9,
  "triggered": true,
  "n_boxes": 2,
  "boxes": [{"x1":...,"y1":...,"x2":...,"y2":...,"conf":...,"class":"feces"}],
  "width": 1600,
  "height": 1200
}
```

## Config

| Env var | Default |
|---|---|
| `YOLO_MODEL` | `<repo>/training/runs/detect/weights/best.pt` |
| `YOLO_CONF` | `0.15` |
| `YOLO_NMS_IOU` | `0.45` |
| `YOLO_MAX_TARGET_BOXES` | `4` |

`YOLO_CONF` is the floor for **reporting** a detection, not the spray gate --
the board's own `server_trigger_conf` decides that. It is 0.15 rather than the
old 0.25 because this model's feces class is weak (mAP50 15.1%), so a real
blob scores 0.15-0.45; at a 0.25 floor the server reported "no feces" for
frames where the blob was plainly visible.

Inference runs at the model's native `imgsz=640` (the camera uploads 640x480).
An earlier 416px downscale bought no speed but halved the confidence, because
Ultralytics letterboxes the image back up to 640 for inference.

## Integration

The ESP32 firmware (`components/detection_service`) posts detected frames to
`POST /verify`. The CodeIgniter dashboard can also call `/verify` (or read the
stored verification result) when a user wants a second opinion on a recorded
detection.
