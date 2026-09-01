#!/usr/bin/env python3
"""
Install best.pt/last.pt from the Colab training-outputs.zip into the repo's
expected location so the verify server (running on :8000) picks it up lazily.

Colab zips store weights at  weights/best.pt  from
training/runs/detect/weights/...  ; the repo expects:
    training/runs/detect/train/weights/best.pt

Usage:
    python install_colab_weights.py [path/to/training-outputs.zip]
"""
import shutil
import sys
import zipfile
from pathlib import Path

ZIP = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("training-outputs.zip")
ROOT = Path(__file__).resolve().parents[1]          # repo root
DEST = ROOT / "training" / "runs" / "detect" / "train" / "weights"

if not ZIP.exists():
    sys.exit(f"ERROR: zip not found at {ZIP} (download it from Drive first)")

DEST.mkdir(parents=True, exist_ok=True)
extracted = []
with zipfile.ZipFile(ZIP) as z:
    for name in ("weights/best.pt", "weights/last.pt"):
        if name in z.namelist():
            out = DEST / name.split("/")[-1]
            with z.open(name) as src, open(out, "wb") as dst:
                shutil.copyfileobj(src, dst)
            extracted.append(out)

if not extracted:
    sys.exit(f"ERROR: no weights/best.pt found inside {ZIP.name}")

for p in extracted:
    print(f"installed: {p}")
print("verify server health now expects model_exists=true on next /verify.")