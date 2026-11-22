"""Isolated process for exactly one historical simulator extension (official or v2 recording).

JSON over stdin/stdout. The v2 recording engines return float32 frames (F, 16, 6) = x, y, angle,
vx, vy, w (absent stone: six zeros). Needs numpy, so run it in the dc_determinism-cpu:local image.

    python3 pybind_worker.py --binary SO --sha256 HEX --engine fcv1-py27|fcv1-py36 \
        --kind official|record < request.json

Kinds:
    official  StoneSimulator.simulator(); a row is {shot_id, final}.
    record    StoneSimulator.simulator_frames() of a v2 recording binary (RECORD_VERSION 2,
              FRAME_FIELDS 6); a row is {shot_id, final, steps, removed_steps, frame_count,
              last_frame} and, when the request names "frames_dir", the full frames are written
              there as <n>.f32 (raw little-endian float32, shape [frame_count, 16, 6], C order)
              and the row carries "frames_file".

Request: {"schema": "dc-pybind-request-v1", "shots": [...pybind input dicts...],
          "frame_stride": 1, "removal_sheet_width": 0, "frames_dir": optional path}
A shot dict may carry its own "removal_sheet_width", which overrides the request value.
Response: {"schema": "dc-pybind-result-v1", "rows": [...], "python_version": "3.x.y",
           "numpy_version": "x.y.z"}
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import sys


def load_engine(path: Path, expected: str):
    actual = hashlib.sha256(path.read_bytes()).hexdigest()
    if actual != expected:
        raise ValueError(f"binary SHA-256 mismatch: {actual}")
    spec = importlib.util.spec_from_file_location("simulator", path)
    if spec is None or spec.loader is None:
        raise ValueError("cannot load simulator extension")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def arguments(task, engine, np):
    angular = float(task["angular_velocity"])
    if engine == "fcv1-py27":
        angular = int((angular > 0) - (angular < 0))
    return (
        np.asarray(task["initial_positions"], dtype=np.float64),
        int(task["total_shot_number"]),
        float(task["velocity_x"]),
        float(task["velocity_y"]),
        angular,
        int(task["team_number"]),
        int(task["shot_per_team"]),
        int(task["applied_rule"]),
    )


def observe(factory, task, engine, kind, stride, removal_width, frames_dir, np):
    args = arguments(task, engine, np)
    removal_width = float(task.get("removal_sheet_width", removal_width))  # per-shot override
    instance = factory()
    if kind == "official":
        final, _trajectory = instance.simulator(*args)
        return {"final": np.asarray(final).tolist()}
    final, frames, steps, removed = instance.simulator_frames(
        *args, frame_stride=stride, removal_sheet_width=removal_width
    )
    frames = np.asarray(frames)
    if frames.dtype != np.float32 or frames.ndim != 3 or frames.shape[1:] != (16, 6):
        raise ValueError(f"invalid recording frame layout: {frames.dtype} {frames.shape}")
    expected = steps // stride + 1 + int(steps % stride != 0)
    if len(frames) != expected or not np.isfinite(frames).all():
        raise ValueError("incomplete or non-finite frame stream")
    if not np.isfinite(np.asarray(final)).all():
        raise ValueError("non-finite final board")
    row = {
        "final": np.asarray(final).tolist(),
        "steps": int(steps),
        "removed_steps": list(removed),
        "frame_count": int(len(frames)),
        "last_frame": frames[-1].tolist(),
    }
    if frames_dir is not None:
        name = f"{hashlib.sha256(str(task['shot_id']).encode()).hexdigest()[:24]}.f32"
        (frames_dir / name).write_bytes(np.ascontiguousarray(frames, dtype="<f4").tobytes())
        row["frames_file"] = name
    return row


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--engine", choices=("fcv1-py27", "fcv1-py36"), required=True)
    parser.add_argument("--kind", choices=("official", "record"), required=True)
    args = parser.parse_args()
    import numpy as np

    module = load_engine(args.binary, args.sha256)
    if args.kind == "record" and (getattr(module, "RECORD_VERSION", None) != 2
                                  or getattr(module, "FRAME_FIELDS", None) != 6):
        raise ValueError("--kind record needs a v2 recording binary (RECORD_VERSION/FRAME_FIELDS)")
    request = json.load(sys.stdin)
    if request["schema"] != "dc-pybind-request-v1":
        raise ValueError("invalid request schema")
    stride = int(request.get("frame_stride", 1))
    width = float(request.get("removal_sheet_width", 0))
    if stride < 1 or not math.isfinite(width):
        raise ValueError("invalid recording options")
    frames_dir = None
    if request.get("frames_dir"):
        if args.kind != "record":
            raise ValueError("frames_dir needs --kind record")
        frames_dir = Path(request["frames_dir"])
        frames_dir.mkdir(parents=True, exist_ok=True)
    rows = []
    for task in request["shots"]:
        value = observe(module.StoneSimulator, task, args.engine, args.kind, stride, width,
                        frames_dir, np)
        rows.append({"shot_id": task["shot_id"], **value})
    json.dump({"schema": "dc-pybind-result-v1", "rows": rows,
               "python_version": sys.version.split()[0], "numpy_version": str(np.__version__)},
              sys.stdout, allow_nan=False, separators=(",", ":"))
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
