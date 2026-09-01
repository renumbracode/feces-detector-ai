"""
YOLOv8 verification server for the ESP32-S3 Feces Detector.

The ESP32-S3 runs lightweight Edge Impulse FOMO inference on-device and
uploads detected JPEG frames here. This service re-runs the frames through a
full Ultralytics YOLOv8 detection model (which cannot feasibly run on the
MCU) and returns high-confidence verification for logging/analytics.

Endpoints:
  GET  /health               liveness + model info
  POST /verify               body = raw JPEG, header X-Fomo-Conf = fomo score
                             -> { ok, uclass, yolo_conf, boxes, width, height }

Run:
  pip install -r requirements.txt
  uvicorn app:app --host 0.0.0.0 --port 8000
"""
from __future__ import annotations

import io
import os
from pathlib import Path

from fastapi import FastAPI, Header, Request
from fastapi.responses import JSONResponse

from PIL import Image
import numpy as np
from ultralytics import YOLO

MODEL_PATH = os.environ.get(
    "YOLO_MODEL",
    str(Path(__file__).resolve().parents[2] / "training" / "runs" / "detect" / "train" / "weights" / "best.pt"),
)
CONF_THRESHOLD = float(os.environ.get("YOLO_CONF", "0.25"))
MAX_IMAGE_SIZE = (416, 416)  # resize long edge toward 416 to keep inference fast

app = FastAPI(title="Feces YOLOv8 Verifier", version="1.0.0")

_model: YOLO | None = None


def get_model() -> YOLO:
    global _model
    if _model is None:
        _model = YOLO(MODEL_PATH)
    return _model


@app.get("/health")
def health():
    ok = Path(MODEL_PATH).exists() if not _model else True
    return {
        "ok": ok,
        "model_path": str(MODEL_PATH),
        "model_exists": ok,
        "model_loaded": _model is not None,
        "conf_threshold": CONF_THRESHOLD,
    }


@app.post("/verify")
async def verify(request: Request, x_fomo_conf: float = Header(default=0.0)):
    img_bytes = await request.body()
    if not img_bytes:
        return JSONResponse({"ok": False, "error": "empty body"}, status_code=400)

    fomo_conf = max(0.0, min(1.0, x_fomo_conf))

    try:
        pil = Image.open(io.BytesIO(img_bytes)).convert("RGB")
    except Exception as exc:  # noqa: BLE001
        return JSONResponse({"ok": False, "error": f"bad jpeg: {exc}"}, status_code=400)

    width, height = pil.size
    long_edge = max(width, height)
    if long_edge > MAX_IMAGE_SIZE[0]:
        scale = MAX_IMAGE_SIZE[0] / long_edge
        pil = pil.resize((int(width * scale), int(height * scale)))

    arr = np.asarray(pil)
    results = get_model().predict(
        arr,
        conf=CONF_THRESHOLD,
        verbose=False,
    )

    boxes = []
    best_conf = 0.0
    uclass = "none"
    for r in results:
        if r.boxes is None or len(r.boxes) == 0:
            continue
        for box in r.boxes:
            x1, y1, x2, y2 = box.xyxy[0].tolist()
            c = float(box.conf[0])
            cls = int(box.cls[0])
            name = r.names.get(cls, str(cls))
            boxes.append({
                "x1": round(x1, 1), "y1": round(y1, 1),
                "x2": round(x2, 1), "y2": round(y2, 1),
                "conf": round(c, 4), "class": name,
            })
            if c > best_conf:
                best_conf = c
                uclass = name

    triggered = best_conf >= CONF_THRESHOLD and uclass == "feces"

    return {
        "ok": True,
        "yolo_conf": round(best_conf, 4),
        "uclass": uclass,
        "fomo_conf": fomo_conf,
        "triggered": triggered,
        "n_boxes": len(boxes),
        "boxes": boxes,
        "width": width,
        "height": height,
    }
