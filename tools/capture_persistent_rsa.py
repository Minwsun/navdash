import argparse
import serial
import time
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--port", default="COM4")
parser.add_argument("--seconds", type=int, default=1800)
parser.add_argument("--output", default=r"captures\persistent-rsa-pair.log")
args = parser.parse_args()

path = Path(args.output)
with serial.Serial(args.port, 115200, timeout=.25) as port, path.open("w", encoding="utf-8") as output:
    output.write("CAPTURE_START\n"); output.flush()
    end = time.monotonic() + args.seconds
    while time.monotonic() < end:
        raw = port.readline()
        if raw:
            line = raw.decode("utf-8", errors="replace").rstrip()
            output.write(line + "\n"); output.flush()
