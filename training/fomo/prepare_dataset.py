#!/usr/bin/env python3
"""
Prepare the YOLO-format dataset for Edge Impulse FOMO training.

FOMO (Faster Objects, More Objects) detects objects as small per-cell
scores rather than full bounding boxes. Edge Impulse expects a labelled
image dataset; this script packs ours into the two formats its uploader
accepts.

Usage:
    python prepare_dataset.py [--format yolo-txt] [--out-dir out_fomo]
                              [--dataset ../dataset] [--target-size 160]

Formats
-------
`yolo-txt` (default, recommended)
    A straight repack. Edge Impulse's YOLO TXT layout is:
        classes.txt
        data.yaml
        train/images/*.jpg  train/labels/*.txt
        test/images/*.jpg   test/labels/*.txt
    with each label line `class_id center_x center_y width height`,
    normalized to [0,1]. That is already exactly the format our Roboflow
    labels are in, so the files are copied byte-for-byte -- no coordinate
    math, nothing to mis-map. Pick this format in the Studio uploader.

`csv`
    The older "Plain CSV" route. Writes one row per box with
    `filename,label,x,y,width,height` plus a flat image ZIP. Note this is
    NOT the Plain CSV schema Edge Impulse documents (that one wants
    `file_name,classes,xmin,ymin,xmax,ymax` with absolute pixels), so it
    only works through the CSV Wizard's manual column mapping. Kept for
    reproducing earlier runs; prefer `yolo-txt`.

Source data
-----------
Images live in dataset/images/{train,val} with YOLO labels in
dataset/labels/{train,val}. The dataset is 2-class (feces = 0, pig = 1);
class names are resolved from training/dataset.yaml. The repo split maps
`val` -> Edge Impulse `test`.

Every zip gets a `classes.txt` (and `data.yaml` for yolo-txt) written from
the resolved names, so the uploader can name the classes rather than
showing "0" and "1". The script prints per-class box counts and hard-fails
if a class id is out of range, so a single-class zip can never again be
silently trained against a 2-class configuration.

FOMO-specific notes:
  * Edge Impulse preprocesses to the model's input size itself and rescales
    the boxes, so images are uploaded at native resolution and coordinates
    stay normalized to the original image (they already are).
  * 160x160 is the intended input. It yields a 20x20 FOMO heat map, and the
    10th-percentile feces blob in this dataset is ~5px at 96x96 but ~8.8px at
    160x160. FOMO learns centroids per cell, so blobs much smaller than one
    cell are hard to fit. 96x96 gives a 12x12 map and ~5px blobs.
  * Set the learning rate to 0.001 in the FOMO learning block.
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
    p.add_argument("--dataset", type=Path, default=None,
                   help="Repo dataset dir with images/{train,val} labels/{train,val}. "
                        "Defaults to <repo>/dataset, resolved from this script's "
                        "location so it works from any cwd.")
    p.add_argument("--out-dir", type=Path, default=Path("out_fomo"),
                   help="Where the packed zips are written.")
    p.add_argument("--format", choices=("yolo-txt", "csv"), default="yolo-txt",
                   help="Edge Impulse uploader format. yolo-txt is a lossless "
                        "repack; csv needs the CSV Wizard column mapping.")
    p.add_argument("--target-size", type=int, default=160,
                   help="FOMO input size the Edge Impulse model will be trained "
                        "at. Informational: Edge Impulse resizes and rescales "
                        "boxes internally, so images are uploaded unmodified.")
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


def find_images(d: Path) -> list[Path]:
    """All jpg/png images in d, sorted for deterministic zips."""
    return sorted(d.glob("*.[jJ][pP][gG]")) + sorted(d.glob("*.[pP][nN][gG]"))


def data_yaml_text(names: list[str]) -> str:
    """data.yaml contents; the uploader reads class names from this or classes.txt."""
    lines = [
        "path: .",
        "train: train/images",
        "val: test/images",
        f"nc: {len(names)}",
        "names: [" + ", ".join(f"'{n}'" for n in names) + "]",
    ]
    return "\n".join(lines) + "\n"


def pack_yolo_txt(args, names, splits) -> None:
    """Repack dataset/ into Edge Impulse's YOLO TXT layout, one zip per split.

    Labels are copied byte-for-byte: our YOLO txt is already
    `class_id cx cy w h` normalized to [0,1], which is exactly the Edge Impulse
    YOLO TXT schema. Each zip carries its own classes.txt + data.yaml so the
    uploader resolves class names per upload.
    """
    out = args.out_dir
    out.mkdir(parents=True, exist_ok=True)

    for split, img_dir, lbl_dir in splits:
        images = find_images(img_dir)
        if not images:
            print(f"[{split}] no images in {img_dir}, skipping", file=sys.stderr)
            continue

        stage = out / split
        (stage / "images").mkdir(parents=True, exist_ok=True)
        (stage / "labels").mkdir(parents=True, exist_ok=True)

        per_class = {i: 0 for i in range(len(names))}
        annotated = boxless = boxes_total = 0
        bad_ids = set()
        missing_lbl = 0

        for img in images:
            lbl = lbl_dir / (img.stem + ".txt")
            if not lbl.exists():
                missing_lbl += 1
                continue
            shutil.copyfile(img, stage / "images" / img.name)
            shutil.copyfile(lbl, stage / "labels" / (img.stem + ".txt"))

            boxes = read_yolo_label(lbl)
            if not boxes:
                # Empty label file = a genuine negative sample. Copied as-is so
                # Edge Impulse sees it as "image with no objects".
                boxless += 1
                continue
            annotated += 1
            for (cls_id, _xc, _yc, _w, _h) in boxes:
                if cls_id not in per_class:
                    bad_ids.add(cls_id)
                else:
                    per_class[cls_id] += 1
                boxes_total += 1

        if bad_ids:
            print(
                f"ERROR: {split} label files reference class ids "
                f"{sorted(bad_ids)} outside the {len(names)} classes in "
                f"dataset.yaml ({names}). Refusing to write a zip Edge Impulse "
                f"would mis-map.",
                file=sys.stderr,
            )
            sys.exit(1)

        # classes.txt / data.yaml at the zip root, per the EI YOLO TXT layout.
        # newline="\n" so the uploader does not read a trailing "\r" as part of
        # the class name on Windows.
        with open(stage / "classes.txt", "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(names) + "\n")
        with open(stage / "data.yaml", "w", encoding="utf-8", newline="\n") as fh:
            fh.write(data_yaml_text(names))

        zip_path = out / f"{split}.zip"
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
            for f in sorted((stage / "images").glob("*")):
                z.write(f, f"train/images/{f.name}" if split == "train"
                        else f"test/images/{f.name}")
            for f in sorted((stage / "labels").glob("*")):
                z.write(f, f"train/labels/{f.name}" if split == "train"
                        else f"test/labels/{f.name}")
            z.write(stage / "classes.txt", "classes.txt")
            z.write(stage / "data.yaml", "data.yaml")

        counts = ", ".join(f"{names[i]}={per_class[i]}" for i in range(len(names)))
        print(f"[{split:5s}] images={len(images) - missing_lbl} "
              f"annotated={annotated} negative(no-box)={boxless} "
              f"boxes={boxes_total} ({counts}) -> {zip_path}")
        if missing_lbl:
            print(f"       note: {missing_lbl} image(s) had no label file and were skipped")


def pack_csv(args, names, splits) -> None:
    """Legacy Plain-CSV-ish export. Needs the CSV Wizard column mapping."""
    out = args.out_dir
    out.mkdir(parents=True, exist_ok=True)
    for split, img_dir, lbl_dir in splits:
        flat = out / split
        flat.mkdir(parents=True, exist_ok=True)
        per_class = {i: 0 for i in range(len(names))}
        annotated = boxless = 0
        bad_ids = set()

        with open(out / f"{split}.csv", "w", newline="") as f:
            writer = csv.writer(f, lineterminator="\n")
            writer.writerow(["filename", "label", "x", "y", "width", "height"])
            for img in find_images(img_dir):
                lbl = lbl_dir / (img.stem + ".txt")
                if not lbl.exists():
                    continue
                shutil.copyfile(img, flat / img.name)
                boxes = read_yolo_label(lbl)
                if not boxes:
                    boxless += 1
                    continue
                annotated += 1
                for (cls_id, xc, yc, w, h) in boxes:
                    if cls_id not in per_class:
                        bad_ids.add(cls_id)
                        continue
                    per_class[cls_id] += 1
                    writer.writerow([
                        img.name, names[cls_id],
                        round(max(0.0, xc - w / 2), 6), round(max(0.0, yc - h / 2), 6),
                        round(min(1.0, w), 6), round(min(1.0, h), 6),
                    ])

        if bad_ids:
            print(f"ERROR: {split} references class ids {sorted(bad_ids)} "
                  f"not in {names}", file=sys.stderr)
            sys.exit(1)

        zip_path = out / f"{split}.zip"
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
            for f in flat.glob("*"):
                z.write(f, f.name)
        counts = ", ".join(f"{names[i]}={per_class[i]}" for i in range(len(names)))
        print(f"[{split:5s}] annotated={annotated} negative={boxless} "
              f"({counts}) -> {zip_path} + {out / (split + '.csv')}")


def main():
    args = parse_args()
    # training/fomo/prepare_dataset.py -> repo root is two levels up.
    repo_root = Path(__file__).resolve().parent.parent.parent
    dataset = args.dataset if args.dataset else repo_root / "dataset"
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

    print(f"Classes (id -> label): " + ", ".join(f"{i}:{n}" for i, n in enumerate(names)))
    print(f"Format: {args.format}   FOMO input target: {args.target_size}")
    print()

    # repo `val` maps to Edge Impulse `test`
    splits = (
        ("train", train_imgs, train_lbls),
        ("test", val_imgs, val_lbls),
    )

    if args.format == "yolo-txt":
        pack_yolo_txt(args, names, splits)
    else:
        pack_csv(args, names, splits)

    print()
    if args.format == "yolo-txt":
        print("Edge Impulse upload (labeling method: Bounding boxes):")
        print(f"  1. {out / 'train.zip'}  -> format 'YOLO TXT', category 'training'")
        print(f"  2. {out / 'test.zip'}   -> format 'YOLO TXT', category 'testing'")
        print("  Both zips already contain classes.txt + data.yaml; no CSV Wizard.")
        print("  Confirm both classes appear on the Data acquisition page after upload.")
    else:
        print("Edge Impulse upload (Plain CSV - map columns in the CSV Wizard):")
        for split in ("train", "test"):
            print(f"  1. {out / (split + '.zip')}")
            print(f"  2. {out / (split + '.csv')}")
    print()
    print(f"Impulse: Image {args.target_size}x{args.target_size} RGB -> "
          f"Object Detection (FOMO-MobileNetV2 0.35), learning rate 0.001")


if __name__ == "__main__":
    main()