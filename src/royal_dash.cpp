#include <Arduino.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <mbedtls/aes.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/rsa.h>

#include "config.h"
#include "royal_dash.h"

namespace royal_dash {

const IPAddress kDashIp(192, 168, 1, 1);
const IPAddress kNoDefaultGateway(0, 0, 0, 0);
const IPAddress kBroadcastIp(192, 168, 1, 255);
constexpr uint16_t kUdpPorts[] = {2000, 2002, 5000};
constexpr size_t kUdpPortCount = sizeof(kUdpPorts) / sizeof(kUdpPorts[0]);
constexpr size_t kControlSocket = 0;
constexpr size_t kReplySocket = 1;
constexpr uint32_t kAuthHintIntervalMs = 300;
constexpr uint8_t kAuthHintLimit = 12;
constexpr uint8_t kBikeAnnounce[] = {0x00, 0x18, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
                                     0x02, 0x01, 0x00, 0x05, 0x4B, 0x31, 0x47, 0x20,
                                     0x02, 0x06, 0x06, 0x00, 0x03, 0x0E, 0x33, 0x34};

WiFiUDP udpSockets[kUdpPortCount];
bool udpStarted[kUdpPortCount];
uint32_t lastAnnounceMs;
uint32_t lastAuthHintMs;
uint32_t lastStatusMs;
uint8_t k1gSequence;
uint8_t authHintCount;
IPAddress authPeer;
bool authPeerReady;
bool authPeerHadControl;
mbedtls_entropy_context entropy;
mbedtls_ctr_drbg_context drbg;
mbedtls_rsa_context rsa;
uint8_t sessionKey[32];
bool sessionKeyReady;
VideoPacketHandler videoPacketHandler;

void sendBikeAnnounceToPeer(const IPAddress &peer);
void sendAuthPubkey(const IPAddress &peer);
void sendVehicleSecureData(const IPAddress &peer);

void setVideoPacketHandler(VideoPacketHandler handler) {
  videoPacketHandler = handler;
}

bool loadRsaIdentity(Preferences &preferences) {
  uint8_t n[128], e[3], d[128], p[64], q[64];
  if (preferences.getBytesLength("n") != sizeof(n) || preferences.getBytesLength("e") != sizeof(e) ||
      preferences.getBytesLength("d") != sizeof(d) || preferences.getBytesLength("p") != sizeof(p) ||
      preferences.getBytesLength("q") != sizeof(q) || preferences.getBytes("n", n, sizeof(n)) != sizeof(n) ||
      preferences.getBytes("e", e, sizeof(e)) != sizeof(e) || preferences.getBytes("d", d, sizeof(d)) != sizeof(d) ||
      preferences.getBytes("p", p, sizeof(p)) != sizeof(p) || preferences.getBytes("q", q, sizeof(q)) != sizeof(q)) return false;
  return mbedtls_rsa_import_raw(&rsa, n, sizeof(n), p, sizeof(p), q, sizeof(q), d, sizeof(d), e, sizeof(e)) == 0 &&
         mbedtls_rsa_complete(&rsa) == 0 && mbedtls_rsa_check_privkey(&rsa) == 0;
}

bool writeMpi(Preferences &preferences, const char *key, const mbedtls_mpi *value, size_t length) {
  uint8_t bytes[128] = {};
  return length <= sizeof(bytes) && mbedtls_mpi_write_binary(value, bytes, length) == 0 &&
         preferences.putBytes(key, bytes, length) == length;
}

bool saveRsaIdentity(Preferences &preferences) {
  return writeMpi(preferences, "n", &rsa.N, 128) && writeMpi(preferences, "e", &rsa.E, 3) &&
         writeMpi(preferences, "d", &rsa.D, 128) && writeMpi(preferences, "p", &rsa.P, 64) &&
         writeMpi(preferences, "q", &rsa.Q, 64);
}

bool sendEnvelope(const IPAddress &peer, const uint8_t *segments, size_t segmentsLength, uint16_t segmentCount) {
  uint8_t packet[640] = {};
  const size_t length = 17 + segmentsLength;
  if (length > sizeof(packet)) return false;
  packet[0] = length >> 8; packet[1] = length;
  packet[2] = 0; packet[3] = segmentCount + 1;
  packet[8] = 0x02; packet[9] = 0x01; packet[10] = 0; packet[11] = 0x05;
  memcpy(packet + 12, "K1G ", 4); packet[16] = k1gSequence++;
  memcpy(packet + 17, segments, segmentsLength);
  Serial.printf("TX K1G -> %s:2002 len=%u first=%02X%02X\n", peer.toString().c_str(), length,
                segmentsLength > 0 ? segments[0] : 0, segmentsLength > 1 ? segments[1] : 0);

  udpSockets[kControlSocket].beginPacket(peer, 2002);
  udpSockets[kControlSocket].write(packet, length);
  const bool controlSent = udpSockets[kControlSocket].endPacket() == 1;
  udpSockets[kReplySocket].beginPacket(peer, 2002);
  udpSockets[kReplySocket].write(packet, length);
  return udpSockets[kReplySocket].endPacket() == 1 || controlSent;
}

void handleK1g(const uint8_t *data, size_t length, const IPAddress &peer) {
  if (length < 21 || memcmp(data + 12, "K1G ", 4) != 0) return;
  size_t offset = 17;
  while (offset + 4 <= length) {
    const uint8_t type = data[offset], sub = data[offset + 1];
    const uint16_t payloadLength = (data[offset + 2] << 8) | data[offset + 3];
    offset += 4;
    if (offset + payloadLength > length) return;
    Serial.printf("TLV RX %02X/%02X len=%u\n", type, sub, payloadLength);
    if (type == 0x08 && sub == 0x04) {
      sendAuthPubkey(peer);
    } else if (type == 0x06 && sub == 0x08 && authHintCount < kAuthHintLimit) {
      authPeer = peer;
      authPeerReady = true;
      ++authHintCount;
      sendAuthPubkey(peer);
    } else if (type == 0x08 && sub == 0x00 && payloadLength == 128) {
      uint8_t plain[160]; size_t plainLength = 0;
      const int result = mbedtls_rsa_pkcs1_decrypt(&rsa, mbedtls_ctr_drbg_random, &drbg, MBEDTLS_RSA_PRIVATE,
                                                    &plainLength, data + offset, plain, sizeof(plain));
      const size_t ssidLength = strlen(AP_SSID);
      const bool valid = result == 0 && plainLength == ssidLength + 32 && memcmp(plain, AP_SSID, ssidLength) == 0;
      const uint8_t auth[] = {0x07, 0x01, 0x00, 0x01, static_cast<uint8_t>(valid)};
      Serial.printf("AUTH session result=%d ssid=%s\n", result, valid ? "OK" : "FAIL");
      if (valid) {
        memcpy(sessionKey, plain + ssidLength, sizeof(sessionKey));
        sessionKeyReady = true;
      }
      sendEnvelope(peer, auth, sizeof(auth), 1);
      if (valid) sendVehicleSecureData(peer);
    }
    offset += payloadLength;
  }
}

size_t appendSecureTlv(uint8_t *out, size_t offset, uint8_t sub, const uint8_t *plain, size_t plainLength) {
  if (!sessionKeyReady || plainLength > 31) return offset;
  uint8_t iv[16];
  uint8_t block[48] = {};
  const size_t paddedLength = ((plainLength / 16) + 1) * 16;
  const uint8_t padding = paddedLength - plainLength;
  mbedtls_ctr_drbg_random(&drbg, iv, sizeof(iv));
  memcpy(block, plain, plainLength);
  memset(block + plainLength, padding, padding);

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, sessionKey, 256);
  uint8_t cbcIv[16];
  memcpy(cbcIv, iv, sizeof(cbcIv));
  mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, paddedLength, cbcIv, block, block);
  mbedtls_aes_free(&aes);

  out[offset++] = 0x0F;
  out[offset++] = sub;
  out[offset++] = 0;
  out[offset++] = 16 + paddedLength;
  memcpy(out + offset, iv, sizeof(iv));
  offset += sizeof(iv);
  memcpy(out + offset, block, paddedLength);
  return offset + paddedLength;
}

