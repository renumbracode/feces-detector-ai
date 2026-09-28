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
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse

from PIL import Image
import numpy as np
from ultralytics import YOLO

REPO_ROOT = Path(__file__).resolve().parents[2]
RUN_DIR = REPO_ROOT / "training" / "runs" / "detect"

# Ultralytics writes weights to <project>/<name>/weights when a run is created
# with exist_ok=True (which training/train.py passes), but to
# <project>/<name>/train/weights when it has to increment a run name. Both
# layouts exist in the wild, so probe for either instead of hard-coding one.
WEIGHT_CANDIDATES = (
    RUN_DIR / "weights" / "best.pt",
    RUN_DIR / "train" / "weights" / "best.pt",
)


def resolve_model_path() -> Path:
    """YOLO_MODEL env var if set, else the first weights file that exists."""
    env = os.environ.get("YOLO_MODEL")
    if env:
        return Path(env)
    for candidate in WEIGHT_CANDIDATES:
        if candidate.exists():
            return candidate
    return WEIGHT_CANDIDATES[0]


MODEL_PATH = resolve_model_path()
CONF_THRESHOLD = float(os.environ.get("YOLO_CONF", "0.25"))
MAX_IMAGE_SIZE = (416, 416)  # resize long edge toward 416 to keep inference fast
TARGET_CLASS = os.environ.get("YOLO_TARGET_CLASS", "feces")  # trained model, 2-class

app = FastAPI(title="Feces YOLOv8 Verifier", version="1.0.0")

# The dashboard is served from Apache on :80 while this runs on :8000, so the
# live view's fetch() is cross-origin. The device is not affected either way.
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["GET", "POST", "OPTIONS"],
    allow_headers=["*"],
)

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
        "searched": [str(p) for p in WEIGHT_CANDIDATES],
        "classes": _model.names if _model else None,
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
        width, height = int(width * scale), int(height * scale)
        pil = pil.resize((width, height))
    # Boxes below are in the *resized* frame, so width/height must describe it.

    arr = np.asarray(pil)
    results = get_model().predict(
        arr,
        conf=CONF_THRESHOLD,
        verbose=False,
    )

    boxes = []
    best_per_class: dict[str, float] = {}
    best_target_box = None
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
            if c > best_per_class.get(name, 0.0):
                best_per_class[name] = c
            if name == TARGET_CLASS and (best_target_box is None or c > best_target_box["conf"]):
                best_target_box = {
                    "x1": x1, "y1": y1, "x2": x2, "y2": y2, "conf": c,
                }

    # The detector is 2-class (feces + pig). Only a feces box is a detection,
    # so never let a confident pig box mask a real feces box: report the
    # highest-scoring *feces* box, and carry the best pig score alongside it
    # so a "no feces" answer can be explained.
    feces_conf = best_per_class.get(TARGET_CLASS, 0.0)
    pig_conf = best_per_class.get("pig", 0.0)
    best_conf = max(best_per_class.values(), default=0.0)
    uclass = TARGET_CLASS if feces_conf > 0 else (
        max(best_per_class, key=best_per_class.get) if best_per_class else "none"
    )
    triggered = feces_conf >= CONF_THRESHOLD

    # Normalized 0..1 geometry for the best target-class box, so a thin client
    # (the ESP32 overlay) can draw it without knowing the frame size. Sent in
    # post-resize coordinates, matching "boxes" above.
    box_norm = None
    if best_target_box is not None:
        bw = max(1, width)
        bh = max(1, height)
        nx1 = max(0.0, best_target_box["x1"] / bw)
        ny1 = max(0.0, best_target_box["y1"] / bh)
        nx2 = min(1.0, best_target_box["x2"] / bw)
        ny2 = min(1.0, best_target_box["y2"] / bh)
        if nx2 > nx1 and ny2 > ny1:
            box_norm = {
                "x": round(nx1, 4),
                "y": round(ny1, 4),
                "w": round(nx2 - nx1, 4),
                "h": round(ny2 - ny1, 4),
            }

    return {
        # Compact fields first: the ESP32 reads this reply into a small fixed
        # buffer, so what it needs must appear before the verbose box list.
        "ok": True,
        "yolo_conf": round(feces_conf, 4),
        "detected": triggered,
        "box_norm": box_norm,
        "uclass": uclass,
        "fomo_conf": fomo_conf,
        "triggered": triggered,
        "best_any_conf": round(best_conf, 4),
        "feces_conf": round(feces_conf, 4),
        "pig_conf": round(pig_conf, 4),
        "n_boxes": len(boxes),
        "boxes": sorted(boxes, key=lambda b: -b["conf"])[:8],
        "width": width,
        "height": height,
    }
