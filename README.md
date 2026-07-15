# navdash capture probe

ESP32 SoftAP + K1G packet logger for Royal Enfield iOS research.

## Evidence

`ipa/Royal Enfield 10.1.21.ipa` is bundle `com.royalenfield.reprime`. Static symbols include `NEHotspotConfiguration`, `SSID`, `NetworkExtension`, `H264`, `RTP`, and `RTSP`. These are leads, not protocol proof.

## Flash

1. Install PlatformIO Core.
2. Set the Tripper-format SSID `RE_XXXX_XXXXXX` and WPA2 password in `include/config.h`. Factory password: `12345678`.
3. Run `pio run -t upload`.
4. Run `pio device monitor`.
5. In Royal Enfield iOS app, start Tripper/Dash pairing or navigation.
6. Save serial output to `captures/session-YYYYMMDD.log`.
7. Run `python tools/decode_capture.py < captures/session-YYYYMMDD.log`.

## Current protocol boundary

The dash AP is `192.168.1.1/24`; iPhone control traffic targets UDP `2000` and H.264/RTP targets UDP `5000`. The iPhone listens on UDP `2002` for dash replies. Firmware only receives/logs `2000` and `5000`; it does not send unproven replies.

The firmware broadcasts the K1G announce on UDP `2000`; replies originate from UDP `2000` and target the iPhone's UDP `2002`. It handles only the observed RSA handshake: `08/04` → `07/00` + `07/03`, then `08/00` → `07/01`.

The firmware generates a runtime RSA key at boot. This matches the captured working baseline; persistent identity is deferred until a pairing capture proves it is required.

## Next evidence

Record one clean session: AP join, first connection, navigation start, one route instruction, navigation stop. Add observed ports before attempting protocol emulation.
