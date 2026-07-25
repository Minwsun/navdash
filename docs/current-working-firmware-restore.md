# NavDash Working Firmware Restore

Tài liệu này ghi lại firmware ESP32-D0WD-V3 đang chạy được với Royal iOS Tripper Dash. Mục tiêu chính: iPhone kết nối dash qua Wi-Fi `RE_*`, app Royal lưu/nhận thiết bị, khi dẫn đường iPhone vẫn dùng 4G/5G cho internet.

## Trạng Thái Chốt

```text
Ngày xác nhận:     2026-07-23
Board:             ESP32-D0WD-V3 / esp32dev
Port nạp:          COM4
SSID:              RE_1234_567890
Password:          12345678
ESP IP:            192.168.1.1
DHCP gateway:      0.0.0.0
Control UDP:       2000, 2002
Video UDP:         5000
Firmware version:  0.0.0.1
Device serial:     NVD-0001
```

Build gần nhất:

```text
RAM:    48,152 / 327,680 bytes
Flash:  789,489 / 1,310,720 bytes
```

Lưu ý restore: `b4bc0d0` là tài liệu baseline cũ, không phải source đủ mới nhất. Bản chạy đúng hiện tại là source local sau khi thêm RSA NVS và đổi timing auth hint sang sau DHCP/`06/08`.

## File Quan Trọng

```text
platformio.ini                         PlatformIO env esp32dev
include/config.h                       SSID, password, version, serial
include/royal_dash.h                   API module Royal Dash
src/main.cpp                           wrapper Arduino setup/loop
src/royal_dash.cpp                     firmware Royal Dash: AP, UDP, K1G, RSA, 0F, RTP log
tools/capture_com.py                   serial logger
docs/current-working-firmware-restore.md tài liệu khôi phục này
docs/ios-royal-tripper-dash-design.md  thiết kế giao thức
```

## Không Được Đổi

Giữ nguyên các điểm này khi restore:

```text
SSID RE_1234_567890
Password 12345678
ESP IP 192.168.1.1
DHCP gateway 0.0.0.0
UDP bind 2000, 2002, 5000
Reply target iPhone :2002
Dual-source reply từ 2000 và 2002
RSA-1024 lưu NVS namespace k1g0722
Auth hint chỉ sau khi DHCP cấp IP hoặc sau packet 06/08
Secure metadata 0F01..0F0A sau auth OK
```

Không đổi gateway về `192.168.1.1` nếu cần iPhone vẫn có mạng 4G/5G. Khi ESP quảng bá gateway thật, iPhone có thể route internet vào ESP rồi Royal/Google Maps bị load mãi.

## Wi-Fi / DHCP

Firmware chạy SoftAP:

```text
WiFi.mode(WIFI_AP)
WiFi.setSleep(false)
WiFi.softAPConfig(192.168.1.1, 0.0.0.0, 255.255.255.0)
WiFi.softAP(RE_1234_567890, 12345678)
```

Routing mong muốn trên iPhone:

```text
192.168.1.0/24  -> Wi-Fi RE_* -> ESP dash
Internet        -> 4G/5G       -> nhà mạng
```

Log boot đúng:

```text
RSA RESTORED result=0 persisted=YES
AP READY ssid=RE_1234_567890 ip=192.168.1.1 broadcast=192.168.1.255 mac=...
MDNS READY
UDP READY port=2000
UDP READY port=2002
UDP READY port=5000
```

## UDP Layout

```text
UDP 2000  app -> ESP control K1G
UDP 2002  ESP -> app reply K1G; firmware cũng bind để bắt packet lệch source/dest
UDP 5000  app -> ESP RTP/H.264 stream
```

Firmware reply dual-source:

```text
ESP socket 2000 -> iPhone:2002
ESP socket 2002 -> iPhone:2002
```

Lý do: log iOS từng cho thấy packet app có thể đi qua `2000` hoặc `2002`; dual reply tương thích cả trạng thái device đã lưu và trạng thái pairing.

## K1G Envelope

Header:

```text
00 LEN 00 SEGCOUNT 00 00 00 00 02 01 00 05 4B 31 47 20 SEQ
```

Sau `SEQ` là TLV:

```text
TYPE SUB LEN_H LEN_L PAYLOAD...
```

Heartbeat/status app thường gửi:

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

## Discovery

Broadcast announce mỗi 1 giây:

```text
0018000200000000020100054B314720SEQ02060600030E3334
```

Khi thấy packet control từ iPhone, firmware gửi unicast announce:

```text
ANNOUNCE DUAL -> 192.168.1.2:2002
```

## Auth Handshake

Luồng đầy đủ:

```text
App -> ESP: 08/04 request RSA public key
ESP -> App: 07/00 RSA modulus + 07/03 exponent
App -> ESP: 08/00 RSA-PKCS1v1.5(SSID || AES-256 key)
ESP -> App: 07/01 01
ESP -> App: 0F01..0F0A encrypted vehicle metadata
```

Điểm sửa quan trọng nhất:

```text
Không gửi AUTH_PUBKEY ngay lúc WIFI_JOIN.
Chờ DHCP_LEASE hoặc packet 06/08 để biết peer IP thật.
Sau đó mới gửi 07/00 + 07/03 tới peer:2002.
```

Lý do: bản fail gửi pubkey trước DHCP nên log có:

```text
endPacket(): could not send data: 12
```

App chỉ gửi `06/08` rồi rời Wi-Fi, không đi tiếp tới `08/00`.

## RSA Identity

RSA phải ổn định giữa các lần reset/nạp:

```text
Key size:   1024 bit
Exponent:   65537
Padding:    RSA/ECB/PKCS1Padding compatible
NVS ns:     k1g0722
Stored:     n, e, d, p, q
```

Boot lần đầu:

```text
RSA GENERATED result=0 persisted=YES
```

Boot sau:

```text
RSA RESTORED result=0 persisted=YES
```

Không quay lại RSA runtime-only; app có thể cache device/key.

## Auth Hint

Auth hint chỉ chạy khi:

```text
authPeerReady == true
WiFi.softAPgetStationNum() > 0
authHintCount < 6
millis() - lastAuthHintMs >= 700
```

Nguồn `authPeer`:

```text
DHCP_LEASE ip=192.168.1.2
hoặc packet K1G thật từ iPhone
```

Khi thấy `06/08`, firmware gửi pubkey ngay một lần:

```text
TLV RX 06/08 len=1
AUTH_PUBKEY -> 192.168.1.2:2002
TX K1G -> 192.168.1.2:2002 len=156 first=0700
```

## AES / Secure Metadata

Plaintext app gửi trong `08/00`:

```text
ASCII SSID + 32-byte AES session key
```

Điều kiện hợp lệ:

```text
plainLength == strlen(AP_SSID) + 32
memcmp(plain, AP_SSID, strlen(AP_SSID)) == 0
```

Log đúng:

```text
TLV RX 08/00 len=128
AUTH session result=0 ssid=OK
```

Sau đó ESP gửi `07/01 01` và metadata `0F`.

`0Fxx` payload:

```text
IV(16 bytes) + AES-256-CBC-PKCS7(payload)
```

Metadata đang dùng:

```text
0F01 chassis:       NVD0000000000001
0F02 serial:        NVD-0001
0F03 model/name:    NAVDASH
0F05 bssid:         WiFi.softAPmacAddress()
0F06 manufacturing: 20260715
0F07 fw version:    0.0.0.1
0F08 part number:   NVD-K1G
0F09 region:        0x01
0F0A FOTA version:  00000001
```

Log đúng:

```text
SECURE_0F -> 192.168.1.2:2002 len=...
```

## Main Loop

Firmware hiện chạy một loop đơn giản:

```text
src/main.cpp
  setup() -> royal_dash::begin()
  loop()  -> royal_dash::update()

src/royal_dash.cpp
  royal_dash::update()
    sendBikeAnnounce()
    sendAuthHint()
    captureUdpPackets()
```