size_t appendSecureText(uint8_t *out, size_t offset, uint8_t sub, const char *text) {
  return appendSecureTlv(out, offset, sub, reinterpret_cast<const uint8_t *>(text), strlen(text));
}

void sendVehicleSecureData(const IPAddress &peer) {
  uint8_t mac[6];
  uint8_t segments[512];
  WiFi.softAPmacAddress(mac);
  size_t offset = 0;
  offset = appendSecureText(segments, offset, 0x01, "NVD0000000000001");
  offset = appendSecureText(segments, offset, 0x02, "NVD-0001");
  offset = appendSecureText(segments, offset, 0x03, "NAVDASH");
  offset = appendSecureTlv(segments, offset, 0x05, mac, sizeof(mac));
  offset = appendSecureText(segments, offset, 0x06, "20260715");
  offset = appendSecureText(segments, offset, 0x07, "0.0.0.1");
  offset = appendSecureText(segments, offset, 0x08, "NVD-K1G");
  const uint8_t region[] = {0x01};
  offset = appendSecureTlv(segments, offset, 0x09, region, sizeof(region));
  offset = appendSecureText(segments, offset, 0x0A, "00000001");
  Serial.printf("SECURE_0F -> %s:2002 len=%u\n", peer.toString().c_str(), offset);
  sendEnvelope(peer, segments, offset, 9);
}

void logWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
    const uint8_t *mac = info.wifi_ap_staconnected.mac;
    Serial.printf("WIFI_JOIN mac=%02X:%02X:%02X:%02X:%02X:%02X aid=%u\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  info.wifi_ap_staconnected.aid);
    authHintCount = 0;
    authPeerReady = false;
    authPeerHadControl = false;
    lastAuthHintMs = millis();
  } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
    const uint8_t *mac = info.wifi_ap_stadisconnected.mac;
    Serial.printf("WIFI_LEAVE mac=%02X:%02X:%02X:%02X:%02X:%02X aid=%u\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  info.wifi_ap_stadisconnected.aid);
    Serial.printf("WIFI_EVENT code=%d stations=%u\n", event, WiFi.softAPgetStationNum());
    authPeerReady = false;
  } else if (event == ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED) {
    IPAddress assigned(info.wifi_ap_staipassigned.ip.addr);
    authPeer = assigned;
    authPeerReady = true;
    authHintCount = 0;
    lastAuthHintMs = millis() - kAuthHintIntervalMs;
    Serial.printf("DHCP_LEASE ip=%s\n", assigned.toString().c_str());
  }
}

void printPacket(uint16_t port, const IPAddress &remote, uint16_t remotePort, const uint8_t *data, size_t length) {
  if (port == 5000) {
    if (videoPacketHandler) {
      videoPacketHandler(remote, remotePort, data, length);
    }
    return;
  }
  Serial.printf("PKT %lu UDP %s:%u -> %u len=%u hex=", millis(), remote.toString().c_str(), remotePort, port, length);
  for (size_t index = 0; index < length; ++index) {
    Serial.printf("%02X", data[index]);
  }
  Serial.println();
}

void captureUdpPackets() {
  uint8_t packet[1472];
  for (size_t index = 0; index < kUdpPortCount; ++index) {
    if (!udpStarted[index]) {
      continue;
    }
    const int length = udpSockets[index].parsePacket();
    if (length <= 0) {
      continue;
    }
    const size_t received = udpSockets[index].read(packet, min(static_cast<size_t>(length), sizeof(packet)));
    if (received > 0) {
      printPacket(kUdpPorts[index], udpSockets[index].remoteIP(), udpSockets[index].remotePort(), packet, received);
      if (kUdpPorts[index] == 2000 || kUdpPorts[index] == 2002) {
        authPeer = udpSockets[index].remoteIP();
        authPeerReady = true;
        authPeerHadControl = true;
        sendBikeAnnounceToPeer(udpSockets[index].remoteIP());
        handleK1g(packet, received, udpSockets[index].remoteIP());
      }
    }
  }
}
void sendBikeAnnounce() {
  if (!udpStarted[0] || millis() - lastAnnounceMs < 1000) {
    return;
  }
  udpSockets[0].beginPacket(kBroadcastIp, kUdpPorts[0]);
  udpSockets[0].write(kBikeAnnounce, sizeof(kBikeAnnounce));
  udpSockets[0].endPacket();
  lastAnnounceMs = millis();
}

