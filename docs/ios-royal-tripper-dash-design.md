# NavDash iOS Royal Tripper Dash Design

## Mục tiêu

Biến ESP32-D0WD-V3 thành Tripper Dash Wi-Fi giả lập cho app Royal Enfield iOS:

- iPhone join Wi-Fi `RE_1234_567890`.
- App Royal thấy dash như thiết bị K1G.
- Kênh điều khiển chạy UDP `2000`/`2002`.
- Kênh video H.264/RTP nhận ở UDP `5000`.
- Internet của iPhone vẫn đi 4G/5G nhờ DHCP local-only.

## Phần cứng

- MCU: ESP32-D0WD-V3, dual-core Xtensa LX6, 240 MHz.
- Flash: 4 MB.
- RAM: SRAM nội, không PSRAM.
- Upload hiện dùng `COM4`.
- PlatformIO env: `esp32dev`.

## Wi-Fi

Firmware tạo SoftAP:

- SSID: `RE_1234_567890`.
- Password: `12345678`.
- Dash IP: `192.168.1.1`.
- Client thường nhận: `192.168.1.2`.
- DHCP gateway: `0.0.0.0`.

Lý do gateway `0.0.0.0`:

- iPhone chỉ route mạng `192.168.1.0/24` qua Wi-Fi dash.
- Internet/Google Maps/Royal backend vẫn đi cellular 4G/5G.
- Không cần NAT, STA uplink, điện thoại thứ hai.

## UDP Layout

| Port | Hướng | Vai trò |
| --- | --- | --- |
| `2000` | app → ESP | K1G control, heartbeat, auth request |
| `2002` | ESP → app | K1G reply/announce/auth; firmware cũng bind để bắt packet lệch port |
| `5000` | app → ESP | RTP/H.264 stream |

Firmware gửi reply dual-source qua socket `2000` và `2002` để tương thích trạng thái app iOS.

## K1G Envelope

Header K1G đang dùng:

```text
00 LEN  00 SEGCOUNT  00 00 00 00  02 01 00 05  4B 31 47 20  SEQ
```

Sau byte `SEQ` là các TLV:

```text
TYPE SUB LENGTH_H LENGTH_L PAYLOAD...
```

## Discovery

Firmware broadcast định kỳ:

```text
00 18 00 02 00 00 00 00 02 01 00 05 4B 31 47 20 SEQ
02 06 06 00 03 0E 33 34
```

Ngoài broadcast, firmware reply unicast mỗi khi thấy packet từ iPhone.

## Auth Handshake

Luồng chuẩn:

```text
App → ESP: 08/04
ESP → App: 07/00 RSA modulus + 07/03 exponent
App → ESP: 08/00 RSA-PKCS1v1.5(ssid || aes_key)
ESP → App: 07/01 01
ESP → App: 0F01..0F0A secure metadata
```

Firmware hiện:

- Sinh RSA-1024 runtime khi boot.
- Trả public key qua `07/00` và `07/03`.
- Decrypt `08/00`.
- Validate plaintext = `AP_SSID || 32-byte AES key`.
- Lưu AES key RAM.
- Gửi `07/01 01` nếu SSID đúng.
- Gửi secure vehicle metadata `0F01..0F0A`.

## Auth Hint

iOS có lúc không gửi `08/04` khi app đang ở trạng thái device/heartbeat. Firmware gửi chủ động `07/00 + 07/03` sau khi iPhone join:

- Tối đa 12 lần.
- Cách nhau 300 ms.
- Đích mặc định `192.168.1.2:2002`.

Mục tiêu: kích app có đủ RSA public key để tiếp tục `08/00`.

## Secure Vehicle Metadata

Sau auth OK, firmware gửi một K1G packet chứa đủ các TLV app cần để lưu device:

| TLV | Ý nghĩa |
| --- | --- |
| `0F01` | chassis number |
| `0F02` | serial number |
| `0F03` | model/name |
| `0F05` | BSSID |
| `0F06` | manufacturing date |
| `0F07` | hardware/firmware version |
| `0F08` | part number |
| `0F09` | region |
| `0F0A` | FOTA version |

Payload `0Fxx` dùng:

```text
IV(16 bytes) + AES-256-CBC-PKCS7(payload)
```

AES key lấy từ gói `08/00`.

## Logs quan trọng

Log thành công nền:

- `captures/dhcp-baseline.log`: iPhone join, app gửi control, sau đó stream UDP `5000`.
- `captures/route-start.log`: xác nhận RTP/H.264 đã về ESP.

Log debug mới:

- `TLV RX 08/04`: app hỏi RSA.
- `AUTH_PUBKEY`: ESP gửi `07/00 + 07/03`.
- `AUTH session result=0 ssid=OK`: decrypt OK.
- `SECURE_0F`: ESP đã gửi metadata lưu device.
- `PKT ... -> 5000`: video H.264/RTP đang tới.

## Test flow

1. Upload:

```powershell
python -m platformio run -e esp32dev -t upload --upload-port COM4
```

2. Monitor:

```powershell
python tools\capture_com.py COM4 captures\manual.log
```

3. iPhone:

- Forget Wi-Fi `RE_1234_567890`.
- Join lại, password `12345678`.
- Bật cellular data.
- Mở Royal.
- Add/Connect Tripper Dash.
- Bấm dẫn đường.

## Kiến trúc video hiện tại

Firmware hiện mới nhận/log RTP/H.264 ở UDP `5000`; chưa decode lên TFT.

Hướng decode phù hợp cho ESP32 không PSRAM:

- Không full-frame RGB565 double buffer.
- Decode map-specialized.
- IDR-only làm ảnh chuẩn.
- P-frame chỉ parse motion vector để dịch cache.
- Cache map dạng `Y4 + class2`.
- Render TFT bằng strip DMA.

## Giới hạn

- RSA runtime chưa persistent; nếu app lưu key theo device lâu dài, cần NVS identity.
- `0Fxx` metadata đang dùng giá trị giả lập ổn định, chưa lấy từ dash thật.
- H.264 decoder chưa triển khai.
- iOS pairing vẫn phụ thuộc app state; nếu không có `08/00`, phải reset app/Wi-Fi state hoặc capture thêm.

## File chính

- Firmware: `src/main.cpp`.
- Wi-Fi config: `include/config.h`.
- Serial capture: `tools/capture_com.py`.
- Design note: `docs/ios-royal-tripper-dash-design.md`.
