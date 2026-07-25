# Royal video stream reverse engineering

## Kết luận hiện tại

Royal gửi map/dashboard qua UDP `5000` bằng RTP payload type `96`.

Sau khi unwrap Royal payload, video là H.264 Annex-B:

```text
codec:        H.264 Baseline
profile_idc:  66
level_idc:    2.1
entropy:      CAVLC
FMO:          off
chroma:       YUV420
coded size:   528x304
display crop: 526x300
macroblocks:  33x19
NAL seen:     7 SPS, 8 PPS, 5 IDR, 6 SEI
NAL not seen: 1 non-IDR P slice
```

Kết quả này thay đổi đánh giá tính khả thi: nếu capture dài vẫn all-IDR, ESP32-D0WD không cần DPB, reference frame, motion compensation, motion vector hoặc P-frame drift handling.

## Lệnh kiểm chứng

```powershell
python tools\inspect_royal_video.py captures\route-start.log captures\live-20260715.log analysis\route-start-real.h264 analysis\live-20260715-real.h264
```

Tool liên quan:

```text
tools/inspect_royal_video.py
tools/extract_h264_from_log.py
tools/analyze_h264_yuv.py
tools/export_h264_frames.py
```

## RTP / Royal wrapper

RTP:

```text
version:      2
payload type: 96
marker bit:   có dùng
sequence:     có dùng
timestamp:    delta thấy được thường 90
```

Serial log chậm, có thể rơi packet, nên timestamp/fps từ log chỉ dùng để debug wrapper. Không dùng để chốt bitrate/fps thật.

Royal payload không phải RFC 6184 thuần. Có ba dạng đã thấy.

### IDR start packet

```text
3c 87 42 00 15 ab 81 08 4f d5 a0
00 00 00 01 28 ce 3c 80
00 00 00 01 25 ...
```

Diễn giải:

```text
3c 87 + SPS payload
Annex-B PPS
Annex-B IDR slice
```

Unwrap:

```text
3c 87 42 00 15 ab 81 08 4f d5 a0
→ 00 00 00 01 27 42 00 15 ab 81 08 4f d5 a0
```

### Continuation packet

```text
3c 07 ...
```

Đây là fragment tiếp theo của NAL đang mở. Trên ESP32 không ghép thành buffer lớn; `RoyalRbspReader` đọc xuyên qua danh sách RTP fragment.

### SEI packet

```text
27 42 00 15 ab 81 08 4f d5 a0
00 00 00 01 28 ce 3c 80
00 00 00 01 06 ...
```

SEI hiện không cần cho route extraction. Cache SPS/PPS, bỏ SEI.

## SPS/PPS đã khóa

SPS:

```text
sps_hex=27420015ab81084fd5a0
profile=66
level=21
chroma=1
log2_max_frame_num=5
poc_type=0
log2_max_pic_order_cnt_lsb=6
max_refs=0
frame_mbs_only=1
mb_width=33
mb_height=19
coded=528x304
display=526x300
crop=(0, 1, 0, 2)
```

PPS:

```text
pps_hex=28ce3c80
entropy_coding_mode_flag=0
num_slice_groups_minus1=0
deblocking_filter_control_present_flag=1
redundant_pic_cnt_present_flag=0
```

Điểm quan trọng:

```text
entropy_coding_mode_flag=0 → CAVLC, không CABAC
num_slice_groups_minus1=0 → không FMO
frame_mbs_only=1          → progressive frame
```

## Access Unit / slice

`route-start-real.h264`:

```text
NAL histogram:       {5:84, 6:127, 7:211, 8:211}
access_units:        211
slice_count_hist:    {1:84, 0:127}
IDR size min/avg/max: 1337 / 2436 / 9485 bytes
slice_type_hist:     I:84
```

`live-20260715-real.h264`:

```text
NAL histogram:       {5:75, 6:129, 7:204, 8:204}
access_units:        204
slice_count_hist:    {1:75, 0:129}
IDR size min/avg/max: 1337 / 2710 / 9485 bytes
slice_type_hist:     I:75
```

Slice header đầu các IDR:

```text
first_mb_in_slice=0
slice_type=I
frame_num=0
poc_lsb=0
disable_deblocking_filter_idc=0
```

Điều này phù hợp với macroblock raster bình thường: một IDR picture có một I slice bắt đầu tại macroblock `0`.

## Kiến trúc lightweight

Không làm:

```text
Serial print toàn video packet
copy IDR NAL thành block RAM lớn
full RGB framebuffer
full YUV frame cache
P-frame / MV path khi chưa thấy NAL type 1
```

Làm:

