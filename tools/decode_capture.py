#!/usr/bin/env python3
import re
import sys

PACKET = re.compile(r"^PKT (?P<ms>\d+) (?P<transport>TCP|UDP) (?P<source>\S+) -> (?P<port>\d+) len=(?P<length>\d+) hex=(?P<hex>[0-9A-F]+)$")

for line in sys.stdin:
    match = PACKET.match(line.strip())
    if not match:
        continue
    data = bytes.fromhex(match["hex"])
    preview = ''.join(chr(byte) if 32 <= byte < 127 else '.' for byte in data[:96])
    print(f'{match["ms"]:>10}ms {match["transport"]:<3} {match["source"]:<22} port={match["port"]:<5} bytes={len(data):<4} ascii={preview}')
