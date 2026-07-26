# NavDash RTOS Architecture

## Priority

Royal AP, DHCP, K1G, RSA, and control UDP are more important than video. Video may drop frames or stop. Control must remain alive.

## Tasks

```text
Core 0: Wi-Fi/lwIP system tasks
Core 1: Arduino control loop, priority 5
Core 1: dash-video worker, priority 2, 4608 byte stack
```

The connection facade owns the only public path into Royal pairing. Feature code uses `navdash_connection.h`; it must not include `royal_dash.h`.

The control loop owns `WiFiUDP`. UDP 5000 only reserves a fixed RTP slot, copies one packet, and queues a one-byte slot index. It never parses or decodes H.264.

## Lifecycle

```text
boot
  -> RSA/NVS
  -> SoftAP + DHCP gateway 0.0.0.0
  -> UDP 2000, 2002, 5000
  -> LCD init
  -> wait AUTH OK + 8 s save grace
  -> allocate video context
  -> start video worker

disconnect or low memory
  -> unregister RTP handler
  -> request worker stop
  -> worker exits at packet or macroblock-row boundary
  -> free video context
  -> control-only mode
```

Video allocation failure backs off for 30 seconds. It does not reboot the ESP32 and it does not change the Royal handshake.

## Double Buffer

```text
Front buffer: ILI9341 GRAM, RGB565, outside ESP SRAM
Back buffer:  ESP Y4 + class2, 240x240, 43,200 bytes
```

The decoder writes only the back buffer. The LCD is updated only after all 627 source macroblocks decode successfully. While the next frame is decoding, ILI9341 GRAM retains the previous complete frame.

SPI transfers 8-line blocks because ILI9341 is serial, but a partial decoded frame is never presented. The current driver uses one 3,840-byte DMA-capable tile and synchronous SPI transfer. Add a second tile only when the LCD driver is migrated to queued asynchronous DMA; a second tile has no benefit with the current blocking API.

## Video RAM

```text
Y4 + class2 back frame      43,200 B
rolling two-row YUV         25,344 B
macroblock state            16,728 B
RTP slots, 6 x 1472 B        8,832 B
8-line RGB565 present tile   3,840 B
live NAL                    12,288 B
video task stack             4,608 B
```

All large video memory is allocated after AUTH OK plus an 8-second save grace and freed on disconnect or emergency memory pressure. No video memory exists before auth.

The current video context allocates approximately 110 KB dynamically after the grace period. The compile-time image uses 48,672 B RAM in `esp32dev_lcd_video`; the live heap guard remains the decision point.

## Memory Safety

```text
free >= 100 KB: decode allowed
free < 100 KB:  drop/defer decode
free < 80 KB:   stop video and return memory to control
```

Logs must include `heap`, `min`, `stack`, `qdrop`, and queue depth. Do not increase RTP slots, NAL capacity, or task stack without a measured reason.

## Invariants

```text
SSID RE_1234_567890
DHCP gateway 0.0.0.0
UDP 2000/2002/5000
RSA NVS namespace k1g0722
ANNOUNCE DUAL
AUTH_PUBKEY after DHCP_LEASE or 06/08
auth hint 700 ms, limit 6
video starts only after AUTH OK
video starts only after AUTH OK + 8 s save grace
decoder never runs in royal_dash::captureUdpPackets()
```