```text
UDP 5000 receiver
RTP header parser
Royal wrapper unwrap
RTP fragment ring
segmented RBSP bitreader
SPS/PPS cache
SEI ignore
IDR intra CAVLC decoder
macroblock-row output
Y4 + class2 cache
RGB565 strip DMA
```

Pipeline:

```text
UDP packet
→ RTP parser
→ Royal unwrap
→ logical NAL event
→ RBSP de-escape xuyên fragment
→ IDR slice parser/CAVLC
→ reconstruct MB row
→ deblock
→ crop/scale/classify
→ render cache
```

## Điều kiện macroblock-row

Không xuất row ngay khi reconstruct xong.

Quy tắc an toàn:

```text
decode row N
deblock vertical edge trong row N
deblock horizontal edge giữa row N-1 và row N
row N-1 finalized
crop/scale/classify row N-1
reuse buffer cũ
```

Vòng buffer:

```text
previous row → chờ horizontal deblock
current row  → đang decode
spare row    → đơn giản hóa xoay vòng
```

## RAM mục tiêu D0WD

Một macroblock row:

```text
Y: 528 * 16     =  8,448 bytes
U: 264 * 8      =  2,112 bytes
V: 264 * 8      =  2,112 bytes
total one row   = 12,672 bytes
three rows      = 38,016 bytes
```

Buffer chính:

```text
3 hàng YUV MB:     38 KB
Y4 + class2 cache: 25 KB cho 240x137 hoặc 43 KB cho 240x240
TFT strips:        10-23 KB
RTP/FU pool:       25-40 KB
CAVLC/MB state:    cần benchmark
```

Đây là mức có cơ sở cho ESP32-D0WD không PSRAM nếu decoder IDR đủ nhanh.

## RBSP reader bắt buộc

Không ghép nguyên IDR.

Reader cần giữ state xuyên RTP fragment:

```c
typedef struct {
    const RtpFragment *fragment;
    size_t fragment_offset;
    uint32_t bit_cache;
    uint8_t bits_available;
    uint8_t previous_byte_1;
    uint8_t previous_byte_2;
} RoyalRbspReader;
```

Phải xử lý:

```text
RTP padding
CSRC count
RTP extension
sequence gap
Royal 3c87 / 2742 / 3c07 wrapper
emulation prevention 00 00 03 qua ranh giới fragment
```

Nếu thiếu fragment:

```text
drop current NAL
drop current Access Unit
wait next complete IDR
```

Vì stream đang all-IDR, packet loss không tạo drift dài.

## Thứ tự triển khai

Mốc 1 — stream validator:

```text
inspect PPS/FMO/slice
export Annex-B sạch
ffprobe / framemd5 làm chuẩn
```

Mốc 2 — IDR offline:

```text
IDR trong Flash
decode YUV420
CRC từng plane
so sánh FFmpeg
```

Mốc 3 — Y-only row decoder:

```text
decode theo MB row
xuất Y8
CRC từng finalized row
so sánh FFmpeg Y plane
```

Mốc 4 — U/V + class2:

```text
finalized YUV row
scale/crop
Y8 → Y4
U/V → class2
drop source row
```

Mốc 5 — UDP/RTP:

```text
packet ring
segmented RBSP reader
drop AU lỗi
```

Mốc 6 — TFT:

```text
render Y4/class2 cache
RGB565 strip DMA
không LVGL video framebuffer
```

## Tiêu chí quyết định

```text
capture dài xuất hiện NAL 1 → đánh giá lại reference/P-frame
capture dài vẫn all-IDR    → tiếp tục D0WD
PPS FMO != 0               → row decoder phức tạp, dừng tối ưu
IDR offline > 1s           → khó đạt stream thật
IDR offline 500-700ms      → còn hy vọng với low fps
row sai mỗi 16 dòng        → lỗi deblock/top-edge state
packet loss thường xuyên   → tăng RTP pool hoặc drop AU mạnh hơn
```

## Việc cần khóa tiếp

Capture đại diện dài hơn:

```text
map đứng yên
xe di chuyển
map xoay
zoom
chuyển maneuver
panel mở/đóng
1-3 phút liên tục
```

Lệnh:

```powershell
python tools\capture_com.py COM4 captures\route-visible-long.log
python tools\inspect_royal_video.py captures\route-visible-long.log
python tools\extract_h264_from_log.py captures\route-visible-long.log analysis\route-visible-long.h264
python tools\inspect_royal_video.py analysis\route-visible-long.h264
```

Nếu vẫn chỉ có NAL `5/6/7/8`, bắt đầu mốc 2: IDR offline decoder.
