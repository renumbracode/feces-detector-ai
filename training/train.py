#!/usr/bin/env python3
"""
Train a YOLOv8 model for pig feces detection.

Usage:
    python train.py
    python train.py --model yolov8n --epochs 100 --imgsz 640 --batch 16
    python train.py --model yolov8s --epochs 200 --patience 20
    python train.py --resume --chunk-epochs 2 --threads 8

Outputs (with the default --project/--name/--exist-ok, ultralytics writes
directly into the run dir rather than nesting under train/):
    - Best model:  training/runs/detect/weights/best.pt
    - Last model:  training/runs/detect/weights/last.pt
    - Metrics CSV: training/runs/detect/results.csv
"""

import argparse
import os
import shutil
from pathlib import Path


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
    parser.add_argument("--resume", action="store_true", help="Resume training from an existing checkpoint.")
    parser.add_argument(
        "--checkpoint",
        default=None,
        help="Checkpoint to resume from (default: <project>/<name>/weights/last_resume.pt if present, else last.pt).",
    )
    parser.add_argument(
        "--chunk-epochs",
        type=int,
        default=0,
        help="Stop after this many additional epochs; 0 runs to the configured total.",
    )
    parser.add_argument(
        "--threads",
        type=int,
        default=8,
        help="CPU thread cap (default: 8; 0 leaves PyTorch defaults).",
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=None,
        help="Data loader workers (default: Ultralytics default; use 4 on XPU).",
    )
    parser.add_argument("--exist-ok", action="store_true", help="Overwrite existing run.")
    parser.add_argument("--pretrained", action="store_true", default=True,
                        help="Use pretrained weights (default: True).")
    parser.add_argument("--no-pretrained", dest="pretrained", action="store_false",
                        help="Train from scratch.")
    args = parser.parse_args()

    if args.chunk_epochs < 0:
        print("ERROR: --chunk-epochs must be >= 0")
        return 1
    if args.threads < 0:
        print("ERROR: --threads must be >= 0")
        return 1

    data_path = Path(args.data).resolve()
    if not data_path.exists():
        print(f"ERROR: dataset.yaml not found at {data_path}")
        print("Please set up your dataset first:")
        print("  1. Place images in  dataset/images/train/  and  dataset/images/val/")
        print("  2. Place labels in  dataset/labels/train/  and  dataset/labels/val/")
        print("  3. See training/README.md for details.")
        return 1

    if args.checkpoint:
        checkpoint_path = Path(args.checkpoint).resolve()
    else:
        weights_dir = Path(args.project) / args.name / "weights"
        resume_path = weights_dir / "last_resume.pt"
        checkpoint_path = (resume_path if resume_path.exists() else weights_dir / "last.pt").resolve()
    if args.resume and not checkpoint_path.exists():
        print(f"ERROR: resume checkpoint not found at {checkpoint_path}")
        return 1

    run_dir = checkpoint_path.parent.parent if args.resume else Path(args.project) / args.name

    if args.threads > 0:
        for env_name in ("OMP_NUM_THREADS", "MKL_NUM_THREADS", "OPENBLAS_NUM_THREADS", "NUMEXPR_NUM_THREADS"):
            os.environ[env_name] = str(args.threads)

    import torch
    from ultralytics import YOLO

    if args.threads > 0:
        torch.set_num_threads(args.threads)
        try:
            torch.set_num_interop_threads(max(1, args.threads // 2))
        except RuntimeError:
            pass

    model = YOLO(str(checkpoint_path) if args.resume else args.model)

    print("=" * 60)
    print("  YOLOv8 Training — Pig Feces Detection")
    print("=" * 60)
    print(f"  Mode:          {'resume' if args.resume else 'new training'}")
    if args.resume:
        ckpt_args = (model.ckpt or {}).get("train_args", {})
        completed = (model.ckpt or {}).get("epoch", -1) + 1
        print(f"  Checkpoint:    {checkpoint_path}")
        print(f"  Completed:     {completed}/{ckpt_args.get('epochs', '?')} epochs")
        print(f"  Batch / size:  {ckpt_args.get('batch', '?')} / {ckpt_args.get('imgsz', '?')}")
    else:
        print(f"  Base model:    {args.model}")
        print(f"  Epochs:        {args.epochs}")
        print(f"  Image size:    {args.imgsz}")
        print(f"  Batch size:    {args.batch}")
        print(f"  Patience:      {args.patience}")
        print(f"  Learning rate: {args.lr}")
    print(f"  Chunk epochs:  {args.chunk_epochs if args.chunk_epochs else 'full run'}")
    print(f"  CPU threads:   {args.threads if args.threads else 'PyTorch default'}")
    print(f"  Workers:       {args.workers if args.workers is not None else 'Ultralytics default'}")
    print(f"  Dataset:       {data_path}")
    print(f"  Output:        {run_dir}")
    print("=" * 60)

    if args.chunk_epochs > 0:
        chunk_state = {"stop_at": None}

        def on_train_start(trainer):
            stop_at = min(trainer.start_epoch + args.chunk_epochs, trainer.epochs)
            chunk_state["stop_at"] = stop_at
            print(f"Chunk target: stop after epoch {stop_at}/{trainer.epochs}")

        def on_fit_epoch_end(trainer):
            stop_at = chunk_state["stop_at"]
            if stop_at is not None and trainer.epoch + 1 >= stop_at:
                trainer.stop = True
                last = getattr(trainer, "last", None)
                if last is not None and Path(last).exists():
                    resume_path = Path(last).parent / "last_resume.pt"
                    shutil.copy2(last, resume_path)
                    print(f"Resumable checkpoint: {resume_path}")
                print(f"Chunk complete at epoch {trainer.epoch + 1}/{trainer.epochs}")

        model.add_callback("on_train_start", on_train_start)
        model.add_callback("on_fit_epoch_end", on_fit_epoch_end)

    train_kwargs = {
        "device": args.device if args.device else None,
        "verbose": True,
    }
    if args.workers is not None:
        train_kwargs["workers"] = args.workers

    if args.resume:
        results = model.train(resume=str(checkpoint_path), **train_kwargs)
    else:
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
            **train_kwargs,
        )

    best_path = run_dir / "weights" / "best.pt"
    last_path = run_dir / "weights" / "last.pt"
    resume_path = run_dir / "weights" / "last_resume.pt"
    if args.chunk_epochs > 0 and resume_path.exists():
        shutil.copy2(resume_path, last_path)

    print()
    print("=" * 60)
    print("  Chunk complete!" if args.chunk_epochs else "  Training complete!")
    print("=" * 60)
    if best_path.exists():
        print(f"  Best model: {best_path}")
    else:
        print(f"  WARNING: no best.pt at {best_path}")
    if last_path.exists():
        print(f"  Last model: {last_path}")
    print(f"  Results:    {run_dir / 'results.csv'}")
    print()
    print("  Next steps:")
    print(f"    python evaluate.py --model {best_path}")
    print("=" * 60)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