Không thêm task video/decode vào firmware handshake. H264/TFT chỉ thêm sau khi app lưu device ổn định.

## Luồng Thành Công Từ Đầu Đến Cuối

```text
1. ESP boot.
2. RSA load/generate từ NVS.
3. ESP bật AP RE_1234_567890.
4. DHCP local-only cấp IP cho iPhone, gateway 0.0.0.0.
5. iPhone join Wi-Fi nhưng internet vẫn đi 4G/5G.
6. Royal mở Tripper Dash/Add device.
7. App gửi K1G heartbeat/status hoặc 08/04 tới UDP 2000/2002.
8. ESP gửi announce dual tới iPhone:2002.
9. ESP gửi RSA pubkey 07/00 + 07/03 sau khi đã biết peer IP.
10. App gửi 08/00 chứa SSID + AES key đã RSA encrypt.
11. ESP decrypt, validate SSID.
12. ESP gửi 07/01 01.
13. ESP gửi 0F01..0F0A encrypt bằng AES session key.
14. App lưu/hiện device.
15. Khi bấm dẫn đường, app gửi RTP/H.264 tới UDP 5000.
```

## Test Sạch Trên iPhone

```text
1. Kill app Royal.
2. Forget Wi-Fi RE_1234_567890.
3. Bật cellular data.
4. Join Wi-Fi RE_1234_567890, password 12345678.
5. Mở Royal.
6. Vào Add/Connect Tripper Dash.
7. Chờ device hiện.
8. Bấm dẫn đường.
9. Xem log UDP 5000.
```

Nếu app vẫn giữ state sai, xóa device trong Royal hoặc cài lại app rồi test lại.

## Lệnh Build / Upload / Log

Build:

```powershell
python -m platformio run -e esp32dev
```

Upload:

```powershell
python -m platformio run -e esp32dev -t upload --upload-port COM4
```

Serial log:

```powershell
python tools\capture_com.py COM4 captures\manual.log
```

Nếu `COM4` bận:

```powershell
Get-CimInstance Win32_Process |
  Where-Object { $_.CommandLine -match 'COM4|capture_com|platformio.*monitor|pio.*device' } |
  Select-Object ProcessId,Name,CommandLine

Stop-Process -Id <PID> -Force
```

## Log Cần Có

Pairing đúng:

```text
WIFI_JOIN mac=...
DHCP_LEASE ip=192.168.1.2
PKT ... UDP 192.168.1.2:... -> 2000 len=...
TLV RX 06/08 len=1
AUTH_PUBKEY -> 192.168.1.2:2002
TLV RX 08/00 len=128
AUTH session result=0 ssid=OK
SECURE_0F -> 192.168.1.2:2002 len=...
```

Navigation/video đúng:

```text
PKT ... UDP 192.168.1.2:<random> -> 5000 len=1372 hex=8060...
PKT ... UDP 192.168.1.2:<random> -> 5000 len=60 hex=80E0...
```

## Evidence Đã Có

```text
captures/dhcp-baseline.log
  iPhone join, app control, route/RTP UDP 5000. Đây là path device đã lưu.

captures/route-start.log
  Royal gửi nhiều RTP/H.264 tới UDP 5000 khi dẫn đường.

captures/live-restore-b4.log
  Chứng minh lỗi cũ: AUTH_PUBKEY gửi trước DHCP, endPacket error 12, app leave.

captures/live-auth-after-dhcp.log
  Boot bản sửa: RSA RESTORED, UDP 2000/2002/5000 ready.
```

## Chẩn Đoán Lỗi

Không thấy Wi-Fi:

```text
Thiếu AP READY -> nạp lại hoặc reset board.
```

iPhone có Wi-Fi nhưng không có internet:

```text
Sai gateway. Phải là 0.0.0.0, không phải 192.168.1.1.
```

Royal báo failed to connect:

```text
Nếu có endPacket error 12 trước DHCP -> auth hint gửi quá sớm.
Nếu chỉ có 06/08 rồi WIFI_LEAVE -> app chưa nhận pubkey/auth path.
Nếu không có 08/00 -> chưa vào RSA session.
Nếu có 08/00 nhưng ssid=FAIL -> SSID trong app không khớp AP_SSID.
```

