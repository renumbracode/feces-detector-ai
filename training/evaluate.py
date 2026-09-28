#!/usr/bin/env python3
"""
Evaluate a trained YOLOv8 model for pig feces detection.

Reports mAP50, mAP50-95, precision, recall, and F1 score.
Validates whether the model meets the 80% accuracy target.

Usage:
    python evaluate.py
    python evaluate.py --model training/runs/detect/weights/best.pt
    python evaluate.py --model best.pt --data training/dataset.yaml --target 0.80

Exit codes:
    0  — model meets or exceeds target mAP
    1  — model below target or error
"""

import argparse
import csv
import sys
from pathlib import Path

from ultralytics import YOLO


def print_metric(name, value, threshold=None):
    pct = f"{value * 100:.1f}%"
    status = ""
    if threshold is not None:
        status = " [PASS]" if value >= threshold else " [FAIL]"
    print(f"  {name:<20} {pct}{status}")


def main():
    parser = argparse.ArgumentParser(description="Evaluate YOLOv8 pig feces detection model.")
    parser.add_argument(
        "--model",
        default=str(Path(__file__).parent / "runs" / "detect" / "weights" / "best.pt"),
        help="Path to trained model weights.",
    )
    parser.add_argument(
        "--data",
        default=str(Path(__file__).parent / "dataset.yaml"),
        help="Path to dataset.yaml.",
    )
    parser.add_argument(
        "--target",
        type=float,
        default=0.80,
        help="Minimum mAP50 target (default: 0.80 = 80%%).",
    )
    parser.add_argument(
        "--imgsz",
        type=int,
        default=640,
        help="Image size for evaluation (default: 640).",
    )
    parser.add_argument("--device", default="", help="Device: cpu, 0, 1, etc.")
    parser.add_argument("--conf", type=float, default=0.25, help="Confidence threshold (default: 0.25).")
    parser.add_argument("--save-json", action="store_true", help="Save results to JSON.")
    args = parser.parse_args()

    model_path = Path(args.model).resolve()
    if not model_path.exists():
        print(f"ERROR: Model not found at {model_path}")
        print("Train a model first: python train.py")
        return 1

    data_path = Path(args.data).resolve()
    if not data_path.exists():
        print(f"ERROR: dataset.yaml not found at {data_path}")
        return 1

    print("=" * 60)
    print("  YOLOv8 Evaluation — Pig Feces Detection")
    print("=" * 60)
    print(f"  Model:    {model_path}")
    print(f"  Dataset:  {data_path}")
    print(f"  Target:   mAP50 >= {args.target * 100:.0f}%")
    print("=" * 60)
    print()

    model = YOLO(str(model_path))

    metrics = model.val(
        data=str(data_path),
        imgsz=args.imgsz,
        conf=args.conf,
        device=args.device if args.device else None,
        save_json=args.save_json,
        verbose=True,
    )

    print()
    print("=" * 60)
    print("  Results")
    print("=" * 60)

    map50 = metrics.box.map50
    map5095 = metrics.box.map
    precision = metrics.box.mp
    recall = metrics.box.mr
    f1 = 2 * (precision * recall) / (precision + recall) if (precision + recall) > 0 else 0

    print_metric("mAP50", map50, args.target)
    print_metric("mAP50-95", map5095)
    print_metric("Precision", precision)
    print_metric("Recall", recall)
    print_metric("F1 Score", f1)

    # Per-class mAP. With a 2-class model (feces + pig) the aggregate hides
    # feces-only performance, which is the number the panel actually cares
    # about, so break it out explicitly.
    per_class = []
    ap50 = getattr(metrics.box, "ap50", None)
    names = getattr(metrics, "names", None)
    ap_cls = getattr(metrics.box, "ap_class_index", None)
    if (
        ap50 is not None
        and ap_cls is not None
        and len(ap50) > 1
    ):
        print()
        for ci, idx in enumerate(ap_cls):
            name = names.get(int(idx), str(idx)) if names else str(idx)
            val = float(ap50[ci])
            per_class.append((name, val))
            print(f"    {name:<20} {val * 100:6.1f}%")

    print()

    passed = map50 >= args.target
    if passed:
        print(f"  RESULT: PASS — mAP50 ({map50 * 100:.1f}%) meets target ({args.target * 100:.0f}%)")
    else:
        print(f"  RESULT: FAIL — mAP50 ({map50 * 100:.1f}%) below target ({args.target * 100:.0f}%)")
        print()
        print("  Suggestions to improve:")
        print("    - Add more training images (aim for 500-1000)")
        print("    - Increase epochs (--epochs 200)")
        print("    - Try a larger model (--model yolov8s.pt)")
        print("    - Check label quality in dataset/labels/")
    print("=" * 60)

    csv_path = model_path.parent.parent / "results.csv"
    if csv_path.exists():
        print(f"\n  Epoch-by-epoch results: {csv_path}")

    summary_path = Path(args.model).parent.parent / "evaluation_summary.txt"
    with open(summary_path, "w") as f:
        f.write("YOLOv8 Evaluation Summary\n")
        f.write("=" * 40 + "\n")
        f.write(f"Model: {model_path.name}\n")
        f.write(f"Target mAP50: {args.target * 100:.0f}%\n\n")
        f.write(f"mAP50:      {map50 * 100:.1f}%\n")
        f.write(f"mAP50-95:   {map5095 * 100:.1f}%\n")
        f.write(f"Precision:  {precision * 100:.1f}%\n")
        f.write(f"Recall:     {recall * 100:.1f}%\n")
        f.write(f"F1 Score:   {f1 * 100:.1f}%\n")
        if per_class:
            f.write("\nPer-class mAP50:\n")
            for name, val in per_class:
                f.write(f"  {name:<16} {val * 100:.1f}%\n")
        f.write(f"\nRESULT: {'PASS' if passed else 'FAIL'}\n")
    print(f"  Summary saved: {summary_path}")

    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
