#!/usr/bin/env python3
import collections
import pathlib
import re
import statistics
import sys
from typing import Dict, Iterable, List, Optional, Tuple

PACKET = re.compile(r"^PKT (?P<ms>\d+) UDP (?P<source>\S+) -> 5000 len=(?P<length>\d+) hex=(?P<hex>[0-9A-F]+)$")
START_CODE = b"\x00\x00\x00\x01"


def rtp_payload(packet: bytes) -> Optional[Dict[str, object]]:
    if len(packet) < 12 or packet[0] >> 6 != 2:
        return None
    offset = 12 + (packet[0] & 0x0F) * 4
    if packet[0] & 0x10:
        if offset + 4 > len(packet):
            return None
        offset += 4 + int.from_bytes(packet[offset + 2:offset + 4], "big") * 4
    if offset >= len(packet):
        return None
    return {
        "marker": bool(packet[1] & 0x80),
        "payload_type": packet[1] & 0x7F,
        "sequence": int.from_bytes(packet[2:4], "big"),
        "timestamp": int.from_bytes(packet[4:8], "big"),
        "payload": packet[offset:],
    }


def iter_log_packets(path: pathlib.Path) -> Iterable[Dict[str, object]]:
    with path.open("r", encoding="utf-8", errors="replace") as file:
        for line in file:
            match = PACKET.match(line.strip())
            if not match:
                continue
            info = rtp_payload(bytes.fromhex(match["hex"]))
            if not info:
                continue
            info["ms"] = int(match["ms"])
            yield info


def nal_types_in_payload(payload: bytes) -> List[int]:
    types: List[int] = []
    cursor = 0
    while True:
        start = payload.find(START_CODE, cursor)
        if start < 0:
            break
        if start + 4 < len(payload):
            types.append(payload[start + 4] & 0x1F)
        cursor = start + 4
    return types


def iter_annexb_nals(data: bytes) -> Iterable[bytes]:
    starts: List[int] = []
    cursor = 0
    while True:
        start = data.find(START_CODE, cursor)
        if start < 0:
            break
        starts.append(start)
        cursor = start + 4
    for start, end in zip(starts, starts[1:] + [len(data)]):
        if start + 4 < end:
            yield data[start + 4:end]


def split_access_units(nals: List[bytes]) -> List[List[bytes]]:
    units: List[List[bytes]] = []
    current: List[bytes] = []
    for nal in nals:
        if (nal[0] & 0x1F) == 7 and current:
            units.append(current)
            current = []
        current.append(nal)
    if current:
        units.append(current)
    return units


def rbsp(nal: bytes) -> bytes:
    out = bytearray()
    zero_count = 0
    for byte in nal[1:]:
        if zero_count == 2 and byte == 3:
            zero_count = 0
            continue
        out.append(byte)
        zero_count = zero_count + 1 if byte == 0 else 0
    return bytes(out)


class BitReader:
    def __init__(self, data: bytes):
        self.data = data
        self.index = 0

    def bit(self) -> int:
        value = (self.data[self.index >> 3] >> (7 - (self.index & 7))) & 1
        self.index += 1
        return value

    def bits(self, count: int) -> int:
        value = 0
        for _ in range(count):
            value = (value << 1) | self.bit()
        return value

    def ue(self) -> int:
        zero_count = 0
        while self.bit() == 0:
            zero_count += 1
        return (1 << zero_count) - 1 + self.bits(zero_count) if zero_count else 0

    def se(self) -> int:
        code = self.ue()
        return (code + 1) // 2 if code & 1 else -(code // 2)


