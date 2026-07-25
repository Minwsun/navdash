# NavDash Lightweight Video RTOS Path

## Current Milestone

Implemented first safe video step:

```text
AP/pairing path: unchanged
UDP 5000: no full hex dump
Video cache: Y4 + class2 for 240x180
RAM added: 32,400 bytes static cache
Build RAM: 80,560 / 327,680 bytes
```

`navdash_video` currently does RTP/Royal-H264 live inspection only. It does not decode H.264 yet.

## Runtime Flow

```text
royal_dash UDP poll
  -> UDP 2000/2002 K1G control
  -> UDP 5000 navdash_video::handlePacket()
       -> RTP header parse
       -> sequence gap count
       -> payload type 96 count
       -> Royal wrapper/NAL start count
       -> 1 second stats log
```

Control packets still print hex for handshake debugging. Video packets only update counters.

## Lightweight Frame Format

Target display:

```text
ILI9341V 4SPI
240x180
```

Internal cache:

```text
Y4      240 * 180 / 2 = 21,600 bytes
class2  240 * 180 / 4 = 10,800 bytes
total                 = 32,400 bytes
```

Classes:

```text
0 background/map
1 gray road
2 white text/line
3 route blue/accent
```

Render rule for the next stage:

```text
RGB565 = palette[class2][Y4]
```

## Next Milestones

1. Add ILI9341V pin config and LCD strip driver.
2. Render static `Y4 + class2` test pattern to confirm SPI path.
3. Add RTP Access Unit slot pool without copying full IDR.
4. Add SPS/PPS/slice-header parser gate:
   - Baseline profile 66 only.
   - CAVLC only.
   - FMO off only.
   - IDR only.
5. Port minimum H264BSD IDR intra path.
6. Decode one IDR sample from flash to grayscale.
7. Decode live latest IDR only; drop backlog.

## Hard Rules

```text
No video malloc after boot
No full RGB framebuffer
No full YUV framebuffer
No P-frame path on D0WD v1
No UDP 5000 hex spam
Video drops before control blocks
Heap floor target >= 60 KB internal
```

