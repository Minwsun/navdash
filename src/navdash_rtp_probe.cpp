#include "navdash_rtp_probe.h"

namespace navdash_rtp_probe {

namespace {

struct Stats {
  uint32_t packets;
  uint32_t bytes;
  uint32_t pt96;
  uint32_t marker;
  uint32_t badRtp;
  uint32_t sps;
  uint32_t pps;
  uint32_t idr;
  uint32_t sei;
  uint32_t royalStart;
  uint32_t royalContinuation;
  uint32_t rawNal;
  uint32_t unknown;
  uint32_t seqGap;
  uint32_t timestampChanges;
  uint16_t lastSequence;
  uint32_t lastTimestamp;
  bool haveSequence;
  bool haveTimestamp;
  uint32_t lastLogMs;
};

Stats stats;

size_t payloadOffset(const uint8_t *data, size_t length) {
  if (length < 12 || (data[0] >> 6) != 2) return 0;
  size_t offset = 12 + static_cast<size_t>(data[0] & 0x0F) * 4;
  if (offset > length) return 0;
  if (data[0] & 0x10) {
    if (offset + 4 > length) return 0;
    offset += 4 + (static_cast<size_t>(data[offset + 2]) << 8 | data[offset + 3]) * 4;
  }
  if (offset >= length) return 0;
  if (data[0] & 0x20) {
    const uint8_t padding = data[length - 1];
    if (padding == 0 || padding > length - offset) return 0;
  }
  return offset;
}

uint16_t readU16(const uint8_t *data) {
  return static_cast<uint16_t>(data[2] << 8 | data[3]);
}

uint32_t readU32(const uint8_t *data) {
  return (static_cast<uint32_t>(data[4]) << 24) | (static_cast<uint32_t>(data[5]) << 16) |
         (static_cast<uint32_t>(data[6]) << 8) | data[7];
}

void countNal(uint8_t type) {
  if (type == 5) ++stats.idr;
  else if (type == 6) ++stats.sei;
  else if (type == 7) ++stats.sps;
  else if (type == 8) ++stats.pps;
  else if (type != 0) ++stats.unknown;
}

void countAnnexB(const uint8_t *payload, size_t length) {
  for (size_t index = 0; index + 4 < length; ++index) {
    if (payload[index] == 0 && payload[index + 1] == 0 && payload[index + 2] == 0 && payload[index + 3] == 1) {
      countNal(payload[index + 4] & 0x1F);
      index += 4;
    }
  }
}

void trackRoyalPayload(const uint8_t *payload, size_t length) {
  if (length == 0) return;
  if (length >= 2 && payload[0] == 0x3C && payload[1] == 0x87) {
    ++stats.royalStart;
    countNal(7);  // Royal wrapper: implicit SPS header 0x27.
    countAnnexB(payload + 2, length - 2);
    return;
  }
  if (length >= 2 && payload[0] == 0x3C && payload[1] == 0x07) {
    ++stats.royalContinuation;
    return;
  }
  if (payload[0] == 0x27 || payload[0] == 0x28 || payload[0] == 0x25 || payload[0] == 0x26) {
    ++stats.rawNal;
    countNal(payload[0] & 0x1F);
  }
  countAnnexB(payload, length);
}

}  // namespace

void begin() {
  Serial.println("RTP PROBE READY royal-unwrap callback-only");
}

void handlePacket(const IPAddress &, uint16_t, const uint8_t *data, size_t length) {
  ++stats.packets;
  stats.bytes += length;
  const size_t offset = payloadOffset(data, length);
  if (offset == 0) {
    ++stats.badRtp;
    return;
  }
  if ((data[1] & 0x7F) == 96) ++stats.pt96;
  if (data[1] & 0x80) ++stats.marker;
  const uint16_t sequence = readU16(data);
  const uint32_t timestamp = readU32(data);
  if (stats.haveSequence && sequence != static_cast<uint16_t>(stats.lastSequence + 1)) ++stats.seqGap;
  if (stats.haveTimestamp && timestamp != stats.lastTimestamp) ++stats.timestampChanges;
  stats.lastSequence = sequence;
  stats.lastTimestamp = timestamp;
  stats.haveSequence = true;
  stats.haveTimestamp = true;
  trackRoyalPayload(data + offset, length - offset);
}

void update() {
  if (millis() - stats.lastLogMs < 1000) return;
  stats.lastLogMs = millis();
  Serial.printf("RTP pps=%lu bytes=%lu pt96=%lu mark=%lu ts=%lu gap=%lu royal[start=%lu cont=%lu] "
                "nal[sps=%lu pps=%lu idr=%lu sei=%lu raw=%lu unk=%lu] bad=%lu\n",
                stats.packets, stats.bytes, stats.pt96, stats.marker, stats.timestampChanges, stats.seqGap,
                stats.royalStart, stats.royalContinuation, stats.sps, stats.pps, stats.idr, stats.sei, stats.rawNal,
                stats.unknown, stats.badRtp);
  const bool haveSequence = stats.haveSequence;
  const bool haveTimestamp = stats.haveTimestamp;
  const uint16_t lastSequence = stats.lastSequence;
  const uint32_t lastTimestamp = stats.lastTimestamp;
  stats = {};
  stats.lastLogMs = millis();
  stats.haveSequence = haveSequence;
  stats.haveTimestamp = haveTimestamp;
  stats.lastSequence = lastSequence;
  stats.lastTimestamp = lastTimestamp;
}

}  // namespace navdash_rtp_probe