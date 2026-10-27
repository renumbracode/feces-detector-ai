"""
YOLOv8 verification server for the ESP32-S3 Feces Detector.

The ESP32-S3 runs lightweight Edge Impulse FOMO inference on-device and
uploads detected JPEG frames here. This service re-runs the frames through a
full Ultralytics YOLOv8 detection model (which cannot feasibly run on the
MCU) and returns high-confidence verification for logging/analytics.

Multi-instance: every feces box at or above the confidence floor is reported,
not just the best one -- one frame can hold several feces spots, and the
instance count is part of the answer (feces_count + boxes_norm).

Endpoints:
  GET  /health               liveness + model info
  POST /verify               body = raw JPEG, header X-Fomo-Conf = fomo score,
                             optional X-Camera-Id = camera label
                             -> { ok, uclass, yolo_conf, feces_count,
                                  box_norm, boxes_norm, boxes, ... }

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
from starlette.concurrency import run_in_threadpool
from starlette.requests import ClientDisconnect

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
# NMS overlap threshold. Ultralytics defaults to 0.7, which let two overlapping
# detections of ONE blob both survive the server (observed pair at IoU 0.61):
# the board shipped both and the dashboard was left guessing whether they were
# the same object. 0.45 collapses duplicates of a single spot while keeping
# genuinely distinct neighbouring spots as separate boxes.
NMS_IOU = float(os.environ.get("YOLO_NMS_IOU", "0.45"))
MAX_IMAGE_SIZE = (416, 416)  # resize long edge toward 416 to keep inference fast
TARGET_CLASS = os.environ.get("YOLO_TARGET_CLASS", "feces")  # trained model, 2-class
# Cap on target boxes returned in the compact boxes_norm array. The firmware
# holds VERIFY_MAX_FECES_BOXES = 4 entries and reads this reply into a fixed
# buffer, so both sides agree on the same wire cap.
MAX_TARGET_BOXES = int(os.environ.get("YOLO_MAX_TARGET_BOXES", "4"))

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


def _run_yolo(arr):
    """CPU-bound; call through run_in_threadpool.

    predict() on a CPU-only torch build takes seconds. Running it directly in
    the async handler blocked uvicorn's event loop, so while frame N was being
    predicted the board's frame N+1 upload sat unread in the socket -- the
    device gave up at its own timeout and we logged ClientDisconnect. Off the
    loop, uploads are accepted while inference runs.
    """
    return get_model().predict(arr, conf=CONF_THRESHOLD, iou=NMS_IOU, verbose=False)


@app.on_event("startup")
def warm_model() -> None:
    """Load the weights before the first /verify arrives.

    Lazy-loading inside the handler made the very first request pay the full
    model load, which the device's HTTP timeout could not absorb -- that first
    poll failed and the overlay sat on the 30 s backoff. Loading at startup
    also makes /health report model_loaded: true so the state is observable.
    """
    try:
        get_model()
    except Exception as exc:  # noqa: BLE001
        # Never block startup: /health should still come up and report the
        # failure rather than the whole server refusing to boot.
        print(f"model warm-up failed: {exc}")


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
async def verify(request: Request, x_fomo_conf: float = Header(default=0.0),
                 x_camera_id: str = Header(default="")):
    try:
        img_bytes = await request.body()
    except ClientDisconnect:
        # The device abandoned the upload (its own timeout, or a dropped
        # socket). Answering 400 keeps this out of the server error log; there
        # is no request left to infer against.
        return JSONResponse({"ok": False, "error": "client disconnected"}, status_code=400)
    if not img_bytes:
        return JSONResponse({"ok": False, "error": "empty body"}, status_code=400)

    fomo_conf = max(0.0, min(1.0, x_fomo_conf))
    # Multi-camera groundwork: each device can label its uploads. Logged only
    # for now; the dashboard cameras table maps labels to pens.
    camera_id = x_camera_id.strip()[:32]
    if camera_id:
        print(f"verify: camera={camera_id} bytes={len(img_bytes)}")

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
    results = await run_in_threadpool(_run_yolo, arr)

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
    # so a "no feces" answer can be explained. Multi-instance: EVERY feces
    # box at or above the floor is counted and returned, not just the best.
    feces_conf = best_per_class.get(TARGET_CLASS, 0.0)
    pig_conf = best_per_class.get("pig", 0.0)
    best_conf = max(best_per_class.values(), default=0.0)
    uclass = TARGET_CLASS if feces_conf > 0 else (
        max(best_per_class, key=best_per_class.get) if best_per_class else "none"
    )
    feces_boxes = [b for b in boxes if b["class"] == TARGET_CLASS]
    feces_boxes.sort(key=lambda b: -b["conf"])
    feces_count = len(feces_boxes)
    triggered = feces_count > 0

    # Normalized 0..1 geometry for the best target-class box, so a thin client
    # (the ESP32 overlay) can draw it without knowing the frame size. Sent in
    # post-resize coordinates, matching "boxes" above.
    bw = max(1, width)
    bh = max(1, height)

    def _norm(b):
        nx1 = max(0.0, b["x1"] / bw)
        ny1 = max(0.0, b["y1"] / bh)
        nx2 = min(1.0, b["x2"] / bw)
        ny2 = min(1.0, b["y2"] / bh)
        if nx2 <= nx1 or ny2 <= ny1:
            return None
        return {
            "x": round(nx1, 4),
            "y": round(ny1, 4),
            "w": round(nx2 - nx1, 4),
            "h": round(ny2 - ny1, 4),
        }

    box_norm = _norm(best_target_box) if best_target_box is not None else None
    boxes_norm = [n for n in (_norm(b) for b in feces_boxes[:MAX_TARGET_BOXES]) if n]

    return {
        # Compact fields first: the ESP32 reads this reply into a small fixed
        # buffer, so what it needs must appear before the verbose box list.
        "ok": True,
        "yolo_conf": round(feces_conf, 4),
        "detected": triggered,
        "feces_count": feces_count,
        "box_norm": box_norm,
        "boxes_norm": boxes_norm,
        "uclass": uclass,
        "fomo_conf": fomo_conf,
        "triggered": triggered,
        "camera_id": camera_id,
        "best_any_conf": round(best_conf, 4),
        "feces_conf": round(feces_conf, 4),
        "pig_conf": round(pig_conf, 4),
        "n_boxes": len(boxes),
        "boxes": sorted(boxes, key=lambda b: -b["conf"])[:8],
        "width": width,
        "height": height,
    }
