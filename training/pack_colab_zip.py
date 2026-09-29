#!/usr/bin/env python3
"""
Repack dataset/ into dataset/pigfeces-yolo.zip for the Colab YOLOv8 notebook.

The notebook (docs/train_yolov8_colab.ipynb) expects a zip that unzips to the
standard Ultralytics layout:

    images/train/*.jpg  labels/train/*.txt
    images/val/*.jpg    labels/val/*.txt

and it is trained against training/dataset.yaml, which declares BOTH classes
(feces = 0, pig = 1). The previous zip in the repo predated the two-class
conversion and contained only class 0, so the pig class was silently
unsupervised: training completed without error and produced a model that never
learned to reject a pig.

This script rebuilds the zip from the live dataset and prints per-class box
counts so a single-class regression is visible immediately.

Usage:
    python training/pack_colab_zip.py [--dataset dataset] [--out dataset/pigfeces-yolo.zip]
"""
from __future__ import annotations

import argparse
import sys
import zipfile
from pathlib import Path


def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dataset", type=Path, default=None,
                   help="Repo dataset dir with images/{train,val} labels/{train,val}. "
                        "Defaults to <repo>/dataset.")
    p.add_argument("--out", type=Path, default=None,
                   help="Output zip. Defaults to <repo>/dataset/pigfeces-yolo.zip.")
    p.add_argument("--names", default="feces,pig",
                   help="Comma-separated class names in id order. Defaults to the "
                        "two-class set from training/dataset.yaml.")
    return p.parse_args()


def count_classes(label_path: Path, n_classes: int) -> tuple[int, list[int], set[int]]:
    """Return (box_count, per_class_counts, out_of_range_ids) for one label file."""
    per_class = [0] * n_classes
    bad: set[int] = set()
    total = 0
    for line in label_path.read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) < 5:
            continue
        try:
            cls = int(float(parts[0]))
        except ValueError:
            continue
        if 0 <= cls < n_classes:
            per_class[cls] += 1
        else:
            bad.add(cls)
        total += 1
    return total, per_class, bad


def main():
    args = parse_args()
    repo_root = Path(__file__).resolve().parent.parent
    dataset = args.dataset if args.dataset else repo_root / "dataset"
    out_path = args.out if args.out else repo_root / "dataset" / "pigfeces-yolo.zip"
    names = [n.strip() for n in args.names.split(",") if n.strip()]

    if not dataset.exists():
        print(f"ERROR: dataset dir not found: {dataset}", file=sys.stderr)
        sys.exit(1)

    per_class_total = [0] * len(names)
    grand_total = 0
    bad_ids: set[int] = set()
    counts: dict[str, tuple[int, int]] = {}

    for split in ("train", "val"):
        img_dir = dataset / "images" / split
        lbl_dir = dataset / "labels" / split
        if not img_dir.exists():
            print(f"ERROR: missing {img_dir}", file=sys.stderr)
            sys.exit(1)
        images = sorted(img_dir.glob("*.[jJ][pP][gG]")) + sorted(img_dir.glob("*.[pP][nN][gG]"))
        n_boxes = 0
        for img in images:
            lbl = lbl_dir / (img.stem + ".txt")
            if not lbl.exists():
                bad_ids.add(-1)
                print(f"WARNING: no label for {img.name}", file=sys.stderr)
                continue
            total, per_class, bad = count_classes(lbl, len(names))
            n_boxes += total
            bad_ids |= bad
            for i in range(len(names)):
                per_class_total[i] += per_class[i]
        grand_total += n_boxes
        counts[split] = (len(images), n_boxes)

    if bad_ids:
        print(f"ERROR: label ids out of range {sorted(bad_ids)} (names={names}). "
              f"Refusing to write a zip the notebook would train incorrectly.",
              file=sys.stderr)
        sys.exit(1)

    # Build the zip: images/ and labels/ at the root, matching the notebook.
    written = 0
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as z:
        for split in ("train", "val"):
            for img in sorted((dataset / "images" / split).glob("*.[jJ][pP][gG]")) + \
                    sorted((dataset / "images" / split).glob("*.[pP][nN][gG]")):
                z.write(img, f"images/{split}/{img.name}")
                written += 1
            for lbl in sorted((dataset / "labels" / split).glob("*.txt")):
                z.write(lbl, f"labels/{split}/{lbl.name}")

    print(f"Wrote {out_path}")
    print(f"  images: {written}")
    for split, (nimg, nbox) in counts.items():
        print(f"  {split:5s}: {nimg} images, {nbox} boxes")
    print(f"  total boxes: {grand_total}")
    for i, name in enumerate(names):
        print(f"    class {i} ({name}): {per_class_total[i]}")

    if any(c == 0 for c in per_class_total):
        print("ERROR: a class has zero boxes - this would train a single-class model.",
              file=sys.stderr)
        sys.exit(1)
    print("  OK: both classes present.")


if __name__ == "__main__":
    main()
