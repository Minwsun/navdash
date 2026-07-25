#include "navdash_video.h"

#include <esp_heap_caps.h>

#include "navdash_lcd.h"

#ifndef NAVDASH_ENABLE_VIDEO
#define NAVDASH_ENABLE_VIDEO 0
#endif

#if NAVDASH_ENABLE_VIDEO

extern "C" {
#include "h264bsd_macroblock_layer.h"
#include "h264bsd_slice_header.h"
#include "h264bsd_stream.h"
#include "h264bsd_util.h"
}

namespace navdash_video {

constexpr uint16_t kFrameWidth = 240;
constexpr uint16_t kFrameHeight = 137;
constexpr size_t kY4Bytes = kFrameWidth * kFrameHeight / 2;
constexpr size_t kClass2Bytes = kFrameWidth * kFrameHeight / 4;
constexpr uint32_t kLogIntervalMs = 1000;
constexpr size_t kLiveNalBytes = 12 * 1024;
constexpr size_t kRbspScratchBytes = 512;

uint8_t y4Cache[kY4Bytes];
uint8_t class2Cache[kClass2Bytes];
uint8_t liveNal[kLiveNalBytes];
uint8_t rbspScratch[kRbspScratchBytes];
mbStorage_t mbRows[2][33];
macroblockLayer_t mbLayer;
size_t liveNalLength;
bool liveNalOpen;
bool latestIdrReady;
size_t lastSliceDataBit;

struct H264StreamInfo {
  uint8_t profile;
  uint8_t level;
  uint8_t chroma;
  uint8_t log2MaxFrameNum;
  uint8_t log2MaxPocLsb;
  uint8_t entropyCoding;
  uint8_t sliceGroups;
  uint8_t deblockPresent;
  uint16_t mbWidth;
  uint16_t mbHeight;
  uint16_t displayWidth;
  uint16_t displayHeight;
  uint32_t parsedSps;
  uint32_t parsedPps;
  uint32_t parsedIdr;
  uint32_t parseFail;
  bool spsReady;
  bool ppsReady;
};

H264StreamInfo h264;

struct Stats {
  uint32_t packets;
  uint32_t bytes;
  uint32_t rtpPackets;
  uint32_t badRtp;
  uint32_t sequenceDrops;
  uint32_t payloadType96;
  uint32_t accessUnits;
  uint32_t idrStarts;
  uint32_t seiPackets;
  uint32_t spsPackets;
  uint32_t ppsPackets;
  uint32_t continuations;
  uint32_t unsupportedNal;
  uint32_t liveNalComplete;
  uint32_t liveNalOverflow;
  uint32_t latestIdrBytes;
  uint32_t rowWorkspaceBytes;
  uint32_t parsedIdr;
  uint32_t parseFail;
  uint32_t approxFrames;
  uint32_t approxFail;
  uint32_t approxMbs;
  uint32_t lastLogMs;
  uint16_t lastSequence;
  bool haveSequence;
};

Stats stats;

uint32_t readU32(const uint8_t *data) {
  return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

uint16_t readU16(const uint8_t *data) {
  return (static_cast<uint16_t>(data[0]) << 8) | data[1];
}

size_t makeRbsp(const uint8_t *nal, size_t nalLength, uint8_t *out, size_t outCapacity) {
  size_t written = 0;
  uint8_t zeroCount = 0;
  for (size_t i = 1; i < nalLength && written < outCapacity; ++i) {
    const uint8_t value = nal[i];
    if (zeroCount == 2 && value == 0x03) {
      zeroCount = 0;
      continue;
    }
    out[written++] = value;
    zeroCount = value == 0 ? zeroCount + 1 : 0;
  }
  return written;
}

struct BitReader {
  const uint8_t *data;
  size_t bitLength;
  size_t bitIndex;
  bool failed;
};

uint32_t readBits(BitReader &reader, uint8_t count) {
  uint32_t value = 0;
  for (uint8_t i = 0; i < count; ++i) {
    if (reader.bitIndex >= reader.bitLength) {
      reader.failed = true;
      return value;
    }
    value <<= 1;
    value |= (reader.data[reader.bitIndex >> 3] >> (7 - (reader.bitIndex & 7))) & 1;
    ++reader.bitIndex;
  }
  return value;
}

uint32_t readUe(BitReader &reader) {
  uint8_t zeroCount = 0;
  while (!reader.failed && readBits(reader, 1) == 0) {
    ++zeroCount;
    if (zeroCount > 24) {
      reader.failed = true;
      return 0;
    }
  }
  return zeroCount == 0 ? 0 : ((1UL << zeroCount) - 1UL + readBits(reader, zeroCount));
}

int32_t readSe(BitReader &reader) {
  const uint32_t code = readUe(reader);
  return (code & 1) ? static_cast<int32_t>((code + 1) / 2) : -static_cast<int32_t>(code / 2);
}

bool parseSps(const uint8_t *nal, size_t length) {
  const size_t rbspLength = makeRbsp(nal, length, rbspScratch, sizeof(rbspScratch));
  BitReader reader{rbspScratch, rbspLength * 8, 0, false};
  h264.profile = readBits(reader, 8);
  (void)readBits(reader, 8);
  h264.level = readBits(reader, 8);
  (void)readUe(reader);
  h264.chroma = 1;
  h264.log2MaxFrameNum = readUe(reader) + 4;
  const uint32_t pocType = readUe(reader);
  h264.log2MaxPocLsb = 0;
  if (pocType == 0) {
    h264.log2MaxPocLsb = readUe(reader) + 4;
  } else if (pocType == 1) {
    (void)readBits(reader, 1);
    (void)readSe(reader);
    (void)readSe(reader);
    const uint32_t cycle = readUe(reader);
    for (uint32_t i = 0; i < cycle; ++i) (void)readSe(reader);
  }
  (void)readUe(reader);
  (void)readBits(reader, 1);
  h264.mbWidth = readUe(reader) + 1;
  const uint32_t mbHeightMap = readUe(reader) + 1;
  const uint8_t frameMbsOnly = readBits(reader, 1);
  if (!frameMbsOnly) (void)readBits(reader, 1);
  (void)readBits(reader, 1);
  uint32_t cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;
  if (readBits(reader, 1)) {
    cropLeft = readUe(reader);
    cropRight = readUe(reader);
    cropTop = readUe(reader);
    cropBottom = readUe(reader);
  }
  h264.mbHeight = (2 - frameMbsOnly) * mbHeightMap;
  const uint16_t codedWidth = h264.mbWidth * 16;
  const uint16_t codedHeight = h264.mbHeight * 16;
  h264.displayWidth = codedWidth - (cropLeft + cropRight) * 2;
  h264.displayHeight = codedHeight - (cropTop + cropBottom) * 2;
  h264.spsReady = !reader.failed && h264.profile == 66;
  h264.parsedSps += h264.spsReady ? 1 : 0;
  return h264.spsReady;
}

bool parsePps(const uint8_t *nal, size_t length) {
  const size_t rbspLength = makeRbsp(nal, length, rbspScratch, sizeof(rbspScratch));
  BitReader reader{rbspScratch, rbspLength * 8, 0, false};
  (void)readUe(reader);
  (void)readUe(reader);
  h264.entropyCoding = readBits(reader, 1);
  (void)readBits(reader, 1);
  h264.sliceGroups = readUe(reader);
  (void)readUe(reader);
  (void)readUe(reader);
  (void)readBits(reader, 1);
  (void)readBits(reader, 2);
  (void)readSe(reader);
  (void)readSe(reader);
  (void)readSe(reader);
  h264.deblockPresent = readBits(reader, 1);
  h264.ppsReady = !reader.failed && h264.entropyCoding == 0 && h264.sliceGroups == 0;
  h264.parsedPps += h264.ppsReady ? 1 : 0;
  return h264.ppsReady;
}

bool parseIdrSliceHeader(const uint8_t *nal, size_t length) {
  if (!h264.spsReady || !h264.ppsReady) {
    return false;
  }
  const size_t rbspLength = makeRbsp(nal, length, rbspScratch, sizeof(rbspScratch));
  BitReader reader{rbspScratch, rbspLength * 8, 0, false};
  const uint32_t firstMb = readUe(reader);
  const uint32_t sliceType = readUe(reader);
  (void)readUe(reader);
  (void)readBits(reader, h264.log2MaxFrameNum);
  (void)readUe(reader);
  if (h264.log2MaxPocLsb > 0) {
    (void)readBits(reader, h264.log2MaxPocLsb);
  }
  if (h264.deblockPresent) {
    (void)readUe(reader);
  }
  lastSliceDataBit = reader.bitIndex;
  const bool ok = !reader.failed && firstMb == 0 && (sliceType % 5) == 2;
  h264.parsedIdr += ok ? 1 : 0;
  return ok;
}

size_t compactRbspInPlace(uint8_t *nal, size_t nalLength) {
  size_t write = 1;
  uint8_t zeroCount = 0;
  for (size_t read = 1; read < nalLength; ++read) {
    const uint8_t value = nal[read];
    if (zeroCount == 2 && value == 0x03) {
      zeroCount = 0;
      continue;
    }
    nal[write++] = value;
    zeroCount = value == 0 ? zeroCount + 1 : 0;
  }
  return write - 1;
}

uint8_t residualLevelToY(const macroblockLayer_t &layer) {
  int32_t sum = 0;
  uint16_t count = 0;
  for (uint8_t block = 0; block < 16; ++block) {
    sum += abs(layer.residual.level[block][0]);
    ++count;
  }
  if (h264bsdMbPartPredMode(layer.mbType) == PRED_MODE_INTRA16x16) {
    for (uint8_t index = 0; index < 16; ++index) {
      sum += abs(layer.residual.level[24][index]);
      ++count;
    }
  }
  const uint16_t avg = count ? min<int32_t>(255, sum / count) : 0;
  return 32 + min<uint16_t>(200, avg * 5);
}

void storeApproxMb(uint16_t mbAddr, uint8_t yValue, uint8_t cls) {
  const uint16_t mbX = mbAddr % 33;
  const uint16_t mbY = mbAddr / 33;
  const uint16_t outX0 = (mbX * 16UL * kFrameWidth) / 528;
  const uint16_t outX1 = ((mbX + 1) * 16UL * kFrameWidth) / 528;
  const uint16_t outY0 = (mbY * 16UL * kFrameHeight) / 304;
  const uint16_t outY1 = ((mbY + 1) * 16UL * kFrameHeight) / 304;
  const uint8_t y4 = yValue >> 4;
  for (uint16_t y = outY0; y < outY1 && y < kFrameHeight; ++y) {
    for (uint16_t x = outX0; x < outX1 && x < kFrameWidth; ++x) {
      const size_t pixel = static_cast<size_t>(y) * kFrameWidth + x;
      if (pixel & 1) {
        y4Cache[pixel >> 1] = (y4Cache[pixel >> 1] & 0xF0) | y4;
      } else {
        y4Cache[pixel >> 1] = (y4Cache[pixel >> 1] & 0x0F) | (y4 << 4);
      }
      const size_t ci = pixel >> 2;
      const uint8_t shift = (pixel & 3) * 2;
      class2Cache[ci] = (class2Cache[ci] & ~(0x03 << shift)) | ((cls & 0x03) << shift);
    }
  }
}

void renderApproxCacheToLcd() {
  static uint8_t rgb[240 * 2];
  static const uint16_t palette[4][16] = {
      {0x0000,0x0841,0x1082,0x18C3,0x2104,0x2945,0x3186,0x39C7,0x4208,0x4A49,0x528A,0x5ACB,0x630C,0x6B4D,0x738E,0x7BCF},
      {0x0000,0x1082,0x18C3,0x2104,0x2945,0x3186,0x39C7,0x4A49,0x5ACB,0x6B4D,0x7BCF,0x8C51,0x9CD3,0xAD55,0xBDF7,0xCE79},
      {0x001F,0x019F,0x033F,0x04DF,0x067F,0x07FF,0x2FFF,0x57FF,0x7FFF,0xA7FF,0xCFFF,0xE7FF,0xFFFF,0xFFFF,0xFFFF,0xFFFF},
      {0x0000,0x0011,0x0016,0x001B,0x001F,0x015F,0x02BF,0x03FF,0x051F,0x065F,0x07BF,0x07FF,0x57FF,0xAFFF,0xFFFF,0xFFFF},
  };
  for (uint16_t y = 0; y < kFrameHeight; ++y) {
    for (uint16_t x = 0; x < kFrameWidth; ++x) {
      const size_t pixel = static_cast<size_t>(y) * kFrameWidth + x;
      const uint8_t packedY = y4Cache[pixel >> 1];
      const uint8_t y4 = (pixel & 1) ? (packedY & 0x0F) : (packedY >> 4);
      const uint8_t cls = (class2Cache[pixel >> 2] >> ((pixel & 3) * 2)) & 0x03;
      const uint16_t color = palette[cls][y4];
      rgb[x * 2] = color >> 8;
      rgb[x * 2 + 1] = color;
    }
    navdash_lcd::drawRgb565Line(40, 24 + y, rgb, kFrameWidth);
  }
}

uint16_t decodeApproxIdr(uint8_t *nal, size_t nalLength) {
  if (!h264.spsReady || !h264.ppsReady || h264.mbWidth != 33 || h264.mbHeight != 19 || lastSliceDataBit == 0) {
    return 0;
  }
  const size_t rbspLength = compactRbspInPlace(nal, nalLength);
  strmData_t stream{};
  stream.pStrmBuffStart = nal + 1 + (lastSliceDataBit >> 3);
  stream.pStrmCurrPos = stream.pStrmBuffStart;
  stream.bitPosInWord = lastSliceDataBit & 7;
  stream.strmBuffSize = rbspLength - (lastSliceDataBit >> 3);
  stream.strmBuffReadBits = stream.bitPosInWord;
  memset(mbRows, 0, sizeof(mbRows));
  memset(y4Cache, 0, sizeof(y4Cache));
  memset(class2Cache, 0, sizeof(class2Cache));
  uint16_t decoded = 0;
  for (uint16_t mb = 0; mb < 33 * 19; ++mb) {
    mbStorage_t *row = mbRows[(mb / 33) & 1];
    mbStorage_t *prev = mbRows[((mb / 33) + 1) & 1];
    mbStorage_t *cur = &row[mb % 33];
    memset(cur, 0, sizeof(*cur));
    cur->mbA = (mb % 33) ? &row[(mb % 33) - 1] : nullptr;
    cur->mbB = (mb >= 33) ? &prev[mb % 33] : nullptr;
    cur->mbC = (mb >= 33 && (mb % 33) < 32) ? &prev[(mb % 33) + 1] : nullptr;
    cur->mbD = (mb >= 33 && (mb % 33)) ? &prev[(mb % 33) - 1] : nullptr;
    if (h264bsdDecodeMacroblockLayer(&stream, &mbLayer, cur, I_SLICE, 0) != HANTRO_OK) {
      break;
    }
    memcpy(cur->totalCoeff, mbLayer.residual.totalCoeff, sizeof(cur->totalCoeff));
    cur->mbType = mbLayer.mbType;
    cur->decoded = 1;
    const uint8_t yValue = residualLevelToY(mbLayer);
    const uint8_t cls = h264bsdMbPartPredMode(mbLayer.mbType) == PRED_MODE_INTRA16x16 ? 1 : 2;
    storeApproxMb(mb, yValue, cls);
    ++decoded;
    if ((mb % 33) == 32) {
      delay(0);
    }
  }
  if (decoded > 0) {
    renderApproxCacheToLcd();
    return decoded;
  }
  return 0;
}

void parseCompletedNal() {
  const uint8_t nalType = liveNal[0] & 0x1F;
  bool ok = true;
  if (nalType == 7) {
    ok = parseSps(liveNal, liveNalLength);
  } else if (nalType == 8) {
    ok = parsePps(liveNal, liveNalLength);
  } else if (nalType == 5) {
    ok = parseIdrSliceHeader(liveNal, liveNalLength);
    if (ok) {
      ++stats.parsedIdr;
      const uint16_t decodedMbs = decodeApproxIdr(liveNal, liveNalLength);
      stats.approxMbs += decodedMbs;
      if (decodedMbs > 0) {
        ++stats.approxFrames;
      } else {
        ++stats.approxFail;
      }
    }
  }
  if (!ok && (nalType == 5 || nalType == 7 || nalType == 8)) {
    ++h264.parseFail;
    ++stats.parseFail;
  }
}

size_t rtpPayloadOffset(const uint8_t *data, size_t length) {
  if (length < 12 || (data[0] >> 6) != 2) {
    return 0;
  }
  size_t offset = 12 + ((data[0] & 0x0F) * 4);
  if (offset > length) {
    return 0;
  }
  if ((data[0] & 0x10) != 0) {
    if (offset + 4 > length) {
      return 0;
    }
    const uint16_t extensionWords = readU16(data + offset + 2);
    offset += 4 + static_cast<size_t>(extensionWords) * 4;
    if (offset > length) {
      return 0;
    }
  }
  return offset;
}

void trackSequence(uint16_t sequence) {
  if (stats.haveSequence) {
    const uint16_t expected = stats.lastSequence + 1;
    if (sequence != expected) {
      ++stats.sequenceDrops;
    }
  }
  stats.lastSequence = sequence;
  stats.haveSequence = true;
}

void resetLiveNal() {
  liveNalLength = 0;
  liveNalOpen = false;
}

bool appendLiveNal(const uint8_t *data, size_t length) {
  if (liveNalLength + length > sizeof(liveNal)) {
    ++stats.liveNalOverflow;
    resetLiveNal();
    return false;
  }
  memcpy(liveNal + liveNalLength, data, length);
  liveNalLength += length;
  return true;
}

void completeLiveNal() {
  if (!liveNalOpen || liveNalLength == 0) {
    return;
  }
  ++stats.liveNalComplete;
  parseCompletedNal();
  const uint8_t nalType = liveNal[0] & 0x1F;
  if (nalType == 5) {
    latestIdrReady = true;
    stats.latestIdrBytes = liveNalLength;
    stats.rowWorkspaceBytes = sizeof(mbRows) + sizeof(mbLayer);
  }
  resetLiveNal();
}

void startLiveNal(const uint8_t *data, size_t length) {
  resetLiveNal();
  liveNalOpen = true;
  appendLiveNal(data, length);
}

size_t findStartCode(const uint8_t *payload, size_t length) {
  for (size_t i = 0; i + 4 <= length; ++i) {
    if (payload[i] == 0x00 && payload[i + 1] == 0x00 && payload[i + 2] == 0x00 && payload[i + 3] == 0x01) {
      return i;
    }
  }
  return length;
}

void trackRoyalPayload(const uint8_t *payload, size_t length, bool marker) {
  if (length == 0) {
    return;
  }

  const size_t startCode = findStartCode(payload, length);
  if (startCode < length) {
    navdash_lcd::drawH264Bytes(payload, length, true);
    completeLiveNal();
    if (startCode > 0) {
      const uint8_t prefixType = payload[0] & 0x1F;
      if (prefixType >= 1 && prefixType <= 23) {
        startLiveNal(payload, startCode);
        completeLiveNal();
      } else if (prefixType == 28 && startCode >= 2 && (payload[1] & 0x80) != 0) {
        const uint8_t originalType = payload[1] & 0x1F;
        const uint8_t reconstructedHeader = (payload[0] & 0xE0) | originalType;
        startLiveNal(&reconstructedHeader, 1);
        appendLiveNal(payload + 2, startCode - 2);
        completeLiveNal();
      }
    }
    size_t cursor = startCode;
    while (cursor + 4 < length) {
      size_t next = length;
      for (size_t i = cursor + 4; i + 4 <= length; ++i) {
        if (payload[i] == 0x00 && payload[i + 1] == 0x00 && payload[i + 2] == 0x00 && payload[i + 3] == 0x01) {
          next = i;
          break;
        }
      }
      const uint8_t *nal = payload + cursor + 4;
      const size_t nalLength = next - (cursor + 4);
      if (nalLength > 0) {
        const uint8_t nalType = nal[0] & 0x1F;
        if (nalType == 5) {
          ++stats.idrStarts;
          ++stats.accessUnits;
        } else if (nalType == 6) {
          ++stats.seiPackets;
        } else if (nalType == 7) {
          ++stats.spsPackets;
        } else if (nalType == 8) {
          ++stats.ppsPackets;
        }
        startLiveNal(nal, nalLength);
        if (next < length) {
          completeLiveNal();
        }
      }
      cursor = next;
    }
    if (marker) {
      completeLiveNal();
    }
    return;
  }

  if (length >= 2 && payload[0] == 0x3C && payload[1] == 0x87) {
    navdash_lcd::drawH264Bytes(payload, length, true);
    ++stats.spsPackets;
    ++stats.ppsPackets;
    ++stats.idrStarts;
    ++stats.accessUnits;
    const uint8_t spsHeader = 0x27;
    startLiveNal(&spsHeader, 1);
    appendLiveNal(payload + 2, length - 2);
    completeLiveNal();
    return;
  }
  if (length >= 2 && payload[0] == 0x3C && payload[1] == 0x07) {
    ++stats.continuations;
    if (!liveNalOpen) {
      ++stats.unsupportedNal;
      return;
    }
    appendLiveNal(payload + 2, length - 2);
    navdash_lcd::drawH264Bytes(payload + 2, length - 2, false);
    if (marker) {
      completeLiveNal();
    }
    return;
  }
  if (length >= 1) {
    const uint8_t nalType = payload[0] & 0x1F;
    if (nalType == 5) {
      ++stats.idrStarts;
      ++stats.accessUnits;
      startLiveNal(payload, length);
      if (marker) {
        completeLiveNal();
      }
      navdash_lcd::drawH264Bytes(payload, length, true);
    } else if (nalType == 6) {
      ++stats.seiPackets;
      startLiveNal(payload, length);
      completeLiveNal();
    } else if (nalType == 7) {
      ++stats.spsPackets;
      startLiveNal(payload, length);
      completeLiveNal();
    } else if (nalType == 8) {
      ++stats.ppsPackets;
      startLiveNal(payload, length);
      completeLiveNal();
    } else if (nalType != 0) {
      ++stats.unsupportedNal;
    }
  }
  if (marker) {
    ++stats.accessUnits;
  }
}

void begin() {
  memset(y4Cache, 0, sizeof(y4Cache));
  memset(class2Cache, 0, sizeof(class2Cache));
  stats.lastLogMs = millis();
  resetLiveNal();
  Serial.printf("VIDEO READY y4=%u class2=%u cache=%u live_nal=%u mbwork=%u\n", static_cast<unsigned>(sizeof(y4Cache)),
                static_cast<unsigned>(sizeof(class2Cache)),
                static_cast<unsigned>(sizeof(y4Cache) + sizeof(class2Cache)), static_cast<unsigned>(sizeof(liveNal)),
                static_cast<unsigned>(sizeof(mbRows) + sizeof(mbLayer)));
}

void handlePacket(const IPAddress &, uint16_t, const uint8_t *data, size_t length) {
  ++stats.packets;
  stats.bytes += length;

  const size_t payloadOffset = rtpPayloadOffset(data, length);
  if (payloadOffset == 0) {
    ++stats.badRtp;
    return;
  }

  ++stats.rtpPackets;
  const bool marker = (data[1] & 0x80) != 0;
  const uint8_t payloadType = data[1] & 0x7F;
  if (payloadType == 96) {
    ++stats.payloadType96;
  }
  trackSequence(readU16(data + 2));
  (void)readU32(data + 4);

  trackRoyalPayload(data + payloadOffset, length - payloadOffset, marker);
}

void update() {
  const uint32_t now = millis();
  if (now - stats.lastLogMs < kLogIntervalMs) {
    return;
  }

  const uint32_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const uint32_t minInternal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  Serial.printf(
      "VIDEO pps=%lu kbps=%lu rtp=%lu pt96=%lu au=%lu idr=%lu cont=%lu sei=%lu drop_seq=%lu bad=%lu unk=%lu "
      "nal=%lu ovf=%lu idr_bytes=%lu rows=%lu parsed=%lu approx=%lu mbs=%lu afail=%lu pfail=%lu sps=%ux%u disp=%ux%u heap=%lu min=%lu\n",
      stats.packets, (stats.bytes * 8UL) / 1000UL, stats.rtpPackets, stats.payloadType96, stats.accessUnits,
      stats.idrStarts, stats.continuations, stats.seiPackets, stats.sequenceDrops, stats.badRtp, stats.unsupportedNal,
      stats.liveNalComplete, stats.liveNalOverflow, stats.latestIdrBytes, stats.rowWorkspaceBytes, stats.parsedIdr,
      stats.approxFrames, stats.approxMbs, stats.approxFail, stats.parseFail,
      h264.mbWidth * 16, h264.mbHeight * 16, h264.displayWidth, h264.displayHeight, freeInternal, minInternal);
  navdash_lcd::updateVideoStatus(stats.packets, stats.idrStarts, stats.bytes, latestIdrReady);

  stats.packets = 0;
  stats.bytes = 0;
  stats.rtpPackets = 0;
  stats.badRtp = 0;
  stats.sequenceDrops = 0;
  stats.payloadType96 = 0;
  stats.accessUnits = 0;
  stats.idrStarts = 0;
  stats.seiPackets = 0;
  stats.spsPackets = 0;
  stats.ppsPackets = 0;
  stats.continuations = 0;
  stats.unsupportedNal = 0;
  stats.liveNalComplete = 0;
  stats.liveNalOverflow = 0;
  stats.parsedIdr = 0;
  stats.parseFail = 0;
  stats.approxFrames = 0;
  stats.approxFail = 0;
  stats.approxMbs = 0;
  stats.lastLogMs = now;
}

bool hasLiveIdr() {
  return latestIdrReady;
}

size_t latestIdrSize() {
  return stats.latestIdrBytes;
}

}  // namespace navdash_video

#else

namespace navdash_video {

void begin() {}
void handlePacket(const IPAddress &, uint16_t, const uint8_t *, size_t) {}
void update() {}
bool hasLiveIdr() { return false; }
size_t latestIdrSize() { return 0; }

}  // namespace navdash_video

#endif
