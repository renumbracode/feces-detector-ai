#!/usr/bin/env python3
"""
Install best.pt/last.pt from the Colab training-outputs.zip into the repo's
expected location so the verify server (running on :8000) picks it up lazily.

Colab's zips store the weights flat as  weights/best.pt. The repo expects
    training/runs/detect/weights/best.pt
which is where training/train.py writes when it runs with --exist-ok.

If the zip is flat/unknown, this also accepts the nested variant so it works
either way:

    python install_colab_weights.py [path/to/training-outputs.zip] [--force]
"""
import argparse
import shutil
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]          # repo root
RUN_DIR = ROOT / "training" / "runs" / "detect"
CANONICAL = RUN_DIR / "weights"
ALT = RUN_DIR / "train" / "weights"

# Anything under the zip whose tail matches these lands in the run's weights dir.
WEIGHT_SUFFIXES = ("weights/best.pt", "weights/last.pt")
METRIC_SUFFIXES = ("results.csv", "evaluation_summary.txt")


def pick_dest() -> Path:
    """Prefer whichever layout already holds weights, else the canonical one."""
    if CANONICAL.joinpath("best.pt").exists():
        return CANONICAL
    if ALT.joinpath("best.pt").exists():
        return ALT
    return CANONICAL


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("zip", nargs="?", default="training-outputs.zip",
                    help="Path to training-outputs.zip (downloaded from Drive).")
    ap.add_argument("--force", action="store_true",
                    help="Overwrite an existing best.pt.")
    args = ap.parse_args()

    zip_path = Path(args.zip)
    if not zip_path.exists():
        print(f"ERROR: zip not found at {zip_path} (download it from Drive first)")
        return 1

    dest = pick_dest()
    if dest.joinpath("best.pt").exists() and not args.force:
        print(f"ERROR: {dest / 'best.pt'} already exists. Re-run with --force to overwrite.")
        return 1

    dest.mkdir(parents=True, exist_ok=True)
    installed: list[Path] = []
    with zipfile.ZipFile(zip_path) as z:
        names = z.namelist()
        for name in names:
            tail = "/".join(name.split("/")[-2:]) if name.count("/") >= 1 else name
            leaf = Path(name).name
            if any(name.endswith(s) or tail.endswith(s) for s in WEIGHT_SUFFIXES) and leaf.endswith(".pt"):
                out = dest / leaf
                with z.open(name) as src, open(out, "wb") as dst:
                    shutil.copyfileobj(src, dst)
                installed.append(out)
            elif any(name.endswith(s) for s in METRIC_SUFFIXES):
                out = RUN_DIR / leaf
                with z.open(name) as src, open(out, "wb") as dst:
                    shutil.copyfileobj(src, dst)
                installed.append(out)

    if not any(p.name == "best.pt" for p in installed):
        print(f"ERROR: no weights/best.pt found inside {zip_path.name}")
        print(f"       zip entries were: {names[:20]}")
        return 1

    for p in installed:
        print(f"installed: {p}")
    print()
    print("Next: python training/evaluate.py --model "
          f"{dest / 'best.pt'}")
    print("Then point the verify server at it:")
    print(f"  set YOLO_MODEL={dest / 'best.pt'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
