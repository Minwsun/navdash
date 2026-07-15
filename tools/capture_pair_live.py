import serial, time
from pathlib import Path
path = Path(r"captures\pair-live.log")
try:
    port = serial.Serial("COM4", 115200, timeout=.25)
except Exception as exc:
    path.write_text(f"OPEN_FAILED {type(exc).__name__}: {exc}\n", encoding="utf-8")
    raise
with path.open("w", encoding="utf-8") as output:
    output.write("CAPTURE_START\n")
    end = time.monotonic() + 180
    while time.monotonic() < end:
        raw = port.readline()
        if raw:
            line = raw.decode("utf-8", errors="replace").rstrip()
            print(line, flush=True)
            output.write(line + "\n")
            output.flush()
port.close()