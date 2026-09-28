#!/usr/bin/env python3
"""
Convert a Roboflow pig-feces YOLOv8 export into the project's dataset layout.

The Roboflow project (brent-jim-tingzon/pig-feces-bblfq v1) annotates TWO
classes:
    class 0 = feces   (the target for this project)
    class 1 = pig      (context, and the reason we do not false-positive)

The project pipeline is 2-class end to end (training/dataset.yaml, the FOMO
export, and the verify server's `uclass == "feces"` check), so the default here
KEEPS the pig boxes. An earlier version of this script stripped them, which
forced the network to learn "pig body = background" from context alone and left
it free to fire on pigs -- the single biggest source of false-positive sprays.

Merging rule (see docs/data-training-runbook.md):
    repo dataset/images|labels/train  <- Roboflow train + valid
    repo dataset/images|labels/val    <- Roboflow test (TEMP held-out)
                                          Swap in your OWN ESP32-S3 camera
                                          frames later for an honest val set.

Images with no feces box are KEPT as negatives (empty label file) rather than
skipped, so the model is explicitly shown "pig, no feces -> detect nothing".

Usage:
    python convert_roboflow_pigfeces.py --src "C:/tmp/pigfeces_v1" --dst dataset
    python convert_roboflow_pigfeces.py --src ... --dst ... --classes feces
"""
from __future__ import annotations

import argparse
import shutil
from collections import Counter
from pathlib import Path

FECES_CLASS = 0
PIG_CLASS = 1
CLASS_NAMES = {FECES_CLASS: "feces", PIG_CLASS: "pig"}


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


def iou(a, b) -> float:
    """IoU of two (xc, yc, w, h) boxes in normalized coords."""
    ax1, ay1, ax2, ay2 = a[0] - a[2] / 2, a[1] - a[3] / 2, a[0] + a[2] / 2, a[1] + a[3] / 2
    bx1, by1, bx2, by2 = b[0] - b[2] / 2, b[1] - b[3] / 2, b[0] + b[2] / 2, b[1] + b[3] / 2
    ix1, iy1 = max(ax1, bx1), max(ay1, by1)
    ix2, iy2 = min(ax2, bx2), min(ay2, by2)
    iw, ih = max(0.0, ix2 - ix1), max(0.0, iy2 - iy1)
    inter = iw * ih
    union = a[2] * a[3] + b[2] * b[3] - inter
    return inter / union if union > 0 else 0.0


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--src", type=Path, required=True,
                   help="Roboflow YOLOv8 export dir (train/valid/test + images/labels).")
    p.add_argument("--dst", type=Path, required=True,
                   help="Repo dataset dir (with images/{train,val} labels/{train,val}).")
    p.add_argument("--val-source", default="test", choices=["test", "valid"],
                   help="Which Roboflow split becomes the repo val set (default: test).")
    p.add_argument("--classes", default="both", choices=["both", "feces"],
                   help="'both' keeps pig boxes as class 1 (default). "
                        "'feces' reproduces the old single-class set.")
    p.add_argument("--drop-negatives", action="store_true",
                   help="Skip images that have no feces box instead of keeping "
                        "them as empty-label negatives.")
    p.add_argument("--clean", action="store_true",
                   help="Empty the destination image/label dirs first so stale "
                        "frames from a previous conversion cannot linger.")
    args = p.parse_args()

    keep_pig = args.classes == "both"
    src, dst = args.src, args.dst

    imgs = dst / "images"
    lbls = dst / "labels"
    for split in ("train", "val"):
        for d in (imgs / split, lbls / split):
            d.mkdir(parents=True, exist_ok=True)
            if args.clean:
                for f in d.iterdir():
                    if f.name != ".gitkeep":
                        f.unlink()

    # (split_name_in_repo, roboflow_source_split)
    jobs = [("train", "train"), ("train", "valid"), ("val", args.val_source)]

    stats = Counter()
    class_boxes = Counter()
    collisions = 0
    collision_images = set()

    for dst_split, src_split in jobs:
        src_img_dir = src / src_split / "images"
        src_lbl_dir = src / src_split / "labels"
        if not src_img_dir.exists():
            print(f"[skip] no {src_split}/images at {src_img_dir}")
            continue

        kept = neg = 0
        for img_path in sorted(src_img_dir.iterdir()):
            if img_path.suffix.lower() not in (".jpg", ".jpeg", ".png"):
                continue
            lbl_path = src_lbl_dir / (img_path.stem + ".txt")
            if not lbl_path.exists():
                continue

            boxes = read_boxes(lbl_path)
            feces = [b for b in boxes if b[0] == FECES_CLASS]
            pigs = [b for b in boxes if b[0] == PIG_CLASS]

            # Measure annotation conflict before we decide what to keep.
            for f in feces:
                if any(iou(f[1:], g[1:]) > 0.5 for g in pigs):
                    collisions += 1
                    collision_images.add(img_path.name)
                    break

            # A frame with no feces box is a hard negative when it still shows a
            # pig: that is exactly the "pig present, do not spray" case. Only a
            # frame with no annotations at all is a true negative.
            if not feces and args.drop_negatives:
                stats["dropped_no_feces"] += 1
                continue

            rows = list(feces)
            if keep_pig:
                # Pigs keep their class id (1); feces stay class 0.
                rows += pigs
            if not rows:
                # True negative: an empty label file is a valid YOLO background
                # sample, so this is still written rather than skipped.
                stats["true_negatives"] += 1

            shutil.copyfile(img_path, imgs / dst_split / img_path.name)
            (lbls / dst_split / (img_path.stem + ".txt")).write_text(
                "".join(f"{c} {xc} {yc} {w} {h}\n" for (c, xc, yc, w, h) in rows))
            for (c, *_rest) in rows:
                class_boxes[c] += 1

            if feces:
                kept += 1
                stats["positives"] += 1
            elif rows:
                neg += 1
                stats["hard_negatives"] += 1

        print(f"[{src_split:6s} -> {dst_split:5s}] pos/hard-neg {kept}/{neg}")

    total = stats["positives"] + stats["hard_negatives"] + stats["true_negatives"]
    print()
    print(f"TOTAL images written  : {total}")
    print(f"  with feces boxes    : {stats['positives']}")
    print(f"  hard negatives (pig, no feces, box kept): {stats['hard_negatives']}")
    print(f"  true negatives (0 boxes, empty label)  : {stats['true_negatives']}")
    if stats["dropped_no_feces"]:
        print(f"  dropped (--drop-negatives)             : {stats['dropped_no_feces']}")
    print(f"Box counts by class  : "
          + ", ".join(f"{CLASS_NAMES.get(c, c)}={n}" for c, n in sorted(class_boxes.items())))
    print(f"Dropped pig boxes    : {0 if keep_pig else class_boxes[PIG_CLASS]}")
    if collision_images:
        pct = 100.0 * collisions / max(1, total)
        print(f"Annotation conflicts : {collisions} images ({pct:.1f}%) have a feces box "
              f"overlapping a pig box >50% IoU -- review before trusting a score.")
    print(f"\nRepo dataset written to {dst}")


if __name__ == "__main__":
    main()
