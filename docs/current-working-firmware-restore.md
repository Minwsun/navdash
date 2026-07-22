# Current Working Firmware Restore Notes

Tài liệu này mô tả firmware đang chạy được trên ESP32-D0WD-V3 để có thể khôi phục nhanh sau này.

## Restore Point

- Git commit đang là baseline gần nhất: `b4bc0d0 Document iOS Tripper Dash handshake`.
- Branch: `main`.
- Remote: `https://github.com/Minwsun/navdash.git`.
- Board: `esp32dev`.
- Upload port đã dùng thành công: `COM4`.

Khôi phục đúng source:

```powershell
git fetch origin
git checkout main
git reset --hard b4bc0d0
python -m platformio run -e esp32dev -t upload --upload-port COM4
```

## Firmware Scope

Firmware hiện làm được:

- Tạo Wi-Fi dash `RE_1234_567890`.
- Cho iPhone join bằng password `12345678`.
- Giữ iPhone dùng 4G/5G cho internet bằng DHCP gateway `0.0.0.0`.
- Nhận/gửi K1G control trên UDP.
- Hỗ trợ RSA auth handshake Royal/K1G.
- Gửi secure vehicle metadata `0F01..0F0A` để app có dữ liệu lưu thiết bị.
- Nhận RTP/H.264 trên UDP `5000`.
- Log đủ packet để debug.

Firmware hiện chưa làm:

- Decode H.264 ra TFT.
- Render route/map lên màn hình.
- Lưu RSA identity vào NVS.
- NAT internet qua ESP.

## Hardware

```text
MCU:        ESP32-D0WD-V3
CPU:        Xtensa LX6 dual-core 240 MHz
Flash:      4 MB
PSRAM:      Không dùng
Serial:     115200 baud
Upload:     COM4
Framework:  Arduino on PlatformIO
```

## Files

```text
platformio.ini                         PlatformIO env
include/config.h                       SSID/password
src/main.cpp                           firmware chính
tools/capture_com.py                   serial logger
docs/ios-royal-tripper-dash-design.md  thiết kế giao thức
docs/current-working-firmware-restore.md tài liệu khôi phục này
```

## Wi-Fi Design

SoftAP:

```text
SSID:      RE_1234_567890
Password:  12345678
ESP IP:    192.168.1.1
Subnet:    255.255.255.0
Gateway:   0.0.0.0
Client IP: thường là 192.168.1.2
```

Lý do gateway `0.0.0.0`:

```text
192.168.1.0/24  đi Wi-Fi tới ESP dash
Internet        đi 4G/5G của iPhone
```

Không đổi gateway về `192.168.1.1` nếu cần iPhone có mạng cellular khi đang join dash.

## UDP Ports

```text
UDP 2000  app -> ESP control K1G
UDP 2002  ESP -> app reply K1G; firmware cũng bind để bắt packet lệch port
UDP 5000  app -> ESP RTP/H.264
```

Firmware reply dual-source:

- Gửi từ socket `2000` tới `192.168.1.2:2002`.
- Gửi thêm từ socket `2002` tới `192.168.1.2:2002`.

Mục tiêu: tương thích cả trạng thái app iOS đã lưu device lẫn trạng thái pairing.

## Boot Flow

```text
Serial.begin(115200)
WiFi.onEvent(logWiFiEvent)
mbedTLS entropy + CTR_DRBG init
RSA-1024 runtime key generation
WiFi.mode(WIFI_AP)
WiFi.setSleep(false)
WiFi.softAPConfig(192.168.1.1, 0.0.0.0, 255.255.255.0)
WiFi.softAP(RE_1234_567890, 12345678)
MDNS.begin("reprime")
MDNS service: royalenfield/udp/2000
MDNS service: reprime/udp/2000
MDNS service: lnp/udp/2000
UDP begin: 2000, 2002, 5000
loop()
```

Boot log mong đợi:

```text
RSA READY result=0
AP READY ssid=RE_1234_567890 ip=192.168.1.1 broadcast=192.168.1.255 mac=...
MDNS READY
UDP READY port=2000
UDP READY port=2002
UDP READY port=5000
```

