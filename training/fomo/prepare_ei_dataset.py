#!/usr/bin/env python3
"""
Build a leakage-free, free-tier-sized Edge Impulse dataset from the existing
Roboflow-derived data.

This script NEVER writes to the source dataset. It only reads
dataset/images/{train,val} + dataset/labels/{train,val} and writes a new,
self-contained package under training/fomo/out_ei/.

Why a repack instead of uploading dataset/ directly
--------------------------------------------------
1. Format. Our labels are already YOLOv8 normalized `class cx cy w h`, which is
   byte-identical to Edge Impulse's YOLO TXT spec. So the label files are copied
   verbatim - no coordinate math, no chance of corrupting a box in transit.

2. Data leakage. This is the reason the old split cannot be reused. Filenames
   encode the capture session (`IMG_20240322_161347_jpg.rf.<hash>.jpg` ->
   session `IMG_20240322`). 384 of the 488 old val images (78.7%) came from a
   session that also appears in train. Consecutive frames share camera position,
   pen and lighting, so a model can score well by memorising a session's
   background instead of learning what feces looks like. Splitting by whole
   session removes that entirely.

3. Free tier. The Developer plan caps compute at 60 min per job with no GPU, and
   caps nothing on sample count, so size is a training-time concern, not an
   upload concern. See training/fomo/README.md for the matching Studio settings.

Splits are assigned to whole sessions (70/15/15 by image count) with a fixed
seed, so the result is reproducible and re-running does not reshuffle anything.

Usage:
    python training/fomo/prepare_ei_dataset.py
    python training/fomo/prepare_ei_dataset.py --seed 42 --out-dir out_ei
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import os
import random
import re
import shutil
import sys
import zipfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

# Roboflow rewrites the original file extension into the name and appends a
# per-image hash: IMG_20240322_161347.jpg -> IMG_20240322_161347_jpg.rf.<32hex>.jpg
ROBOFLOW_RE = re.compile(r"^(?P<base>.+?)_(?:jpg|jpeg|png)\.rf\.(?P<hash>[0-9a-f]{32})\.jpg$",
                         re.IGNORECASE)
# A trailing 6-digit HHMMSS stamp marks a still frame from a video; everything
# before it is the capture session.
TIMESTAMP_RE = re.compile(r"[-_]\d{6}$")

CLASS_NAMES = ["feces", "pig"]  # must match training/dataset.yaml
SPLIT_TARGETS = {"train": 0.70, "val": 0.15, "test": 0.15}

# dHash distance (0-64) below which two images count as near-identical.
NEAR_DUP_DISTANCE = 2


class SourceError(RuntimeError):
    """Raised when the source dataset fails validation; nothing is written."""


# --------------------------------------------------------------------------
# source parsing
# --------------------------------------------------------------------------

def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parent.parent.parent
    p.add_argument("--dataset", type=Path, default=repo / "dataset")
    p.add_argument("--out-dir", type=Path, default=Path(__file__).resolve().parent / "out_ei")
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--near-dup-threshold", type=int, default=NEAR_DUP_DISTANCE,
                   help="dHash Hamming distance treated as a near-duplicate (0-64).")
    p.add_argument("--spotcheck-count", type=int, default=30)
    return p.parse_args()


def source_frame(path: Path) -> str:
    """Original frame name with the Roboflow suffix stripped."""
    m = ROBOFLOW_RE.match(path.name)
    return m.group("base") if m else path.stem


def session_of(path: Path) -> str:
    """Capture session for an image, e.g. 'IMG_20240322'."""
    return TIMESTAMP_RE.sub("", source_frame(path))


def read_label(path: Path) -> list[tuple[int, float, float, float, float]]:
    """Parse a YOLO label file into (cls, cx, cy, w, h) tuples."""
    out = []
    for lineno, raw in enumerate(path.read_text().splitlines(), 1):
        raw = raw.strip()
        if not raw:
            continue
        parts = raw.split()
        if len(parts) != 5:
            raise SourceError(f"{path}:{lineno}: expected 5 fields, got {len(parts)}: {raw!r}")
        try:
            cls = int(float(parts[0]))
            vals = [float(x) for x in parts[1:]]
        except ValueError as exc:
            raise SourceError(f"{path}:{lineno}: non-numeric field: {raw!r}") from exc
        out.append((cls, *vals))
    return out


def validate_image(img: Path, lbl: Path, boxes, n_classes: int) -> None:
    """Fail loudly rather than shipping a bad label to Edge Impulse."""
    for cls, cx, cy, w, h in boxes:
        if not 0 <= cls < n_classes:
            raise SourceError(f"{lbl}: class id {cls} outside 0..{n_classes - 1}")
        for name, v in (("cx", cx), ("cy", cy), ("w", w), ("h", h)):
            if not 0.0 <= v <= 1.0:
                raise SourceError(f"{lbl}: {name}={v} outside [0,1]")
        if w <= 0 or h <= 0:
            raise SourceError(f"{lbl}: non-positive box size w={w} h={h}")
        if cx - w / 2 < -1e-6 or cx + w / 2 > 1 + 1e-6:
            raise SourceError(f"{lbl}: box extends past the left/right edge")
        if cy - h / 2 < -1e-6 or cy + h / 2 > 1 + 1e-6:
            raise SourceError(f"{lbl}: box extends past the top/bottom edge")


def collect_source(dataset: Path, n_classes: int):
    """Read and validate every source image + label. Returns records."""
    records = []
    problems = []
    for split in ("train", "val"):
        img_dir = dataset / "images" / split
        lbl_dir = dataset / "labels" / split
        if not img_dir.is_dir():
            raise SourceError(f"missing {img_dir}")
        images = sorted(img_dir.glob("*.jpg")) + sorted(img_dir.glob("*.png"))
        for img in images:
            lbl = lbl_dir / (img.stem + ".txt")
            if not lbl.exists():
                problems.append(f"{img.name}: no label file")
                continue
            try:
                boxes = read_label(lbl)
                validate_image(img, lbl, boxes, n_classes)
            except SourceError as exc:
                problems.append(str(exc))
                continue
            records.append({
                "img": img,
                "lbl": lbl,
                "boxes": boxes,
                "src_split": split,
                "frame": source_frame(img),
                "session": session_of(img),
                "feces": sum(1 for b in boxes if b[0] == 0),
                "pig": sum(1 for b in boxes if b[0] == 1),
            })
    return records, problems


# --------------------------------------------------------------------------
# near-duplicate detection
# --------------------------------------------------------------------------

def dhash(path: Path, n: int = 8) -> np.ndarray:
    """64-bit difference hash, used to find near-identical frames."""
    with Image.open(path) as im:
        small = np.asarray(im.convert("L").resize((n + 1, n), Image.LANCZOS), dtype=np.int16)
    return (small[:, 1:] > small[:, :-1]).flatten()


def file_digest(path: Path) -> str:
    h = hashlib.md5()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# --------------------------------------------------------------------------
# splitting
# --------------------------------------------------------------------------

def group_split(records, seed: int, merged=None):
    """Assign whole capture sessions to train/val/test, hitting image targets.

    `merged` maps session -> super-session id. Sessions that share a
    near-duplicate image are merged first, so one physically distinct scene can
    never be split across two sets. Splitting after the union is what keeps both
    the session and near-duplicate guarantees intact.
    """
    by_session = collections.defaultdict(list)
    for r in records:
        by_session[r["session"]].append(r)

    super_of = merged or {s: s for s in by_session}
    by_super = collections.defaultdict(list)
    for s, recs in by_session.items():
        by_super[super_of.get(s, s)].extend(recs)

    total = len(records)
    supers = sorted(by_super)
    random.Random(seed).shuffle(supers)

    counts = collections.Counter()
    assignment = {}
    # Repeatedly give the next super-session to whichever split is furthest
    # below its image quota. Greedy, but stable given the fixed seed.
    for s in supers:
        size = len(by_super[s])
        split = min(SPLIT_TARGETS, key=lambda k: counts[k] - SPLIT_TARGETS[k] * total)
        assignment[s] = split
        counts[split] += size
    for r in records:
        r["split"] = assignment[super_of.get(r["session"], r["session"])]
    return assignment, by_super, counts


class UnionFind:
    def __init__(self):
        self.parent = {}

    def find(self, x):
        self.parent.setdefault(x, x)
        while self.parent[x] != x:
            self.parent[x] = self.parent[self.parent[x]]
            x = self.parent[x]
        return x

    def union(self, a, b):
        ra, rb = self.find(a), self.find(b)
        if ra != rb:
            # Keep the lexicographically smaller root so the result is stable.
            if rb < ra:
                ra, rb = rb, ra
            self.parent[rb] = ra


def find_near_duplicates(records, threshold: int):
    """Group records by dHash distance <= threshold. Returns list of groups."""
    hashes = {id(r): dhash(r["img"]) for r in records}
    buckets = collections.defaultdict(list)
    for r in records:
        buckets[tuple(hashes[id(r)])].append(r)

    # Only compare within a hash bucket plus a small band around it, otherwise
    # this is a full O(n^2) scan over 2956 x 64 comparisons for every pair.
    seen = set()
    groups = []
    keys = list(buckets)
    for i, k1 in enumerate(keys):
        for k2 in keys[i + 1:]:
            arr1 = np.array(k1, dtype=np.uint8)
            arr2 = np.array(k2, dtype=np.uint8)
            if int(np.count_nonzero(arr1 != arr2)) > threshold:
                continue
            members = buckets[k1] + buckets[k2]
            ids = frozenset(id(m) for m in members)
            if ids in seen:
                continue
            # Confirm with true Hamming distance between the two hashes.
            members = [m for m in members
                       if any(int(np.count_nonzero(hashes[id(m)] ^ h)) <= threshold
                              for h in (arr1, arr2))]
            ids = frozenset(id(m) for m in members)
            if ids in seen or len(members) < 2:
                seen.add(ids)
                continue
            seen.add(ids)
            groups.append(members)
    return groups


# --------------------------------------------------------------------------
# output
# --------------------------------------------------------------------------

def write_split_zip(records, out_dir: Path, split: str) -> dict:
    # Every archive uses the documented YOLO TXT layout (train/images +
    # train/labels + classes.txt). Edge Impulse decides the actual split from
    # the category picked in the uploader, not from the directory name, so the
    # same layout works unchanged for train.zip, val.zip and test.zip. No
    # data.yaml on purpose: classes.txt is sufficient and leaves Ultralytics'
    # server-side dataset.yaml as the single source of class configuration.
    stage = out_dir / "_stage"
    shutil.rmtree(stage, ignore_errors=True)
    (stage / "images").mkdir(parents=True, exist_ok=True)
    (stage / "labels").mkdir(parents=True, exist_ok=True)

    for r in records:
        # copyfile, not copy2: keeps the source mtime untouched on read.
        with open(r["img"], "rb") as src, open(stage / "images" / r["img"].name, "wb") as dst:
            dst.write(src.read())
        with open(r["lbl"], "rb") as src, open(stage / "labels" / r["lbl"].name, "wb") as dst:
            dst.write(src.read())

    with open(stage / "classes.txt", "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(CLASS_NAMES) + "\n")

    zip_path = out_dir / f"{split}.zip"
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for f in sorted((stage / "images").glob("*")):
            z.write(f, f"train/images/{f.name}")
        for f in sorted((stage / "labels").glob("*")):
            z.write(f, f"train/labels/{f.name}")
        z.write(stage / "classes.txt", "classes.txt")
    shutil.rmtree(stage, ignore_errors=True)

    return {
        "images": len(records),
        "feces": sum(r["feces"] for r in records),
        "pig": sum(r["pig"] for r in records),
        "negatives": sum(1 for r in records if not r["boxes"]),
        "pig_only": sum(1 for r in records if r["feces"] == 0 and r["pig"] > 0),
        "sessions": len({r["session"] for r in records}),
    }


def write_spotcheck(records, out_dir: Path, count: int) -> None:
    """Contact sheet with boxes drawn, so box placement can be eyeballed."""
    if count <= 0:
        return
    spot = out_dir / "spotcheck"
    spot.mkdir(parents=True, exist_ok=True)

    # Spread the sample across sessions rather than taking the first N.
    by_session = collections.defaultdict(list)
    for r in records:
        by_session[r["session"]].append(r)
    sessions = sorted(by_session)
    picks = []
    i = 0
    while len(picks) < count and i < 10000:
        s = sessions[i % len(sessions)]
        if by_session[s]:
            picks.append(by_session[s].pop(0))
        i += 1

    cols, cell, pad = 6, 256, 4
    rows = (len(picks) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (cell + pad), rows * (cell + pad + 18)), (18, 18, 20))
    draw = ImageDraw.Draw(sheet)
    colors = {0: (60, 220, 90), 1: (255, 170, 40)}

    for idx, r in enumerate(picks):
        with Image.open(r["img"]) as im:
            im = im.convert("RGB").resize((cell, cell), Image.LANCZOS)
        d = ImageDraw.Draw(im)
        for cls, cx, cy, w, h in r["boxes"]:
            x0, y0 = (cx - w / 2) * cell, (cy - h / 2) * cell
            x1, y1 = (cx + w / 2) * cell, (cy + h / 2) * cell
            d.rectangle([x0, y0, x1, y1], outline=colors.get(cls, (255, 0, 255)), width=2)
        col, row = idx % cols, idx // cols
        ox, oy = col * (cell + pad), row * (cell + pad + 18)
        sheet.paste(im, (ox, oy))
        tag = f"{r['img'].name[:26]}"
        counts = f"f{r['feces']}/p{r['pig']}"
        draw.text((ox + 2, oy + cell + 3), f"{tag} [{counts}]", fill=(200, 200, 200))
        with open(spot / f"{idx:02d}_{r['img'].name}.txt", "w", encoding="utf-8") as fh:
            fh.write(f"image: {r['img'].name}\nsession: {r['session']}\n"
                     f"source split: {r['src_split']}\nnew split: {r['split']}\n"
                     f"boxes: {len(r['boxes'])} (feces={r['feces']}, pig={r['pig']})\n")
    sheet.save(spot / "contact_sheet.png")
    print(f"  spotcheck  : {len(picks)} images -> {spot / 'contact_sheet.png'}")


# --------------------------------------------------------------------------

def main():
    args = parse_args()
    dataset, out_dir = args.dataset, args.out_dir
    n_classes = len(CLASS_NAMES)

    if not (dataset / "images").is_dir():
        print(f"ERROR: {dataset}/images not found", file=sys.stderr)
        return 1

    print("Edge Impulse dataset builder (source is read-only)")
    print(f"  source : {dataset}")
    print(f"  output : {out_dir}")
    print(f"  classes: {CLASS_NAMES}")
    print(f"  seed   : {args.seed}")
    print()

    print("[1/7] reading and validating source labels")
    records, problems = collect_source(dataset, n_classes)
    if problems:
        print(f"  ERROR: {len(problems)} problem(s) in the source dataset:", file=sys.stderr)
        for p in problems[:20]:
            print(f"    {p}", file=sys.stderr)
        print("  Refusing to write a package built on unvalidated labels.", file=sys.stderr)
        return 1
    if not records:
        print("  ERROR: no valid images found", file=sys.stderr)
        return 1
    print(f"  {len(records)} images validated, "
          f"{sum(len(r['boxes']) for r in records)} boxes, 0 malformed")
    missing_ann = sum(1 for r in records if not r["boxes"])
    print(f"  images with no annotation at all (true negatives): {missing_ann}")
    print(f"  pig-only images (natural negatives): {sum(1 for r in records if r['feces'] == 0 and r['pig'] > 0)}")
    print()

    print("[2/7] checking for byte-identical duplicates")
    digests = collections.defaultdict(list)
    for r in records:
        digests[file_digest(r["img"])].append(r)
    dup_groups = {k: v for k, v in digests.items() if len(v) > 1}
    print(f"  byte-identical duplicate groups: {len(dup_groups)}")
    for k, v in list(dup_groups.items())[:3]:
        splits = {r["split"] if "split" in r else "?" for r in v}
        print(f"    {len(v)} copies: {[r['img'].name[:40] for r in v]} splits={splits}")
    print()

    print("[3/7] grouping by capture session")
    by_session = collections.defaultdict(list)
    for r in records:
        by_session[r["session"]].append(r)
    print(f"  distinct sessions: {len(by_session)}")
    print(f"  largest sessions : {sorted(((len(v), k) for k, v in by_session.items()), reverse=True)[:5]}")
    print()

    print("[4/7] scanning for near-duplicates (dHash)")
    groups = find_near_duplicates(records, args.near_dup_threshold)
    print(f"  near-duplicate clusters: {len(groups)}")
    # Roboflow can produce visually identical frames under different session
    # names (e.g. IMG_20240722_155135 vs photo_2024-08-02_16-51-00), so grouping
    # by session name alone is not sufficient. Union every session that contains
    # a member of a near-duplicate cluster, then split the merged units.
    uf = UnionFind()
    for r in records:
        uf.find(r["session"])
    cross_session = 0
    for g in groups:
        sessions = sorted({r["session"] for r in g})
        if len(sessions) > 1:
            cross_session += 1
        for s in sessions[1:]:
            uf.union(sessions[0], s)
    merged = {r["session"]: uf.find(r["session"]) for r in records}
    n_super = len(set(merged.values()))
    print(f"  clusters spanning >1 session name: {cross_session}")
    print(f"  sessions: {len({r['session'] for r in records})} -> {n_super} merged units")
    print()

    print("[5/7] assigning merged units to train/val/test")
    assignment, by_super, counts = group_split(records, args.seed, merged)
    total = len(records)
    for split in ("train", "val", "test"):
        print(f"  {split:5s}: {counts[split]:5d} images ({100*counts[split]/total:5.1f}%) "
              f"target {100*SPLIT_TARGETS[split]:.0f}%")

    # Hard guarantees: verify on the final assignment, not on the inputs.
    train_sessions = {r["session"] for r in records if r["split"] == "train"}
    ok = True
    for split in ("val", "test"):
        ss = {r["session"] for r in records if r["split"] == split}
        overlap = ss & train_sessions
        print(f"  {split} sessions overlapping train: {len(overlap)} (must be 0)")
        if overlap:
            print(f"    ERROR: {sorted(overlap)[:5]}", file=sys.stderr)
            ok = False
    leaking = [g for g in groups if len({r["split"] for r in g}) > 1]
    print(f"  near-duplicate clusters spanning >1 split: {len(leaking)} (must be 0)")
    if leaking:
        ok = False
    if not ok:
        print("  Refusing to write a package that would leak across splits.", file=sys.stderr)
        return 1
    print()

    print("[6/7] writing zips")
    out_dir.mkdir(parents=True, exist_ok=True)
    summary = {}
    for split in ("train", "val", "test"):
        sel = [r for r in records if r["split"] == split]
        summary[split] = write_split_zip(sel, out_dir, split)
        s = summary[split]
        print(f"  {split}.zip  images={s['images']:5d}  feces={s['feces']:6d}  pig={s['pig']:5d}  "
              f"negatives={s['negatives']:2d}  pig-only={s['pig_only']:2d}  sessions={s['sessions']}")
    print()

    print("[7/7] manifest + spot-check sheet")
    write_spotcheck(records, out_dir, args.spotcheck_count)

    tf = sum(s["feces"] for s in summary.values())
    tp = sum(s["pig"] for s in summary.values())
    src_feces = sum(r["feces"] for r in records)
    src_pig = sum(r["pig"] for r in records)
    if (tf, tp) != (src_feces, src_pig):
        print(f"  ERROR: box totals changed: source ({src_feces},{src_pig}) vs output ({tf},{tp})",
              file=sys.stderr)
        return 1
    print(f"  box totals preserved: feces={tf} pig={tp} (matches source)")

    with open(out_dir / "MANIFEST.txt", "w", encoding="utf-8", newline="\n") as fh:
        fh.write("Edge Impulse dataset manifest\n")
        fh.write("=" * 60 + "\n\n")
        fh.write(f"source   : {dataset}\n")
        fh.write(f"seed     : {args.seed}\n")
        fh.write(f"classes  : {CLASS_NAMES} (0=feces, 1=pig)\n")
        fh.write("format   : YOLO TXT (labels copied verbatim, no conversion)\n")
        fh.write(f"sessions : {len({r['session'] for r in records})} capture sessions, "
                 f"merged into {n_super} leak-free units, split as whole units\n")
        fh.write(f"near-dup : dHash threshold {args.near_dup_threshold}, "
                 f"{len(groups)} clusters, {cross_session} spanning >1 session name\n\n")
        for split in ("train", "val", "test"):
            s = summary[split]
            fh.write(f"[{split}] images={s['images']} feces={s['feces']} pig={s['pig']} "
                     f"negatives={s['negatives']} pig-only={s['pig_only']} sessions={s['sessions']}\n")
        fh.write(f"\nTOTAL images={sum(s['images'] for s in summary.values())} "
                 f"feces={tf} pig={tp}\n\n")
        fh.write("leak-free units per split:\n")
        for split in ("train", "val", "test"):
            ss = sorted(s for s, sp in assignment.items() if sp == split)
            fh.write(f"  {split} ({len(ss)}): {', '.join(ss)}\n")
    print(f"  manifest -> {out_dir / 'MANIFEST.txt'}")
    print()
    print("Upload order: train.zip (training), val.zip (validation), test.zip (test).")
    print("Format: YOLO TXT. Each zip carries classes.txt; pick the category in the uploader.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
