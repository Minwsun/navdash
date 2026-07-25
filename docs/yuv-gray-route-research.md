# Y/Gray/U/V route research

## Kết luận hiện tại

Royal gửi video H.264 `526x300` qua UDP `5000`. Dữ liệu không có lớp route riêng đã thấy trong capture hiện có; nguồn khả thi duy nhất vẫn là pixel video sau khi decode YUV.

Với capture hiện tại:

- `gray road/text` lấy được ổn bằng `Y` + tương phản cục bộ.
- `U/V` giúp tách sắc xanh, nhưng chưa đủ để xác nhận route line vì capture chưa có route xanh rõ.
- Nền map/water cũng có `U` cao, `V` thấp, nên threshold xanh đơn giản sẽ bắt nhầm vùng lớn.
- Firmware ESP32 yếu nên lưu `Y4 + class2`, không lưu RGB framebuffer.

## Capture đã dùng

```powershell
python tools\extract_h264_from_log.py captures\route-start.log analysis\route-start-real.h264
python tools\extract_h264_from_log.py captures\live-20260715.log analysis\live-20260715-real.h264
python tools\analyze_h264_yuv.py analysis\route-start-real.h264 analysis\route-start-real-yuv 40
python tools\analyze_h264_yuv.py analysis\live-20260715-real.h264 analysis\live-real-yuv 40
```

Preview kiểm tra:

```text
analysis/yuv-contact.png
analysis/route-start-real-yuv/frame_010_classes.png
analysis/live-real-yuv/frame_010_classes.png
```

## ROI hợp lệ

Frame decode là `526x300`, nhưng capture hiện có bị lỗi xanh ở nửa dưới do stream thiếu fragment/reference.

Analyzer chỉ đo vùng:

```text
x: 0..525
y: 0..291
```

Tương ứng YUV420 sample:

```text
Y  dùng mỗi 2 pixel
U  263x146
V  263x146
```

## Ngưỡng YUV đo được

Frame `route-start-real`, `frame_010`:

```text
road_gray:
  pct 2.52%
  Y p50 75
  U p50 138
  V p50 124

text_white:
  pct 0.89%
  Y p50 148
  U p50 136
  V p50 124

blue_candidate:
  pct 13.27%
  Y p50 65
  U p50 142
  V p50 121
```

Frame `live-real`, `frame_010`:

```text
road_gray:
  pct 2.35%
  Y p50 85
  U p50 138
  V p50 124

text_white:
  pct 0.89%
  Y p50 153
  U p50 136
  V p50 123

blue_candidate:
  pct 12.83%
  Y p50 71
  U p50 142
  V p50 121
```

`blue_candidate` hiện đang bắt nhiều nền xanh. Không dùng nó làm route thật cho đến khi có capture đang dẫn đường.

## Classifier hiện tại

Không phân loại gray bằng RGB. Dùng YUV trực tiếp:

```c
typedef enum {
    PIX_BG = 0,
    PIX_ROAD_GRAY = 1,
    PIX_TEXT_WHITE = 2,
    PIX_ROUTE_BLUE = 3,
} pixel_class_t;
```

Logic:

```text
local_mean = mean_5x5(Y)
contrast   = Y - local_mean
neutral    = U 132..141 && V 121..128

text_white:
  Y 115..220
  U 130..140
  V 121..127

route_blue_candidate:
  Y 58..130
  U 140..150
  V 117..123
  U - V >= 18

road_gray:
  Y 48..150
  neutral
  contrast >= 5
  not route_blue_candidate
```

Thứ tự:

```text
route candidate trước road
text ghi đè cuối để chữ luôn rõ
```

## Vì sao cần contrast cho gray

Threshold tuyệt đối kiểu `Y 60..90, U 134..139, V 123..125` bắt cả mảng nền map.

Road line thật là đường sáng mảnh trên nền tối. Dấu hiệu tốt hơn:

```text
Y gần neutral
Y cao hơn vùng xung quanh khoảng 5+
độ rộng nhỏ
```

Trên ESP32 có thể làm rẻ:

```text
decode/downsample Y
giữ 5 dòng Y nhỏ
tính mean_5x5 bằng rolling sum
classify từng pixel
```

## Cache cho ESP32

Không giữ RGB full frame.

```text
Target viewport: 240x137 hoặc crop tròn 240x240
Y4:             4 bit/pixel
class2:         2 bit/pixel
```

Nếu fit width `240x137`:

```text
Y4      = 240 * 137 / 2 = 16,440 byte
class2  = 240 * 137 / 4 =  8,220 byte
total   = 24,660 byte
```

Nếu full `240x240`:

```text
Y4      = 28,800 byte
class2  = 14,400 byte
total   = 43,200 byte
```

Render:

```text
rgb565 = palette[class2][Y4]
```

Palette:

```text
PIX_BG         dark navy/black
PIX_ROAD_GRAY  dim gray
PIX_TEXT_WHITE bright gray-white
PIX_ROUTE_BLUE navigation blue
```

## Điều cần capture thêm

Cần một capture đúng lúc Royal đang dẫn đường, route line xanh hiện rõ:

```powershell
python tools\capture_com.py COM4 captures\route-visible.log
```

Yêu cầu:

```text
device đã lưu
iPhone dùng 4G/5G
đã bấm dẫn đường
UDP 5000 chạy ít nhất 10 giây
route line xanh hiện rõ trên app
```

Sau đó chạy:

```powershell
python tools\extract_h264_from_log.py captures\route-visible.log analysis\route-visible.h264
python tools\analyze_h264_yuv.py analysis\route-visible.h264 analysis\route-visible-yuv 80
python tools\export_h264_frames.py analysis\route-visible.h264 analysis\route-visible-png 20
```

Nếu `route_blue_candidate` vẫn bắt nền lớn, hướng đúng là thêm lọc hình học:

```text
connected component
giữ đường dài/mảnh
bỏ blob lớn water/background
ưu tiên vùng trung tâm/đường từ marker
```

## Phán quyết

`gray road/text` đã có đường triển khai rõ.

`route blue` chưa đủ bằng chứng trên capture hiện tại. Không nên viết firmware route-blue final khi chưa có capture route-visible, vì sẽ biến nền map xanh thành route giả.
