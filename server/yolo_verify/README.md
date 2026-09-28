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
| `YOLO_CONF` | `0.25` |

## Integration

The ESP32 firmware (`components/detection_service`) posts detected frames to
`POST /verify`. The CodeIgniter dashboard can also call `/verify` (or read the
stored verification result) when a user wants a second opinion on a recorded
detection.
