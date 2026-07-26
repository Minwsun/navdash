#include "navdash_h264_collector.h"

namespace navdash_h264_collector {

namespace {

constexpr size_t kIdrCapacity = 16 * 1024;
constexpr uint32_t kFNVOffset = 2166136261UL;
constexpr uint32_t kFNVPrime = 16777619UL;

struct Stats {
  uint32_t packets;
  uint32_t bytes;
  uint32_t pt96;
  uint32_t marker;
  uint32_t badRtp;
  uint32_t seqGap;
  uint32_t royalStart;
  uint32_t royalContinuation;
  uint32_t sps;
  uint32_t pps;
  uint32_t idr;
  uint32_t sei;
  uint32_t idrReady;
  uint32_t idrDropGap;
  uint32_t idrDropOverflow;
  uint32_t idrDropRestart;
  uint32_t latestIdrBytes;
  uint32_t latestIdrHash;
  uint16_t lastSequence;
  bool haveSequence;
  uint32_t lastLogMs;
};

Stats stats;
uint8_t idrBuffer[kIdrCapacity];
size_t idrLength;
bool idrOpen;
bool idrCorrupt;
bool idrOverflow;
uint32_t idrHash;

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

size_t findStartCode(const uint8_t *data, size_t length, size_t from) {
  for (size_t i = from; i + 4 < length; ++i) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) return i;
  }
  return length;
}

void countNal(uint8_t type) {
  if (type == 5) ++stats.idr;
  else if (type == 6) ++stats.sei;
  else if (type == 7) ++stats.sps;
  else if (type == 8) ++stats.pps;
}

void resetCurrent() {
  idrLength = 0;
  idrOpen = false;
  idrCorrupt = false;
  idrOverflow = false;
  idrHash = kFNVOffset;
}

bool appendIdr(const uint8_t *data, size_t length) {
  if (!idrOpen || idrLength + length > sizeof(idrBuffer)) {
    idrCorrupt = true;
    idrOverflow = true;
    return false;
  }
  for (size_t i = 0; i < length; ++i) idrHash = (idrHash ^ data[i]) * kFNVPrime;
  memcpy(idrBuffer + idrLength, data, length);
  idrLength += length;
  return true;
}

void startIdr(const uint8_t *data, size_t length) {
  if (idrOpen) ++stats.idrDropRestart;
  resetCurrent();
  idrOpen = true;
  appendIdr(data, length);
}

void finishIdr() {
  if (!idrOpen) return;
  if (idrCorrupt || idrLength == 0) {
    if (idrOverflow) ++stats.idrDropOverflow;
    else ++stats.idrDropGap;
    resetCurrent();
    return;
  }
  ++stats.idrReady;
  stats.latestIdrBytes = idrLength;
  stats.latestIdrHash = idrHash;
  resetCurrent();
}

void openRoyalStart(const uint8_t *payload, size_t length) {
  ++stats.royalStart;
  countNal(7);  // 3C 87 has an implicit SPS NAL header 0x27.
  bool started = false;
  for (size_t code = findStartCode(payload + 2, length - 2, 0); code < length - 2;) {
    const uint8_t *annex = payload + 2;
    const size_t nalStart = code + 4;
    if (nalStart >= length - 2) break;
    const size_t next = findStartCode(annex, length - 2, nalStart);
    const uint8_t type = annex[nalStart] & 0x1F;
    countNal(type);
    if (type == 5) {
      startIdr(annex + nalStart, (length - 2) - nalStart);
      started = true;
      break;
    }
    code = next;
  }
  if (!started) resetCurrent();
}

void trackRoyalPayload(const uint8_t *payload, size_t length, bool marker) {
  (void)marker;
  if (length == 0) return;
  if (length >= 2 && payload[0] == 0x3C && payload[1] == 0x87) {
    // Royal RTP markers may belong to SEI. A new start is the reliable IDR boundary.
    finishIdr();
    openRoyalStart(payload, length);
  } else if (length >= 2 && payload[0] == 0x3C && payload[1] == 0x07) {
    ++stats.royalContinuation;
    if (idrOpen) appendIdr(payload + 2, length - 2);
  } else if (payload[0] == 0x26) {
    countNal(6);
  }
}

}  // namespace

void begin() {
  resetCurrent();
  Serial.printf("H264 COLLECTOR READY idr_capacity=%u callback-only\n", static_cast<unsigned>(kIdrCapacity));
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
  const bool marker = (data[1] & 0x80) != 0;
  if (marker) ++stats.marker;
  const uint16_t sequence = readU16(data);
  if (stats.haveSequence && sequence != static_cast<uint16_t>(stats.lastSequence + 1)) {
    ++stats.seqGap;
    if (idrOpen) idrCorrupt = true;
  }
  stats.lastSequence = sequence;
  stats.haveSequence = true;
  trackRoyalPayload(data + offset, length - offset, marker);
}

void update() {
  if (millis() - stats.lastLogMs < 1000) return;
  stats.lastLogMs = millis();
  Serial.printf("H264 pps=%lu bytes=%lu pt96=%lu mark=%lu gap=%lu royal=%lu/%lu nal=%lu/%lu/%lu/%lu "
                "ready=%lu drop_gap=%lu drop_ovf=%lu restart=%lu latest=%lu hash=%08lX bad=%lu\n",
                stats.packets, stats.bytes, stats.pt96, stats.marker, stats.seqGap, stats.royalStart,
                stats.royalContinuation, stats.sps, stats.pps, stats.idr, stats.sei, stats.idrReady,
                stats.idrDropGap, stats.idrDropOverflow, stats.idrDropRestart, stats.latestIdrBytes,
                stats.latestIdrHash, stats.badRtp);
  const bool haveSequence = stats.haveSequence;
  const uint16_t lastSequence = stats.lastSequence;
  const uint32_t latestIdrBytes = stats.latestIdrBytes;
  const uint32_t latestIdrHash = stats.latestIdrHash;
  stats = {};
  stats.lastLogMs = millis();
  stats.haveSequence = haveSequence;
  stats.lastSequence = lastSequence;
  stats.latestIdrBytes = latestIdrBytes;
  stats.latestIdrHash = latestIdrHash;
}

}  // namespace navdash_h264_collector