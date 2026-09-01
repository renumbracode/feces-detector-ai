#!/usr/bin/env python3
"""
Convert a Roboflow pig-feces YOLOv8 export into the project's single-class
dataset layout.

The Roboflow model (irvanprama/pig-feces) has TWO classes:
    class 0 = feces   (the target for this project)
    class 1 = pig      (context/negative only)

The project pipeline (training/dataset.yaml, FOMO labels) is single-class
'feces'. This script strips the 'pig' boxes, keeps only 'feces' (renumbered
to class 0), and drops any image left with zero feces boxes.

Merging rule (see docs/data-training-runbook.md):
    repo dataset/images|labels/train  <- Roboflow train + valid
    repo dataset/images|labels/val    <- Roboflow test (TEMP held-out)
                                          Swap in your OWN ESP32-S3 camera
                                          frames later for an honest val set.

Usage:
    python convert_roboflow_pigfeces.py \
        --src "C:/tmp/pigfeces_v1" \
        --dst "<project>/dataset"

Requires: Pillow (or just shutil for the images we keep).
"""
from __future__ import annotations

import argparse
import shutil
from pathlib import Path

# Class name -> keep? class 0 = feces (target), class 1 = pig (discard).
FECES_CLASS = 0


def read_boxes(txt: Path):
    """Return list of (cls, xc, yc, w, h) as floats, skipping bad rows."""
    out = []
    for line in txt.read_text().splitlines():
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


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--src", type=Path, required=True,
                   help="Roboflow YOLOv8 export dir (train/valid/test + images/labels).")
    p.add_argument("--dst", type=Path, required=True,
                   help="Repo dataset dir (with images/{train,val} labels/{train,val}).")
    p.add_argument("--val-source", default="test",
                   choices=["test", "valid"],
                   help="Which Roboflow split becomes the repo val set (default: test).")
    args = p.parse_args()

    src = args.src
    dst = args.dst

    imgs = dst / "images"
    lbls = dst / "labels"
    for split in ("train", "val"):
        (imgs / split).mkdir(parents=True, exist_ok=True)
        (lbls / split).mkdir(parents=True, exist_ok=True)

    # (split_name_in_repo, roboflow_source_split)
    jobs = [("train", "train"), ("train", "valid"), ("val", args.val_source)]

    kept_total = 0
    dropped_total = 0
    for dst_split, src_split in jobs:
        src_img_dir = src / src_split / "images"
        src_lbl_dir = src / src_split / "labels"
        if not src_img_dir.exists():
            print(f"[skip] no {src_split}/images at {src_img_dir}")
            continue

        kept = 0
        for img_path in sorted(src_img_dir.iterdir()):
            if img_path.suffix.lower() not in (".jpg", ".jpeg", ".png"):
                continue
            lbl_path = src_lbl_dir / (img_path.stem + ".txt")
            if not lbl_path.exists():
                continue

            boxes = read_boxes(lbl_path)
            # Keep only feces rows.
            feces = [b for b in boxes if b[0] == FECES_CLASS]
            if not feces:
                dropped_total += 1
                continue

            # Renumber to class 0 (they already are 0, but be explicit).
            new_lines = "".join(f"0 {xc} {yc} {w} {h}\n"
                                for (_, xc, yc, w, h) in feces)

            shutil.copyfile(img_path, imgs / dst_split / img_path.name)
            (lbls / dst_split / (img_path.stem + ".txt")).write_text(new_lines)
            kept += 1

        kept_total += kept
        print(f"[{src_split:6s} -> {dst_split:5s}] kept {kept}")

    print(f"\nTOTAL kept={kept_total}, dropped (no feces)={dropped_total}")
    print(f"Repo dataset written to {dst}")


if __name__ == "__main__":
    main()