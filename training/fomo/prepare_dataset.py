#!/usr/bin/env python3
"""
Prepare the YOLO-format dataset for Edge Impulse FOMO training.

FOMO (Faster Objects, More Objects) detects objects as small per-cell
scores rather than full bounding boxes. Edge Impulse expects a labelled
image dataset; this script converts our YOLO txt labels into the format
Edge Impulse ingestion accepts:

  * A ZIP of images for each split (train/test), plus
  * a CSV of annotations (filename, label, x, y, width, height) in the
    folder structure Edge Impulse wants.

Usage:
    python prepare_dataset.py [--target-size 96] [--out-dir out_fomo]
                              [--dataset ../dataset]

The source images live in dataset/images/{train,val} with YOLO labels in
dataset/labels/{train,val}. The dataset is 2-class (feces = 0, pig = 1);
class ids are mapped through training/dataset.yaml `names` into the label
column. Images with no annotations at all are copied in but omitted from the
CSV, which Edge Impulse treats as "no objects" background samples.

FOMO-specific notes:
  * Edge Impulse preprocesses to the model's input size (e.g. 96x96 or
    160x160) itself and rescales the boxes, so the images are uploaded at
    their native resolution and the box coordinates stay normalized to the
    original image (they already are, from the YOLO labels).
  * A 96x96 input is the smallest ESP32-S3 footprint.
"""
from __future__ import annotations

import argparse
import csv
import random
import shutil
import sys
import zipfile
from pathlib import Path


def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dataset", type=Path, default=Path("../dataset"),
                   help="Repo dataset dir with images/{train,val} labels/{train,val}.")
    p.add_argument("--out-dir", type=Path, default=Path("out_fomo"),
                   help="Where train.zip/test.zip + CSVs are written.")
    p.add_argument("--target-size", type=int, default=96,
                   help="FOMO input size the Edge Impulse model will be trained "
                        "at (96 or 160). Informational: Edge Impulse resizes and "
                        "rescales boxes internally, so images are uploaded "
                        "unmodified.")
    p.add_argument("--names", default=None,
                   help="Comma-separated class names in id order. Defaults to "
                        "the names list in training/dataset.yaml.")
    p.add_argument("--yaml", type=Path, default=Path("../dataset.yaml"),
                   help="dataset.yaml used to resolve class names.")
    return p.parse_args()


def read_yolo_label(path: Path):
    """Yield (cls_id, x_c, y_c, w, h) floats from a YOLO txt file."""
    out = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) < 5:
            continue
        try:
            vals = [float(v) for v in parts[:5]]
        except ValueError:
            continue
        out.append((int(vals[0]), vals[1], vals[2], vals[3], vals[4]))
    return out


def resolve_names(args) -> list[str]:
    """Class names in id order from --names, else training/dataset.yaml."""
    """Class names in id order from --names, else training/dataset.yaml."""
    if args.names:
        names = [n.strip() for n in args.names.split(",") if n.strip()]
        if names:
            return names
    yaml_path = Path(__file__).resolve().parent.parent / "dataset.yaml"
    if args.yaml and args.yaml != yaml_path:
        yaml_path = args.yaml
    if yaml_path.exists():
        text = yaml_path.read_text(encoding="utf-8")
        if "names:" in text:
            import re
            m = re.search(r"names:\s*\[([^\]]*)\]", text)
            if m:
                names = [n.strip().strip("'\"") for n in m.group(1).split(",")]
                names = [n for n in names if n]
                if names:
                    return names
    return ["feces", "pig"]


def main():
    args = parse_args()
    dataset = args.dataset
    out = args.out_dir
    names = resolve_names(args)

    train_imgs = dataset / "images" / "train"
    val_imgs = dataset / "images" / "val"
    train_lbls = dataset / "labels" / "train"
    val_lbls = dataset / "labels" / "val"

    if not train_imgs.exists() or not any(train_imgs.iterdir()):
        print(
            "Dataset is empty. Run training/convert_roboflow_pigfeces.py first "
            "to build dataset/images+labels, then re-run.",
            file=sys.stderr,
        )
        sys.exit(1)

    out.mkdir(parents=True, exist_ok=True)
    (out / "train").mkdir(exist_ok=True)
    (out / "test").mkdir(exist_ok=True)

    print(f"Classes (id -> label): " + ", ".join(f"{i}:{n}" for i, n in enumerate(names)))

    kept = {"train": [0, 0], "test": [0, 0]}  # [annotated, boxless]

    for split, img_dir, lbl_dir, out_split in (
        ("train", train_imgs, train_lbls, "train"),
        ("test", val_imgs, val_lbls, "test"),  # Edge Impulse calls it 'test'
    ):
        if not img_dir.exists():
            continue
        with open(out / f"{split}.csv", "w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(["filename", "label", "x", "y", "width", "height"])

            for img_path in sorted(img_dir.glob("*.[jJ][pP][gG]")) + \
                             sorted(img_dir.glob("*.[pP][nN][gG]")):
                lbl_path = lbl_dir / (img_path.stem + ".txt")
                if not lbl_path.exists():
                    continue
                boxes = read_yolo_label(lbl_path)
                dest = out / out_split / img_path.name
                shutil.copyfile(img_path, dest)

                if not boxes:
                    # No boxes: Edge Impulse sees an image with no CSV row as
                    # a background/"no objects" sample, so it is a negative.
                    kept[split][1] += 1
                    continue

                for (cls_id, xc, yc, w, h) in boxes:
                    label = names[cls_id] if cls_id < len(names) else f"c{cls_id}"
                    # Edge Impulse bounding-box schema: x/y = top-left,
                    # width/height, all relative to the source image.
                    writer.writerow([
                        img_path.name, label,
                        round(max(0.0, xc - w / 2), 6),
                        round(max(0.0, yc - h / 2), 6),
                        round(min(1.0, w), 6),
                        round(min(1.0, h), 6),
                    ])
                kept[split][0] += 1

        zip_path = out / f"{split}.zip"
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
            for f in (out / out_split).glob("*"):
                z.write(f, f.name)

        print(f"[{split:5s}] annotated={kept[split][0]} negative(no-box)={kept[split][1]} "
              f"-> {zip_path} + {out / (split + '.csv')}")

    print()
    print("Upload to Edge Impulse (Create / Object Detection / FOMO):")
    for split in ("train", "test"):
        print(f"  1. {out / (split + '.zip')}")
        print(f"  2. {out / (split + '.csv')}  (label = column above)")


if __name__ == "__main__":
    main()