def parse_sps(nal: bytes) -> Dict[str, object]:
    reader = BitReader(rbsp(nal))
    profile = reader.bits(8)
    constraints = reader.bits(8)
    level = reader.bits(8)
    sps_id = reader.ue()
    chroma = 1
    separate_colour_plane_flag = 0
    if profile in (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135):
        chroma = reader.ue()
        if chroma == 3:
            separate_colour_plane_flag = reader.bit()
        reader.ue()
        reader.ue()
        reader.bit()
        if reader.bit():
            raise RuntimeError("scaling matrix SPS unsupported by inspector")
    log2_max_frame_num = reader.ue() + 4
    poc_type = reader.ue()
    log2_max_pic_order_cnt_lsb = None
    if poc_type == 0:
        log2_max_pic_order_cnt_lsb = reader.ue() + 4
    elif poc_type == 1:
        reader.bit()
        reader.se()
        reader.se()
        for _ in range(reader.ue()):
            reader.se()
    max_refs = reader.ue()
    reader.bit()
    mb_width = reader.ue() + 1
    mb_height_map = reader.ue() + 1
    frame_mbs_only = reader.bit()
    if not frame_mbs_only:
        reader.bit()
    reader.bit()
    crop = (0, 0, 0, 0)
    if reader.bit():
        crop = (reader.ue(), reader.ue(), reader.ue(), reader.ue())
    coded_width = mb_width * 16
    coded_height = (2 - frame_mbs_only) * mb_height_map * 16
    crop_unit_x = 1 if chroma == 0 else 2
    crop_unit_y = (2 - frame_mbs_only) * (1 if chroma == 0 else 2)
    display_width = coded_width - (crop[0] + crop[1]) * crop_unit_x
    display_height = coded_height - (crop[2] + crop[3]) * crop_unit_y
    return {
        "profile": profile,
        "constraints": constraints,
        "level": level,
        "sps_id": sps_id,
        "chroma": chroma,
        "separate_colour_plane_flag": separate_colour_plane_flag,
        "log2_max_frame_num": log2_max_frame_num,
        "poc_type": poc_type,
        "log2_max_pic_order_cnt_lsb": log2_max_pic_order_cnt_lsb,
        "max_refs": max_refs,
        "frame_mbs_only": frame_mbs_only,
        "mb_width": mb_width,
        "mb_height": (2 - frame_mbs_only) * mb_height_map,
        "coded": f"{coded_width}x{coded_height}",
        "display": f"{display_width}x{display_height}",
        "crop": crop,
    }


def parse_pps(nal: bytes) -> Dict[str, object]:
    reader = BitReader(rbsp(nal))
    pps_id = reader.ue()
    sps_id = reader.ue()
    entropy_coding_mode_flag = reader.bit()
    bottom_field_pic_order_in_frame_present_flag = reader.bit()
    num_slice_groups_minus1 = reader.ue()
    if num_slice_groups_minus1 > 0:
        slice_group_map_type = reader.ue()
        if slice_group_map_type == 0:
            for _ in range(num_slice_groups_minus1 + 1):
                reader.ue()
        elif slice_group_map_type == 2:
            for _ in range(num_slice_groups_minus1):
                reader.ue()
                reader.ue()
                reader.ue()
        elif slice_group_map_type in (3, 4, 5):
            reader.bit()
            reader.ue()
        elif slice_group_map_type == 6:
            pic_size_in_map_units_minus1 = reader.ue()
            bits = max(1, (num_slice_groups_minus1 + 1).bit_length())
            for _ in range(pic_size_in_map_units_minus1 + 1):
                reader.bits(bits)
    reader.ue()
    reader.ue()
    weighted_pred_flag = reader.bit()
    weighted_bipred_idc = reader.bits(2)
    pic_init_qp_minus26 = reader.se()
    reader.se()
    reader.se()
    deblocking_filter_control_present_flag = reader.bit()
    constrained_intra_pred_flag = reader.bit()
    redundant_pic_cnt_present_flag = reader.bit()
    return {
        "pps_id": pps_id,
        "sps_id": sps_id,
        "entropy_coding_mode_flag": entropy_coding_mode_flag,
        "bottom_field_pic_order_in_frame_present_flag": bottom_field_pic_order_in_frame_present_flag,
        "num_slice_groups_minus1": num_slice_groups_minus1,
        "weighted_pred_flag": weighted_pred_flag,
        "weighted_bipred_idc": weighted_bipred_idc,
        "pic_init_qp_minus26": pic_init_qp_minus26,
        "deblocking_filter_control_present_flag": deblocking_filter_control_present_flag,
        "constrained_intra_pred_flag": constrained_intra_pred_flag,
        "redundant_pic_cnt_present_flag": redundant_pic_cnt_present_flag,
    }


def slice_name(slice_type: int) -> str:
    return ["P", "B", "I", "SP", "SI"][slice_type % 5] if slice_type % 5 < 5 else str(slice_type)


