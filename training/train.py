#!/usr/bin/env python3
"""
Train a YOLOv8 model for pig feces detection.

Usage:
    python train.py
    python train.py --model yolov8n --epochs 100 --imgsz 640 --batch 16
    python train.py --model yolov8s --epochs 200 --patience 20

Outputs:
    - Best model:  training/runs/detect/train/weights/best.pt
    - Last model:  training/runs/detect/train/weights/last.pt
    - Metrics CSV: training/runs/detect/train/results.csv
"""

import argparse
import os
from pathlib import Path

from ultralytics import YOLO


def main():
    parser = argparse.ArgumentParser(description="Train YOLOv8 for pig feces detection.")
    parser.add_argument(
        "--model",
        default="yolov8n.pt",
        help="Base model to start from (default: yolov8n.pt). Options: yolov8n, yolov8s, yolov8m.",
    )
    parser.add_argument("--epochs", type=int, default=100, help="Training epochs (default: 100).")
    parser.add_argument("--imgsz", type=int, default=640, help="Image size (default: 640).")
    parser.add_argument("--batch", type=int, default=16, help="Batch size (default: 16).")
    parser.add_argument("--patience", type=int, default=30, help="Early stopping patience (default: 30).")
    parser.add_argument("--lr", type=float, default=0.01, help="Initial learning rate (default: 0.01).")
    parser.add_argument(
        "--data",
        default=str(Path(__file__).parent / "dataset.yaml"),
        help="Path to dataset.yaml.",
    )
    parser.add_argument(
        "--project",
        default=str(Path(__file__).parent / "runs"),
        help="Project directory for saving results.",
    )
    parser.add_argument("--name", default="detect", help="Run name (default: detect).")
    parser.add_argument("--device", default="", help="Device: cpu, 0, 1, etc. (default: auto).")
    parser.add_argument("--exist-ok", action="store_true", help="Overwrite existing run.")
    parser.add_argument("--pretrained", action="store_true", default=True,
                        help="Use pretrained weights (default: True).")
    parser.add_argument("--no-pretrained", dest="pretrained", action="store_false",
                        help="Train from scratch.")
    args = parser.parse_args()

    data_path = Path(args.data).resolve()
    if not data_path.exists():
        print(f"ERROR: dataset.yaml not found at {data_path}")
        print("Please set up your dataset first:")
        print("  1. Place images in  dataset/images/train/  and  dataset/images/val/")
        print("  2. Place labels in  dataset/labels/train/  and  dataset/labels/val/")
        print("  3. See training/README.md for details.")
        return 1

    print("=" * 60)
    print("  YOLOv8 Training — Pig Feces Detection")
    print("=" * 60)
    print(f"  Base model:    {args.model}")
    print(f"  Epochs:        {args.epochs}")
    print(f"  Image size:    {args.imgsz}")
    print(f"  Batch size:    {args.batch}")
    print(f"  Patience:      {args.patience}")
    print(f"  Learning rate: {args.lr}")
    print(f"  Dataset:       {data_path}")
    print(f"  Output:        {Path(args.project) / args.name}")
    print("=" * 60)

    model = YOLO(args.model)

    results = model.train(
        data=str(data_path),
        epochs=args.epochs,
        imgsz=args.imgsz,
        batch=args.batch,
        patience=args.patience,
        lr0=args.lr,
        project=args.project,
        name=args.name,
        exist_ok=args.exist_ok,
        pretrained=args.pretrained,
        device=args.device if args.device else None,
        verbose=True,
    )

    best_path = Path(args.project) / args.name / "weights" / "best.pt"
    last_path = Path(args.project) / args.name / "weights" / "last.pt"

    print()
    print("=" * 60)
    print("  Training complete!")
    print("=" * 60)
    if best_path.exists():
        print(f"  Best model: {best_path}")
    if last_path.exists():
        print(f"  Last model: {last_path}")
    print(f"  Results:    {Path(args.project) / args.name / 'results.csv'}")
    print()
    print("  Next steps:")
    print("    python evaluate.py --model training/runs/detect/train/weights/best.pt")
    print("=" * 60)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