## Main Loop

Firmware không dùng FreeRTOS task riêng. Mọi thứ chạy trong `loop()`:

```text
sendBikeAnnounce()
sendAuthHint()
captureUdpPackets()
```

### `sendBikeAnnounce()`

- Broadcast K1G announce mỗi 1 giây tới `192.168.1.255:2000`.
- Giúp app nhận dash presence.

### `sendAuthHint()`

- Khi có station join, firmware chủ động gửi RSA public key.
- Tối đa 12 lần.
- Chu kỳ 300 ms.
- Đích mặc định `192.168.1.2:2002`.

Lý do cần auth hint:

- Có lúc iOS chỉ gửi heartbeat/status `06/08`, `06/10`, `06/0F`, không gửi `08/04`.
- Auth hint ép app có `07/00 + 07/03` để tiếp tục `08/00`.

### `captureUdpPackets()`

- Đọc packet từ UDP `2000`, `2002`, `5000`.
- In hex toàn bộ packet.
- Với `2000` hoặc `2002`: gửi announce unicast rồi parse K1G.
- Với `5000`: hiện tại chỉ log packet; đây là RTP/H.264 stream.

## K1G Packet Format

Header:

```text
00 LEN 00 SEGCOUNT 00 00 00 00 02 01 00 05 4B 31 47 20 SEQ
```

TLV:

```text
TYPE SUB LEN_H LEN_L PAYLOAD...
```

Ví dụ app heartbeat/status:

```text
06/08 len=1
06/10 len=1
06/0F len=1
06/01 len=1
06/03 len=1
06/04 len=1
05/17 len=1
05/21 len=1
05/4D len=1
05/22 len=1
```

## Discovery Replies

Broadcast announce payload:

```text
0018000200000000020100054B314720SEQ02060600030E3334
```

Unicast announce:

```text
ESP socket 2000 -> iPhone 2002
ESP socket 2002 -> iPhone 2002
```

Log:

```text
ANNOUNCE DUAL -> 192.168.1.2:2002
```

## Auth Handshake

Chuẩn đầy đủ:

```text
App -> ESP: 08/04 request RSA pubkey
ESP -> App: 07/00 RSA modulus
ESP -> App: 07/03 RSA exponent
App -> ESP: 08/00 RSA-PKCS1v1.5(ssid || aes_key)
ESP -> App: 07/01 01
ESP -> App: 0F01..0F0A encrypted vehicle metadata
```

Firmware cũng gửi `07/00 + 07/03` chủ động qua `sendAuthHint()`.

## RSA

```text
Key size: 1024 bit
Exponent: 65537
Padding: RSA/ECB/PKCS1Padding compatible
Lifetime: runtime only
```

Plaintext app gửi trong `08/00`:

```text
ASCII SSID + 32-byte AES session key
```

Điều kiện valid:

```text
plainLength == strlen(AP_SSID) + 32
memcmp(plain, AP_SSID, strlen(AP_SSID)) == 0
```

Log OK:

```text
AUTH session result=0 ssid=OK
```

## AES Secure Metadata

Sau khi decrypt `08/00`, firmware lưu 32-byte AES session key vào RAM.

`0Fxx` payload format:

```text
IV(16 bytes) + AES-256-CBC-PKCS7(plaintext)
```

AES dùng `mbedtls_aes_crypt_cbc()`.

## Vehicle Metadata

Firmware gửi đủ field app cần để lưu device:

```text
0F01 chassis:          NVD0000000000001
0F02 serial:           NVD-0001
0F03 model/name:       NAVDASH
0F05 bssid:            WiFi.softAPmacAddress()
0F06 manufacturing:    20260715
0F07 hw/fw version:    0.0.0.1
0F08 part number:      NVD-K1G
0F09 region:           0x01
0F0A FOTA version:     00000001
```

Log:

```text
SECURE_0F -> 192.168.1.2:2002 len=...
```

## iPhone Flow

Quy trình test sạch:

