#!/usr/bin/env python3
import csv
import pathlib
import sys
from typing import Dict

import av
import numpy as np
from PIL import Image


def plane_array(frame: av.VideoFrame, plane_index: int, width: int, height: int) -> np.ndarray:
    plane = frame.planes[plane_index]
    data = np.frombuffer(bytes(plane), dtype=np.uint8)
    return data.reshape((plane.height, plane.line_size))[:height, :width].copy()


def classify_yuv(y: np.ndarray, u: np.ndarray, v: np.ndarray) -> np.ndarray:
    classes = np.zeros(y.shape, dtype=np.uint8)
    local_mean = local_mean_5x5(y)
    contrast = y.astype(np.int16) - local_mean
    neutral = (u >= 132) & (u <= 141) & (v >= 121) & (v <= 128)
    text = (y >= 115) & (y <= 220) & (u >= 130) & (u <= 140) & (v >= 121) & (v <= 127)
    route = (y >= 58) & (y <= 130) & (u >= 140) & (u <= 150) & (v >= 117) & (v <= 123) & ((u.astype(np.int16) - v.astype(np.int16)) >= 18)
    road = (y >= 48) & (y <= 150) & neutral & (contrast >= 5) & ~route
    classes[road] = 1
    classes[text] = 2
    classes[route] = 3
    return classes


def local_mean_5x5(y: np.ndarray) -> np.ndarray:
    padded = np.pad(y.astype(np.int16), 2, mode="edge")
    total = np.zeros(y.shape, dtype=np.int16)
    for dy in range(5):
        for dx in range(5):
            total += padded[dy:dy + y.shape[0], dx:dx + y.shape[1]]
    return total // 25


def class_stats(name: str, mask: np.ndarray, y: np.ndarray, u: np.ndarray, v: np.ndarray) -> Dict[str, object]:
    if not np.any(mask):
        return {
            f"{name}_pct": 0.0,
            f"{name}_y_p10": "",
            f"{name}_y_p50": "",
            f"{name}_y_p90": "",
            f"{name}_u_p10": "",
            f"{name}_u_p50": "",
            f"{name}_u_p90": "",
            f"{name}_v_p10": "",
            f"{name}_v_p50": "",
            f"{name}_v_p90": "",
        }
    return {
        f"{name}_pct": float(np.mean(mask) * 100.0),
        f"{name}_y_p10": int(np.percentile(y[mask], 10)),
        f"{name}_y_p50": int(np.percentile(y[mask], 50)),
        f"{name}_y_p90": int(np.percentile(y[mask], 90)),
        f"{name}_u_p10": int(np.percentile(u[mask], 10)),
        f"{name}_u_p50": int(np.percentile(u[mask], 50)),
        f"{name}_u_p90": int(np.percentile(u[mask], 90)),
        f"{name}_v_p10": int(np.percentile(v[mask], 10)),
        f"{name}_v_p50": int(np.percentile(v[mask], 50)),
        f"{name}_v_p90": int(np.percentile(v[mask], 90)),
    }


def save_class_preview(out_path: pathlib.Path, classes: np.ndarray) -> None:
    palette = np.array([
        [0, 0, 0],
        [150, 150, 150],
        [255, 255, 255],
        [0, 120, 255],
    ], dtype=np.uint8)
    rgb = palette[classes]
    Image.fromarray(np.repeat(np.repeat(rgb, 2, axis=0), 2, axis=1), "RGB").save(out_path)


def analyze(path: pathlib.Path, out_dir: pathlib.Path, max_frames: int) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    container = av.open(str(path), format="h264")
    rows = []
    for index, frame in enumerate(container.decode(video=0)):
        if index >= max_frames:
            break
        yuv = frame.reformat(format="yuv420p")
        width, height = yuv.width, yuv.height
        y = plane_array(yuv, 0, width, height)
        u = plane_array(yuv, 1, width // 2, height // 2)
        v = plane_array(yuv, 2, width // 2, height // 2)
        y_small = y[::2, ::2]
        roi_h = min(146, y_small.shape[0])
        yr = y_small[:roi_h, :]
        ur = u[:roi_h, :]
        vr = v[:roi_h, :]
        classes = classify_yuv(yr, ur, vr)
        row = {
            "frame": index,
            "width": width,
            "height": height,
            "roi": f"0,0,{width},{roi_h * 2}",
            "y_min": int(yr.min()),
            "y_p05": int(np.percentile(yr, 5)),
            "y_mean": float(np.mean(yr)),
            "y_p50": int(np.percentile(yr, 50)),
            "y_p95": int(np.percentile(yr, 95)),
            "y_max": int(yr.max()),
            "u_mean": float(np.mean(ur)),
            "v_mean": float(np.mean(vr)),
        }
        row.update(class_stats("road_gray", classes == 1, yr, ur, vr))
        row.update(class_stats("text_white", classes == 2, yr, ur, vr))
        row.update(class_stats("route_blue", classes == 3, yr, ur, vr))
        rows.append(row)
        if index in (0, 1, 2, 5, 10):
            (out_dir / f"frame_{index:03d}_y.raw").write_bytes(y.tobytes())
            (out_dir / f"frame_{index:03d}_u.raw").write_bytes(u.tobytes())
            (out_dir / f"frame_{index:03d}_v.raw").write_bytes(v.tobytes())
            save_class_preview(out_dir / f"frame_{index:03d}_classes.png", classes)
    if not rows:
        raise RuntimeError("no frames decoded")
    with (out_dir / "yuv_stats.csv").open("w", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(file, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    print(f"frames={len(rows)} size={rows[0]['width']}x{rows[0]['height']} out={out_dir}")


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: analyze_h264_yuv.py <in.h264> <out_dir> [max_frames]", file=sys.stderr)
        return 2
    analyze(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), int(sys.argv[3]) if len(sys.argv) > 3 else 30)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