void sendBikeAnnounceToPeer(const IPAddress &peer) {
  uint8_t announce[sizeof(kBikeAnnounce)];
  memcpy(announce, kBikeAnnounce, sizeof(announce));
  announce[16] = k1gSequence++;
  udpSockets[kControlSocket].beginPacket(peer, 2002);
  udpSockets[kControlSocket].write(announce, sizeof(announce));
  udpSockets[kControlSocket].endPacket();
  udpSockets[kReplySocket].beginPacket(peer, 2002);
  udpSockets[kReplySocket].write(announce, sizeof(announce));
  udpSockets[kReplySocket].endPacket();
  Serial.printf("ANNOUNCE DUAL -> %s:2002\n", peer.toString().c_str());
}

void sendAuthPubkey(const IPAddress &peer) {
  uint8_t segments[139];
  segments[0] = 0x07; segments[1] = 0x00; segments[2] = 0; segments[3] = 128;
  mbedtls_mpi_write_binary(&rsa.N, segments + 4, 128);
  segments[132] = 0x07; segments[133] = 0x03; segments[134] = 0; segments[135] = 3;
  segments[136] = 0x01; segments[137] = 0x00; segments[138] = 0x01;
  Serial.printf("AUTH_PUBKEY -> %s:2002\n", peer.toString().c_str());
  sendEnvelope(peer, segments, sizeof(segments), 2);
}

void sendAuthHint() {
  if (!udpStarted[kControlSocket] || !authPeerReady || WiFi.softAPgetStationNum() == 0 || authHintCount >= kAuthHintLimit ||
      millis() - lastAuthHintMs < kAuthHintIntervalMs) {
    return;
  }
  lastAuthHintMs = millis();
  ++authHintCount;
  sendAuthPubkey(authPeer);
}

void logStatus() {
  if (millis() - lastStatusMs < 1000) return;
  lastStatusMs = millis();
  Serial.printf("STATUS sta=%u peer=%s control=%u hint=%u heap=%u\n", WiFi.softAPgetStationNum(),
                authPeerReady ? authPeer.toString().c_str() : "none", authPeerHadControl ? 1 : 0, authHintCount,
                ESP.getFreeHeap());
}

void begin() {
  Serial.begin(115200);
  WiFi.onEvent(logWiFiEvent);
  mbedtls_entropy_init(&entropy); mbedtls_ctr_drbg_init(&drbg); mbedtls_rsa_init(&rsa, MBEDTLS_RSA_PKCS_V15, 0);
  const char *personalization = "navdash-k1g";
  const int seedResult = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                                reinterpret_cast<const uint8_t *>(personalization), strlen(personalization));
  Preferences preferences;
  preferences.begin("k1g0722", false);
  const bool loaded = seedResult == 0 && loadRsaIdentity(preferences);
  const int keyResult = loaded ? 0 : (seedResult == 0 ? mbedtls_rsa_gen_key(&rsa, mbedtls_ctr_drbg_random, &drbg, 1024, 65537) : seedResult);
  const bool saved = keyResult == 0 && (loaded || saveRsaIdentity(preferences));
  preferences.end();
  Serial.printf("RSA %s result=%d persisted=%s\n", loaded ? "RESTORED" : "GENERATED", keyResult, saved ? "YES" : "NO");
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAPConfig(kDashIp, kNoDefaultGateway, IPAddress(255, 255, 255, 0));
  const bool started = WiFi.softAP(AP_SSID, AP_PASSWORD[0] == '\0' ? nullptr : AP_PASSWORD);
  Serial.printf("AP %s ssid=%s ip=%s broadcast=192.168.1.255 mac=%s\n", started ? "READY" : "FAILED", AP_SSID,
                WiFi.softAPIP().toString().c_str(), WiFi.softAPmacAddress().c_str());

  const bool mdnsStarted = MDNS.begin("reprime");
  Serial.printf("MDNS %s\n", mdnsStarted ? "READY" : "FAILED");
  if (mdnsStarted) {
    MDNS.addService("royalenfield", "udp", 2000);
    MDNS.addService("reprime", "udp", 2000);
    MDNS.addService("lnp", "udp", 2000);
  }
  for (size_t index = 0; index < kUdpPortCount; ++index) {
    udpStarted[index] = udpSockets[index].begin(kUdpPorts[index]);
    Serial.printf("UDP %s port=%u\n", udpStarted[index] ? "READY" : "FAILED", kUdpPorts[index]);
  }
}

void update() {
  sendBikeAnnounce();
  sendAuthHint();
  captureUdpPackets();
  logStatus();
}

}  // namespace royal_dash
