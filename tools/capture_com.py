#!/usr/bin/env python3
import pathlib
import sys
import time

import serial

port_name = sys.argv[1]
log_path = pathlib.Path(sys.argv[2])
log_path.parent.mkdir(parents=True, exist_ok=True)

with log_path.open("a", encoding="utf-8") as log:
    with serial.Serial(port_name, 115200, timeout=1) as port:
        port.dtr = False
        port.rts = False
        while True:
            line = port.readline().decode("utf-8", errors="replace")
            if line:
                log.write(f"{time.strftime('%Y-%m-%dT%H:%M:%S')} {line}")
                log.flush()
