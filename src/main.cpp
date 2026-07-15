#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/rsa.h>

#include "config.h"

const IPAddress kDashIp(192, 168, 1, 1);
const IPAddress kNoDefaultGateway(0, 0, 0, 0);
const IPAddress kBroadcastIp(192, 168, 1, 255);
constexpr uint16_t kUdpPorts[] = {2000, 5000};
constexpr size_t kUdpPortCount = sizeof(kUdpPorts) / sizeof(kUdpPorts[0]);
constexpr size_t kControlSocket = 0;
constexpr uint8_t kBikeAnnounce[] = {0x00, 0x18, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
                                     0x02, 0x01, 0x00, 0x05, 0x4B, 0x31, 0x47, 0x20,
                                     0x02, 0x06, 0x06, 0x00, 0x03, 0x0E, 0x33, 0x34};

WiFiUDP udpSockets[kUdpPortCount];
bool udpStarted[kUdpPortCount];
uint32_t lastAnnounceMs;
uint8_t k1gSequence;
mbedtls_entropy_context entropy;
mbedtls_ctr_drbg_context drbg;
mbedtls_rsa_context rsa;

void sendBikeAnnounceToPeer(const IPAddress &peer);

bool sendEnvelope(const IPAddress &peer, const uint8_t *segments, size_t segmentsLength, uint16_t segmentCount) {
  uint8_t packet[256] = {};
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
  return udpSockets[kControlSocket].endPacket() == 1;
}

void handleK1g(const uint8_t *data, size_t length, const IPAddress &peer) {
  if (length < 21 || memcmp(data + 12, "K1G ", 4) != 0) return;
  size_t offset = 17;
  while (offset + 4 <= length) {
    const uint8_t type = data[offset], sub = data[offset + 1];
    const uint16_t payloadLength = (data[offset + 2] << 8) | data[offset + 3];
    offset += 4;
    if (offset + payloadLength > length) return;
    if (type == 0x08 && sub == 0x04) {
      uint8_t segments[139];
      segments[0] = 0x07; segments[1] = 0x00; segments[2] = 0; segments[3] = 128;
      mbedtls_mpi_write_binary(&rsa.N, segments + 4, 128);
      segments[132] = 0x07; segments[133] = 0x03; segments[134] = 0; segments[135] = 3;
      segments[136] = 0x01; segments[137] = 0x00; segments[138] = 0x01;
      Serial.printf("AUTH pubkey -> %s\n", peer.toString().c_str());
      sendEnvelope(peer, segments, sizeof(segments), 2);
    } else if (type == 0x08 && sub == 0x00 && payloadLength == 128) {
      uint8_t plain[160]; size_t plainLength = 0;
      const int result = mbedtls_rsa_pkcs1_decrypt(&rsa, mbedtls_ctr_drbg_random, &drbg, MBEDTLS_RSA_PRIVATE,
                                                    &plainLength, data + offset, plain, sizeof(plain));
      const size_t ssidLength = strlen(AP_SSID);
      const bool valid = result == 0 && plainLength == ssidLength + 32 && memcmp(plain, AP_SSID, ssidLength) == 0;
      const uint8_t auth[] = {0x07, 0x01, 0x00, 0x01, static_cast<uint8_t>(valid)};
      Serial.printf("AUTH session result=%d ssid=%s\n", result, valid ? "OK" : "FAIL");
      sendEnvelope(peer, auth, sizeof(auth), 1);
    }
    offset += payloadLength;
  }
}

void logWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
    const uint8_t *mac = info.wifi_ap_staconnected.mac;
    Serial.printf("WIFI_JOIN mac=%02X:%02X:%02X:%02X:%02X:%02X aid=%u\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  info.wifi_ap_staconnected.aid);
  } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
    const uint8_t *mac = info.wifi_ap_stadisconnected.mac;
    Serial.printf("WIFI_LEAVE mac=%02X:%02X:%02X:%02X:%02X:%02X aid=%u\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  info.wifi_ap_stadisconnected.aid);
    Serial.printf("WIFI_EVENT code=%d stations=%u\n", event, WiFi.softAPgetStationNum());
  } else if (event == ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED) {
    IPAddress assigned(info.wifi_ap_staipassigned.ip.addr);
    Serial.printf("DHCP_LEASE ip=%s\n", assigned.toString().c_str());
  }
}

void printPacket(uint16_t port, const IPAddress &remote, uint16_t remotePort, const uint8_t *data, size_t length) {
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
      if (kUdpPorts[index] == 2000) {
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
  Serial.printf("ANNOUNCE 2000 -> %s:2002\n", peer.toString().c_str());
}

void setup() {
  Serial.begin(115200);
  WiFi.onEvent(logWiFiEvent);
  mbedtls_entropy_init(&entropy); mbedtls_ctr_drbg_init(&drbg); mbedtls_rsa_init(&rsa, MBEDTLS_RSA_PKCS_V15, 0);
  const char *personalization = "navdash-k1g";
  const int seedResult = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                                reinterpret_cast<const uint8_t *>(personalization), strlen(personalization));
  const int keyResult = seedResult == 0 ? mbedtls_rsa_gen_key(&rsa, mbedtls_ctr_drbg_random, &drbg, 1024, 65537) : seedResult;
  Serial.printf("RSA READY result=%d\n", keyResult);
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

void loop() {
  sendBikeAnnounce();
  captureUdpPackets();
}
