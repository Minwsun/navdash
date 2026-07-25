#!/usr/bin/env python3
import pathlib
import sys

import av
from PIL import Image


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: export_h264_frames.py <in.h264> <out_dir> [max_frames]", file=sys.stderr)
        return 2
    src = pathlib.Path(sys.argv[1])
    out_dir = pathlib.Path(sys.argv[2])
    max_frames = int(sys.argv[3]) if len(sys.argv) > 3 else 12
    out_dir.mkdir(parents=True, exist_ok=True)
    container = av.open(str(src), format="h264")
    count = 0
    for index, frame in enumerate(container.decode(video=0)):
        if index >= max_frames:
            break
        rgb = frame.to_ndarray(format="rgb24")
        Image.fromarray(rgb, "RGB").save(out_dir / f"frame_{index:03d}.png")
        count += 1
    print(f"frames={count} out={out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
