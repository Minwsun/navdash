#!/usr/bin/env python3
import pathlib
import re
import sys
from typing import Dict, Optional

PACKET = re.compile(r"^PKT (?P<ms>\d+) UDP (?P<source>\S+) -> 5000 len=(?P<length>\d+) hex=(?P<hex>[0-9A-F]+)$")
START_CODE = b"\x00\x00\x00\x01"


def rtp_payload(packet: bytes) -> Optional[bytes]:
    if len(packet) < 13 or packet[0] >> 6 != 2:
        return None
    offset = 12 + (packet[0] & 0x0F) * 4
    if packet[0] & 0x10:
        if offset + 4 > len(packet):
            return None
        offset += 4 + int.from_bytes(packet[offset + 2:offset + 4], "big") * 4
    return packet[offset:] if offset < len(packet) else None


def extract(log_path: pathlib.Path, out_path: pathlib.Path) -> Dict[str, int]:
    stats = {"rtp": 0, "annexb": 0, "royal_cont": 0, "single": 0, "fua_start": 0, "fua_end": 0, "bad": 0}
    nal_counts: Dict[int, int] = {}
    current_fu: Optional[bytearray] = None
    with log_path.open("r", encoding="utf-8", errors="replace") as src, out_path.open("wb") as out:
        for line in src:
            match = PACKET.match(line.strip())
            if not match:
                continue
            stats["rtp"] += 1
            payload = rtp_payload(bytes.fromhex(match["hex"]))
            if not payload:
                stats["bad"] += 1
                continue
            start = payload.find(START_CODE)
            if start >= 0:
                prefix = payload[:start]
                if len(prefix) >= 1:
                    prefix_type = prefix[0] & 0x1F
                    if 1 <= prefix_type <= 23:
                        out.write(START_CODE + prefix)
                        nal_counts[prefix_type] = nal_counts.get(prefix_type, 0) + 1
                    elif prefix_type == 28 and len(prefix) >= 2 and (prefix[1] & 0x80):
                        original_type = prefix[1] & 0x1F
                        out.write(START_CODE + bytes([(prefix[0] & 0xE0) | original_type]) + prefix[2:])
                        nal_counts[original_type] = nal_counts.get(original_type, 0) + 1
                out.write(payload[start:])
                stats["annexb"] += 1
                cursor = start
                while True:
                    cursor = payload.find(START_CODE, cursor)
                    if cursor < 0 or cursor + 4 >= len(payload):
                        break
                    nal_type = payload[cursor + 4] & 0x1F
                    nal_counts[nal_type] = nal_counts.get(nal_type, 0) + 1
                    cursor += 4
                continue
            if len(payload) > 2 and (payload[0] & 0x1F) == 28:
                out.write(payload[2:])
                stats["royal_cont"] += 1
                continue
            nal_type = payload[0] & 0x1F
            if 1 <= nal_type <= 23:
                nal_counts[nal_type] = nal_counts.get(nal_type, 0) + 1
                out.write(START_CODE + payload)
                stats["single"] += 1
            elif nal_type == 28 and len(payload) >= 2:
                fu_indicator, fu_header = payload[0], payload[1]
                original_type = fu_header & 0x1F
                nal_counts[original_type] = nal_counts.get(original_type, 0) + 1
                if fu_header & 0x80:
                    current_fu = bytearray([(fu_indicator & 0xE0) | original_type])
                    current_fu.extend(payload[2:])
                    stats["fua_start"] += 1
                elif current_fu is not None:
                    current_fu.extend(payload[2:])
                else:
                    stats["bad"] += 1
                    continue
                if fu_header & 0x40:
                    out.write(START_CODE + current_fu)
                    current_fu = None
                    stats["fua_end"] += 1
            else:
                stats["bad"] += 1
    for nal_type, count in sorted(nal_counts.items()):
        stats[f"nal_{nal_type}"] = count
    return stats


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: extract_h264_from_log.py <capture.log> <out.h264>", file=sys.stderr)
        return 2
    stats = extract(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]))
    print(" ".join(f"{key}={value}" for key, value in stats.items()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
