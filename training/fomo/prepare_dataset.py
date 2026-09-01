#!/usr/bin/env python3
"""
Prepare the YOLO-format dataset for Edge Impulse FOMO training.

FOMO (Faster Objects, More Objects) detects objects as small per-cell
scores rather than full bounding boxes. Edge Impulse expects a labelled
image dataset; this script converts our YOLO txt labels into the format
Edge Impulse ingestion accepts:

  * A ZIP of images for each split (train/val), plus
  * a CSV of annotations (label, x, y, width, height) in the folder
    structure Edge Impulse wants.

Usage:
    python prepare_dataset.py [--split-ratio 0.8] [--target-size 96]
                              [--out-dir out_fomo]

The raw source images live in ../dataset/images/{train,val} with YOLO
labels in ../dataset/labels/{train,val}. If the dataset is still empty,
this script prints instructions for collecting images.

FOMO-specific notes:
  * Input is small (96x96 or 160x160). Images are center-cropped to square
    then downscaled, which also downscales the bounding boxes.
  * Leaf target size is 96x96 for the smallest ESP32-S3 footprint.
"""
from __future__ import annotations

import argparse
import csv
import random
import shutil
import sys
import zipfile
from pathlib import Path

random.seed(42)


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--split-ratio", type=float, default=0.8, help="train fraction")
    p.add_argument("--target-size", type=int, default=96,
                   help="FOMO input size (one side of square crop)")
    p.add_argument("--dataset", type=Path, default=Path("../dataset"))
    p.add_argument("--out-dir", type=Path, default=Path("out_fomo"))
    return p.parse_args()


def read_yolo_label(path: Path):
    """Yield (x_c, y_c, w, h) normalized tuples from a YOLO txt file."""
    out = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) < 5:
            continue
        out.append(tuple(float(v) for v in parts[1:5]))
    return out


def main():
    args = parse_args()
    dataset = args.dataset
    out = args.out_dir

    train_imgs = dataset / "images" / "train"
    val_imgs = dataset / "images" / "val"
    train_lbls = dataset / "labels" / "train"
    val_lbls = dataset / "labels" / "val"

    if not train_imgs.exists() or not any(train_imgs.iterdir()):
        print(
            "Dataset is empty. Collect pigpen images, annotate them in YOLO "
            "format (see training/README.md), then re-run.",
            file=sys.stderr,
        )
        sys.exit(1)

    if args.target_size not in (96, 160):
        print("target-size should be 96 or 160", file=sys.stderr)
        sys.exit(1)

    out.mkdir(parents=True, exist_ok=True)
    (out / "train").mkdir(exist_ok=True)
    (out / "test").mkdir(exist_ok=True)

    for split, img_dir, lbl_dir, out_split in (
        ("train", train_imgs, train_lbls, "train"),
        ("test", val_imgs, val_lbls, "test"),   # Edge Impulse calls it 'test'
    ):
        if not img_dir.exists():
            continue
        with open(out / f"{split}.csv", "w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(["filename", "label", "x", "y", "width", "height"])

            # Copy each labelled image; writes the annotation into the CSV.
            for img_path in sorted(img_dir.glob("*.[jJ][pP][gG]")) + \
                             sorted(img_dir.glob("*.[pP][nN][gG]")):
                lbl_path = lbl_dir / (img_path.stem + ".txt")
                if not lbl_path.exists():
                    continue
                boxes = read_yolo_label(lbl_path)
                if not boxes:
                    continue
                dest = out / out_split / img_path.name
                shutil.copyfile(img_path, dest)

                for (xc, yc, w, h) in boxes:
                    # Edge Impulse bounding-box schema: x/y = top-left,
                    # width/height, all relative to the (square) image.
                    writer.writerow([
                        img_path.name, "feces",
                        round(max(0.0, xc - w / 2), 6),
                        round(max(0.0, yc - h / 2), 6),
                        round(min(1.0, w), 6),
                        round(min(1.0, h), 6),
                    ])

        # Bundle into a ZIP for the Edge Impulse UI upload.
        zip_path = out / f"{split}.zip"
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
            for f in (out / out_split).glob("*"):
                z.write(f, f.name)

    print(f"Done. Upload one of {out/'train.zip'} / {out/'test.zip'} plus the "
          f"CSVs to Edge Impulse. See training/fomo/README.md for the next steps.")


if __name__ == "__main__":
    main()