def parse_slice_header(nal: bytes, sps: Dict[str, object], pps: Dict[str, object]) -> Dict[str, object]:
    reader = BitReader(rbsp(nal))
    nal_ref_idc = (nal[0] >> 5) & 0x03
    nal_type = nal[0] & 0x1F
    first_mb_in_slice = reader.ue()
    raw_slice_type = reader.ue()
    pps_id = reader.ue()
    if sps["separate_colour_plane_flag"] == 1:
        reader.bits(2)
    frame_num = reader.bits(int(sps["log2_max_frame_num"]))
    field_pic_flag = 0
    bottom_field_flag = 0
    if not sps["frame_mbs_only"]:
        field_pic_flag = reader.bit()
        if field_pic_flag:
            bottom_field_flag = reader.bit()
    idr_pic_id = None
    if nal_type == 5:
        idr_pic_id = reader.ue()
    pic_order_cnt_lsb = None
    if sps["poc_type"] == 0:
        pic_order_cnt_lsb = reader.bits(int(sps["log2_max_pic_order_cnt_lsb"]))
        if pps["bottom_field_pic_order_in_frame_present_flag"] and not field_pic_flag:
            reader.se()
    if pps["redundant_pic_cnt_present_flag"]:
        reader.ue()
    no_output_of_prior_pics_flag = None
    long_term_reference_flag = None
    if nal_ref_idc:
        if nal_type == 5:
            no_output_of_prior_pics_flag = reader.bit()
            long_term_reference_flag = reader.bit()
        else:
            if reader.bit():
                raise RuntimeError("adaptive ref pic marking unsupported by inspector")
    slice_qp_delta = reader.se()
    disable_deblocking_filter_idc = None
    if pps["deblocking_filter_control_present_flag"]:
        disable_deblocking_filter_idc = reader.ue()
        if disable_deblocking_filter_idc != 1:
            reader.se()
            reader.se()
    return {
        "nal_type": nal_type,
        "nal_ref_idc": nal_ref_idc,
        "first_mb_in_slice": first_mb_in_slice,
        "slice_type": slice_name(raw_slice_type),
        "slice_type_raw": raw_slice_type,
        "pps_id": pps_id,
        "frame_num": frame_num,
        "field_pic_flag": field_pic_flag,
        "bottom_field_flag": bottom_field_flag,
        "idr_pic_id": idr_pic_id,
        "pic_order_cnt_lsb": pic_order_cnt_lsb,
        "no_output_of_prior_pics_flag": no_output_of_prior_pics_flag,
        "long_term_reference_flag": long_term_reference_flag,
        "slice_qp_delta": slice_qp_delta,
        "disable_deblocking_filter_idc": disable_deblocking_filter_idc,
    }