```text
1. Forget Wi-Fi RE_1234_567890 trên iPhone.
2. Bật cellular data.
3. Join Wi-Fi RE_1234_567890, password 12345678.
4. Mở Royal.
5. Add/Connect Tripper Dash.
6. Nếu device xuất hiện, bấm dẫn đường.
7. Xem log có UDP 5000.
```

## Expected Logs

Khi iPhone join:

```text
WIFI_JOIN mac=... aid=1
DHCP_LEASE ip=192.168.1.2
AUTH_PUBKEY -> 192.168.1.2:2002
TX K1G -> 192.168.1.2:2002 len=156 first=0700
```

Khi app nói chuyện:

```text
PKT ... UDP 192.168.1.2:<random> -> 2000 len=...
TLV RX ...
ANNOUNCE DUAL -> 192.168.1.2:2002
```

Khi auth chạy đúng:

```text
TLV RX 08/04 len=1
AUTH_PUBKEY -> 192.168.1.2:2002
TLV RX 08/00 len=128
AUTH session result=0 ssid=OK
SECURE_0F -> 192.168.1.2:2002 len=...
```

Khi route/video chạy:

```text
PKT ... UDP 192.168.1.2:<random> -> 5000 len=1372 hex=8060...
PKT ... UDP 192.168.1.2:<random> -> 5000 len=60 hex=80E0...
```

## Capture Commands

Upload:

```powershell
python -m platformio run -e esp32dev -t upload --upload-port COM4
```

Serial capture:

```powershell
python tools\capture_com.py COM4 captures\manual.log
```

Nếu `COM4` bận:

```powershell
Get-CimInstance Win32_Process |
  Where-Object { $_.CommandLine -match 'COM4|capture_com|platformio.*monitor' } |
  Select-Object ProcessId,Name,CommandLine

Stop-Process -Id <PID> -Force
```

## Known Good Evidence

Các log đã chứng minh từng phần:

```text
captures/dhcp-baseline.log
  iPhone join + gateway 0.0.0.0 + app control + RTP 5000.

captures/route-start.log
  app bắt đầu navigation, nhiều RTP/H.264 packets tới UDP 5000.

captures/no-device-dual-2000-2002-live.log
  firmware dual-source 2000/2002, app gửi heartbeat/status, chưa gửi 08/04 trong lần đó.
```

## Failure Diagnosis

### Không thấy Wi-Fi

Kiểm tra boot log:

```text
AP READY ssid=RE_1234_567890
```

Nếu không có, nạp lại firmware hoặc reset board.

### iPhone connect nhưng không có internet

Kiểm tra code còn:

```cpp
WiFi.softAPConfig(kDashIp, kNoDefaultGateway, IPAddress(255, 255, 255, 0));
```

Không đổi `kNoDefaultGateway` thành `kDashIp`.

### App không lưu device

Cần log có đủ:

```text
TLV RX 08/00
AUTH session result=0 ssid=OK
SECURE_0F
```

Nếu chỉ thấy heartbeat `06/08`, app chưa vào auth path hoặc đang dùng state cũ.

### Không có route/video

Cần log có UDP `5000`.

Nếu không có `5000`:

- Device chưa được app nhận.
- Chưa bấm dẫn đường.
- App chưa vào projection/nav mode.

## Không Được Đổi Khi Restore

Giữ nguyên các điểm này:

```text
SSID RE_1234_567890
Password 12345678
ESP IP 192.168.1.1
DHCP gateway 0.0.0.0
UDP bind 2000, 2002, 5000
Reply target iPhone :2002
Dual-source reply 2000 + 2002
Auth hint sau Wi-Fi join
0F01..0F0A sau auth OK
```

## Hướng Phát Triển Sau

Chỉ thêm sau khi handshake ổn định:

```text
1. Persistent RSA identity trong NVS.
2. RTP jitter buffer nhỏ.
3. H.264 IDR-only parser.
4. Map cache Y4 + class2.
5. TFT strip DMA renderer.
```

Không thêm NAT, BLE, LVGL video path, full-frame double buffer cho bản ESP32 không PSRAM.
