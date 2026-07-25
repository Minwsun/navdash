#!/usr/bin/env python3
import pathlib
import sys
import time

import serial

port_name = sys.argv[1]
log_path = pathlib.Path(sys.argv[2])
log_path.parent.mkdir(parents=True, exist_ok=True)

with log_path.open("a", encoding="utf-8") as log:
    port = serial.Serial()
    port.port = port_name
    port.baudrate = 115200
    port.timeout = 1
    port.dtr = False
    port.rts = False
    port.open()
    try:
        while True:
            line = port.readline().decode("utf-8", errors="replace")
            if line:
                log.write(f"{time.strftime('%Y-%m-%dT%H:%M:%S')} {line}")
                log.flush()
    finally:
        port.close()