Không hiện device:

```text
Cần đủ AUTH session OK + SECURE_0F.
Nếu thiếu SECURE_0F, app có thể connect tạm nhưng không lưu device.
```

Không có route/video:

```text
Cần bấm dẫn đường trong Royal.
Cần log UDP 5000.
Nếu app load route mãi, kiểm tra iPhone còn dùng 4G/5G.
```

## Hướng Video Sau Khi Handshake Ổn

Royal gửi video qua RTP/H.264 UDP `5000`. Không dùng BLE, không có JSON route nhẹ đã xác nhận.

Với ESP32 không PSRAM:

```text
Không full-frame RGB565 double buffer.
Không decode H.264 full reference path trước.
Ưu tiên RTP inspector + IDR-only parser.
Cache nhẹ: Y4 + class2.
Render TFT bằng strip DMA.
```

Chỉ thêm video sau khi log pairing ổn định:

```text
AUTH session result=0 ssid=OK
SECURE_0F
PKT -> 5000
```

## Restore Nhanh

Không dùng `git reset --hard` nếu worktree đang có thay đổi chưa lưu. Cách an toàn:

```powershell
git status --short
python -m platformio run -e esp32dev
python -m platformio run -e esp32dev -t upload --upload-port COM4
python tools\capture_com.py COM4 captures\manual.log
```

Nếu cần khôi phục source từ lịch sử, ưu tiên checkpoint/commit có đủ các điểm:

```text
RSA NVS k1g0722
DHCP gateway 0.0.0.0
UDP 2000, 2002, 5000
ANNOUNCE DUAL
AUTH_PUBKEY sau DHCP hoặc 06/08
0F01..0F0A
```

Không restore nguyên bản `b4bc0d0` nếu mục tiêu là pairing mới, vì bản đó còn mô tả auth hint quá sớm và chưa phản ánh sửa lỗi `endPacket(): could not send data: 12`.

## Pairing Lock 2026-07-25

Baseline confirmed by live test: Royal iOS connects and saves the device.

Keep this invariant before adding HTTP, LCD, H264, BLE, NAT, or extra RTOS tasks:

```text
NAVDASH_ENABLE_LCD=0
NAVDASH_ENABLE_VIDEO=0
AP/DHCP/RSA/K1G code owns pairing.
Video/H264 is optional and registered by callback only.
royal_dash.cpp must not include navdash_video.h.
Auth hint starts only after DHCP_LEASE or real K1G peer.
Auth hint cadence: 300 ms.
Auth hint limit: 12 sends.
Reply target: peer:2002.
Reply source sockets: UDP 2000 and UDP 2002.
DHCP gateway: 0.0.0.0.
```

Current module boundary:

```text
src/main.cpp
  setup()
    optional navdash_lcd::begin() when NAVDASH_ENABLE_LCD=1
    optional navdash_video::begin() + royal_dash::setVideoPacketHandler() when NAVDASH_ENABLE_VIDEO=1
    royal_dash::begin()

  loop()
    royal_dash::update()
    optional navdash_video::update() when NAVDASH_ENABLE_VIDEO=1

src/royal_dash.cpp
  owns SoftAP, DHCP, UDP 2000/2002/5000 sockets, RSA NVS, K1G envelope, 07/00, 07/03, 07/01, 0F metadata.
  never calls H264 decode directly.
  forwards UDP 5000 only when a video callback is registered.
```

Hard restore check:

```powershell
python -m platformio run -e esp32dev
python -m platformio run -e esp32dev -t upload --upload-port COM4
python tools\capture_com.py COM4 captures\pairing-lock-check.log
```

Expected live log:

```text
WIFI_JOIN
DHCP_LEASE ip=192.168.1.2
AUTH_PUBKEY -> 192.168.1.2:2002
TLV RX 08/00 len=128
AUTH session result=0 ssid=OK
SECURE_0F -> 192.168.1.2:2002
```
