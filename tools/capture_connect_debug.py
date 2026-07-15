import serial, time
from pathlib import Path
path=Path(r"captures\connect-debug.log")
with serial.Serial("COM4",115200,timeout=.25) as port, path.open("w",encoding="utf-8") as output:
    output.write("CAPTURE_START\n"); output.flush()
    end=time.monotonic()+180
    while time.monotonic()<end:
        raw=port.readline()
        if raw:
            line=raw.decode("utf-8",errors="replace").rstrip()
            output.write(line+"\n"); output.flush()