def summarize_numbers(values: List[int]) -> Dict[str, object]:
    if not values:
        return {}
    ordered = sorted(values)
    return {
        "count": len(values),
        "min": ordered[0],
        "avg": round(statistics.mean(values), 1),
        "p50": ordered[len(ordered) // 2],
        "max": ordered[-1],
    }


def inspect_log(path: pathlib.Path) -> None:
    packets = list(iter_log_packets(path))
    if not packets:
        print(f"{path}: no UDP 5000 RTP packets")
        return
    payload_types = collections.Counter(packet["payload_type"] for packet in packets)
    markers = sum(1 for packet in packets if packet["marker"])
    starts = collections.Counter()
    wrappers = collections.Counter()
    timestamp_groups: Dict[int, List[Dict[str, object]]] = collections.defaultdict(list)
    gaps = 0
    previous_sequence = None
    for packet in packets:
        sequence = int(packet["sequence"])
        if previous_sequence is not None and ((previous_sequence + 1) & 0xFFFF) != sequence:
            gaps += 1
        previous_sequence = sequence
        timestamp_groups[int(packet["timestamp"])].append(packet)
        payload = packet["payload"]
        assert isinstance(payload, bytes)
        for nal_type in nal_types_in_payload(payload):
            starts[nal_type] += 1
        if payload.startswith(b"\x3c\x87"):
            wrappers["chunk_start_sps_wrapped"] += 1
        elif payload.startswith(b"\x27\x42"):
            wrappers["chunk_start_sps_raw"] += 1
        elif payload.startswith(b"\x3c\x07"):
            wrappers["chunk_continue"] += 1
    duration = (int(packets[-1]["ms"]) - int(packets[0]["ms"])) / 1000.0
    deltas = [b - a for a, b in zip(sorted(timestamp_groups), sorted(timestamp_groups)[1:])]
    print(f"log={path}")
    print(f"packets={len(packets)} duration_s={duration:.3f} payload_types={dict(payload_types)} markers={markers} sequence_gaps={gaps}")
    print(f"timestamp_count={len(timestamp_groups)} timestamp_delta_hist={dict(collections.Counter(deltas))}")
    print(f"annexb_start_nals={dict(sorted(starts.items()))}")
    print(f"royal_wrappers={dict(wrappers)}")
    print("timestamp_groups_first=timestamp packets bytes seq_first seq_last markers nal_starts")
    for timestamp, group in list(sorted(timestamp_groups.items()))[:12]:
        payload_bytes = sum(len(packet["payload"]) for packet in group if isinstance(packet["payload"], bytes))
        nal_starts = collections.Counter()
        for packet in group:
            payload = packet["payload"]
            assert isinstance(payload, bytes)
            for nal_type in nal_types_in_payload(payload):
                nal_starts[nal_type] += 1
        print(timestamp, len(group), payload_bytes, group[0]["sequence"], group[-1]["sequence"], sum(1 for packet in group if packet["marker"]), dict(nal_starts))


def inspect_h264(path: pathlib.Path) -> None:
    nals = list(iter_annexb_nals(path.read_bytes()))
    counts = collections.Counter(nal[0] & 0x1F for nal in nals)
    sps_map: Dict[int, Dict[str, object]] = {}
    pps_map: Dict[int, Dict[str, object]] = {}
    slice_headers = []
    print(f"h264={path}")
    print(f"nals={len(nals)} types={dict(sorted(counts.items()))}")
    for nal in nals:
        nal_type = nal[0] & 0x1F
        if nal_type == 7:
            parsed = parse_sps(nal)
            sps_map[int(parsed["sps_id"])] = parsed
        elif nal_type == 8:
            parsed = parse_pps(nal)
            pps_map[int(parsed["pps_id"])] = parsed
        elif nal_type in (1, 5) and sps_map and pps_map:
            pps = next(iter(pps_map.values()))
            sps = sps_map[int(pps["sps_id"])]
            slice_headers.append(parse_slice_header(nal, sps, pps))
    if sps_map:
        first_sps_nal = next(nal for nal in nals if (nal[0] & 0x1F) == 7)
        print(f"sps_hex={first_sps_nal.hex()}")
        print(f"sps={next(iter(sps_map.values()))}")
    if pps_map:
        first_pps_nal = next(nal for nal in nals if (nal[0] & 0x1F) == 8)
        print(f"pps_hex={first_pps_nal.hex()}")
        print(f"pps={next(iter(pps_map.values()))}")
    units = split_access_units(nals)
    idr_sizes = [sum(len(nal) for nal in unit if (nal[0] & 0x1F) == 5) for unit in units if any((nal[0] & 0x1F) == 5 for nal in unit)]
    slice_counts = collections.Counter(sum(1 for nal in unit if (nal[0] & 0x1F) in (1, 5)) for unit in units)
    print(f"access_units={len(units)} slice_count_hist={dict(slice_counts)} idr_size={summarize_numbers(idr_sizes)}")
    print(f"slice_type_hist={dict(collections.Counter(header['slice_type'] for header in slice_headers))}")
    print("access_units_first=index nal_order nal_sizes")
    for index, unit in enumerate(units[:12]):
        print(index, [nal[0] & 0x1F for nal in unit], [len(nal) for nal in unit])
    print("slice_headers_first=first_mb slice_type frame_num idr_pic_id poc_lsb deblock")
    for header in slice_headers[:12]:
        print(header["first_mb_in_slice"], header["slice_type"], header["frame_num"], header["idr_pic_id"], header["pic_order_cnt_lsb"], header["disable_deblocking_filter_idc"])


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: inspect_royal_video.py <capture.log|stream.h264> [...]", file=sys.stderr)
        return 2
    for arg in sys.argv[1:]:
        path = pathlib.Path(arg)
        if path.suffix.lower() == ".h264":
            inspect_h264(path)
        else:
            inspect_log(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
