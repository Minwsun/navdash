#include "navdash_video.h"

#include <esp_heap_caps.h>

#include "navdash_lcd.h"

#ifndef NAVDASH_ENABLE_VIDEO
#define NAVDASH_ENABLE_VIDEO 0
#endif

#ifndef NAVDASH_VIDEO_DEBUG_BYTES
#define NAVDASH_VIDEO_DEBUG_BYTES 0
#endif

#if NAVDASH_ENABLE_VIDEO

extern "C" {
#include "h264bsd_macroblock_layer.h"
#include "h264bsd_nal_unit.h"
#include "h264bsd_pic_param_set.h"
#include "h264bsd_seq_param_set.h"
#include "h264bsd_slice_header.h"
#include "h264bsd_stream.h"
#include "h264bsd_util.h"
}

namespace navdash_video {

constexpr uint16_t kFrameWidth = 240;
constexpr uint16_t kFrameHeight = 240;
constexpr size_t kY4Bytes = kFrameWidth * kFrameHeight / 2;
constexpr size_t kClass2Bytes = kFrameWidth * kFrameHeight / 4;
constexpr uint32_t kLogIntervalMs = 1000;
constexpr uint32_t kDecodeIntervalMs = 1000;
constexpr uint32_t kMinDecodeHeapBytes = 90000;
constexpr uint32_t kEmergencyHeapBytes = 80000;
constexpr size_t kPacketSlotCount = 6;
constexpr size_t kPacketBytes = 1472;
constexpr uint32_t kVideoTaskStack = 4608;
constexpr size_t kLiveNalBytes = 24 * 1024;
constexpr size_t kRbspScratchBytes = 512;
constexpr uint16_t kPresentLines = 8;
constexpr size_t kPresentBytes = kFrameWidth * kPresentLines * 2;
constexpr uint16_t kSourceMbWidth = 33;
constexpr uint16_t kSourceMbHeight = 19;
constexpr uint16_t kSourceWidth = 528;
constexpr uint16_t kSourceHeight = 304;
constexpr uint16_t kCropX = 113;
constexpr uint16_t kCropY = 0;
constexpr uint16_t kCropSize = 300;
constexpr size_t kRollingLumaBytes = kSourceMbWidth * 2 * 16 * 16;
constexpr size_t kRollingChromaBytes = kSourceMbWidth * 2 * 8 * 8;
constexpr size_t kRollingImageBytes = kRollingLumaBytes + kRollingChromaBytes * 2;
constexpr size_t kMbRowsBytes = sizeof(mbStorage_t) * 2 * kSourceMbWidth;
constexpr size_t kMbLayerBytes = sizeof(macroblockLayer_t);
constexpr size_t kMbPixelWords = 384 / 4;
constexpr size_t kMbPixelBytes = kMbPixelWords * sizeof(uint32_t);

uint8_t *y4Cache;
uint8_t *class2Cache;
uint8_t *liveNal;
uint8_t *rbspScratch;
mbStorage_t (*mbRows)[kSourceMbWidth];
macroblockLayer_t *mbLayer;
uint8_t *rollingImageData;
uint32_t *mbPixelWords;
uint8_t *presentBuffer;
image_t rollingImage;
size_t liveNalLength;
bool liveNalOpen;
bool liveNalCorrupt;
bool latestIdrReady;
size_t lastSliceDataBit;
QueueHandle_t freeSlots;
QueueHandle_t readySlots;
TaskHandle_t videoTaskHandle;
StaticQueue_t freeSlotsStorage;
StaticQueue_t readySlotsStorage;
uint8_t freeSlotQueueStorage[kPacketSlotCount];
uint8_t readySlotQueueStorage[kPacketSlotCount];
bool stopRequested;
bool videoStopped = true;

enum class FrameState : uint8_t {
  Empty,
  Decoding,
  Ready,
  Presenting,
};

FrameState frameState = FrameState::Empty;
uint16_t frameNextMb;
uint32_t frameSliceId;

struct RtpSlot {
  uint16_t length;
  uint8_t data[kPacketBytes];
};

RtpSlot *rtpSlots;

struct H264StreamInfo {
  uint8_t profile;
  uint8_t level;
  uint8_t chroma;
  uint8_t log2MaxFrameNum;
  uint8_t log2MaxPocLsb;
  uint8_t entropyCoding;
  uint8_t sliceGroups;
  uint8_t deblockPresent;
  uint8_t pocType;
  uint8_t bottomPocPresent;
  uint8_t redundantPicCntPresent;
  uint8_t constrainedIntraPred;
  int8_t picInitQp;
  int8_t sliceQpDelta;
  uint16_t firstMbInSlice;
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
seqParamSet_t nativeSps{};
picParamSet_t nativePps{};
bool nativeHeadersReady;

struct NativeSlice { strmData_t stream; size_t rbspLength; };

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
  uint32_t grayFrames;
  uint32_t grayMbs;
  uint32_t liteFrames;
  uint32_t liteMbs;
  uint32_t presentedFrames;
  uint32_t decodeSkipped;
  uint32_t queueDrops;
  uint32_t lastFailMb;
  uint32_t lastFailBits;
  uint32_t lastFailCode;
  uint32_t lastNalBytes;
  uint32_t lastRbspBytes;
  uint32_t lastSliceBits;
  uint32_t lastBitsLeft;
  uint32_t lastNalTail;
  uint32_t lastLogMs;
  uint16_t lastSequence;
  bool haveSequence;
  uint32_t lastDecodeMs;
};

Stats stats;
uint32_t probeLastPacketMs;
uint32_t probeLastLogMs;
uint32_t probePackets;
uint32_t probeBytes;

void processPacket(const uint8_t *data, size_t length);

void videoTask(void *) {
  uint8_t slotIndex;
  while (!stopRequested) {
    if (readySlots && xQueueReceive(readySlots, &slotIndex, pdMS_TO_TICKS(20)) == pdTRUE) {
      processPacket(rtpSlots[slotIndex].data, rtpSlots[slotIndex].length);
      xQueueSend(freeSlots, &slotIndex, 0);
    }
    update();
  }
  videoStopped = true;
  videoTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

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

size_t compactRbspInPlace(uint8_t *nal, size_t nalLength);
bool decodeNativeParameterSet(const uint8_t *nal, size_t length, bool sequence) {
  if (length < 2 || length > kRbspScratchBytes) return false;
  rbspScratch[0] = nal[0];
  const size_t payloadLength = makeRbsp(nal, length, rbspScratch + 1, kRbspScratchBytes - 1);
  strmData_t stream{};
  stream.pStrmBuffStart = rbspScratch;
  stream.pStrmCurrPos = rbspScratch;
  stream.strmBuffSize = payloadLength + 1;
  nalUnit_t unit{};
  if (h264bsdDecodeNalUnit(&stream, &unit) != HANTRO_OK) return false;
  if (sequence) {
    return unit.nalUnitType == NAL_SEQ_PARAM_SET && h264bsdDecodeSeqParamSet(&stream, &nativeSps) == HANTRO_OK;
  }
  return unit.nalUnitType == NAL_PIC_PARAM_SET && h264bsdDecodePicParamSet(&stream, &nativePps) == HANTRO_OK;
}

bool applyNativeSliceHeader(uint8_t *nal, size_t nalLength) {
  if (!nativeHeadersReady) return false;
  const size_t rbspLength = compactRbspInPlace(nal, nalLength);
  strmData_t stream{};
  stream.pStrmBuffStart = nal;
  stream.pStrmCurrPos = nal;
  stream.strmBuffSize = rbspLength + 1;
  nalUnit_t unit{};
  sliceHeader_t header{};
  if (h264bsdDecodeNalUnit(&stream, &unit) != HANTRO_OK ||
      h264bsdDecodeSliceHeader(&stream, &header, &nativeSps, &nativePps, &unit) != HANTRO_OK ||
      unit.nalUnitType != NAL_CODED_SLICE_IDR || !IS_I_SLICE(header.sliceType)) {
    return false;
  }
  h264.firstMbInSlice = header.firstMbInSlice;
  h264.sliceQpDelta = header.sliceQpDelta;
  lastSliceDataBit = stream.strmBuffReadBits - 8;
  return true;
}

bool parseSps(const uint8_t *nal, size_t length) {
  const size_t rbspLength = makeRbsp(nal, length, rbspScratch, kRbspScratchBytes);
  BitReader reader{rbspScratch, rbspLength * 8, 0, false};
  h264.profile = readBits(reader, 8);
  (void)readBits(reader, 8);
  h264.level = readBits(reader, 8);
  (void)readUe(reader);
  h264.chroma = 1;
  h264.log2MaxFrameNum = readUe(reader) + 4;
  const uint32_t pocType = readUe(reader);
  h264.pocType = pocType;
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
  const size_t rbspLength = makeRbsp(nal, length, rbspScratch, kRbspScratchBytes);
  BitReader reader{rbspScratch, rbspLength * 8, 0, false};
  (void)readUe(reader);
  (void)readUe(reader);
  h264.entropyCoding = readBits(reader, 1);
  h264.bottomPocPresent = readBits(reader, 1);
  h264.sliceGroups = readUe(reader);
  (void)readUe(reader);
  (void)readUe(reader);
  (void)readBits(reader, 1);
  (void)readBits(reader, 2);
  h264.picInitQp = 26 + readSe(reader);
  (void)readSe(reader);
  (void)readSe(reader);
  h264.deblockPresent = readBits(reader, 1);
  h264.constrainedIntraPred = readBits(reader, 1);
  h264.redundantPicCntPresent = readBits(reader, 1);
  h264.ppsReady = !reader.failed && h264.entropyCoding == 0 && h264.sliceGroups == 0;
  h264.parsedPps += h264.ppsReady ? 1 : 0;
  return h264.ppsReady;
}

bool parseIdrSliceHeader(uint8_t *nal, size_t length) {
  if (!h264.spsReady || !h264.ppsReady) {
    return false;
  }
  const size_t rbspLength = makeRbsp(nal, length, rbspScratch, kRbspScratchBytes);
  BitReader reader{rbspScratch, rbspLength * 8, 0, false};
  const uint32_t firstMb = readUe(reader);
  const uint32_t sliceType = readUe(reader);
  (void)readUe(reader);
  (void)readBits(reader, h264.log2MaxFrameNum);
  (void)readUe(reader);
  if (h264.pocType == 0 && h264.log2MaxPocLsb > 0) {
    (void)readBits(reader, h264.log2MaxPocLsb);
    if (h264.bottomPocPresent) {
      (void)readSe(reader);
    }
  }
  if (h264.redundantPicCntPresent) {
    (void)readUe(reader);
  }
  (void)readBits(reader, 2);
  h264.sliceQpDelta = readSe(reader);
  if (h264.deblockPresent) {
    const uint32_t disableDeblock = readUe(reader);
    if (disableDeblock != 1) {
      (void)readSe(reader);
      (void)readSe(reader);
    }
  }
  lastSliceDataBit = reader.bitIndex;
  h264.firstMbInSlice = firstMb;
  bool ok = !reader.failed && firstMb < kSourceMbWidth * kSourceMbHeight && (sliceType % 5) == 2;
  if (ok && nativeHeadersReady) {
    ok = applyNativeSliceHeader(nal, length);
  }
  if (!ok) return false;
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
  uint8_t base = 42;
  if (h264bsdMbPartPredMode(layer.mbType) == PRED_MODE_INTRA16x16) {
    base = 72;
  } else if (h264bsdMbPartPredMode(layer.mbType) == PRED_MODE_INTRA4x4) {
    base = 96;
  }
  return base + min<uint16_t>(150, avg * 4);
}

uint8_t macroblockClass(const macroblockLayer_t &layer) {
  const int32_t blueEnergy = abs(layer.residual.level[16][0]) + abs(layer.residual.level[17][0]) +
                             abs(layer.residual.level[20][0]) + abs(layer.residual.level[21][0]);
  if (blueEnergy > 16) {
    return 3;
  }
  return h264bsdMbPartPredMode(layer.mbType) == PRED_MODE_INTRA4x4 ? 2 : 1;
}

void storeApproxMb(uint16_t mbAddr, uint8_t yValue, uint8_t) {
  const uint16_t mbX = mbAddr % kSourceMbWidth;
  const uint16_t mbY = mbAddr / kSourceMbWidth;
  const uint16_t outX0 = (mbX * 16UL * kFrameWidth) / kSourceWidth;
  const uint16_t outX1 = ((mbX + 1) * 16UL * kFrameWidth) / kSourceWidth;
  const uint16_t outY0 = (mbY * 16UL * kFrameHeight) / kSourceHeight;
  const uint16_t outY1 = ((mbY + 1) * 16UL * kFrameHeight) / kSourceHeight;
  const uint8_t y4 = yValue >> 4;
  for (uint16_t y = outY0; y < outY1 && y < kFrameHeight; ++y) {
    for (uint16_t x = outX0; x < outX1 && x < kFrameWidth; ++x) {
      const size_t pixel = static_cast<size_t>(y) * kFrameWidth + x;
      if (pixel & 1) y4Cache[pixel >> 1] = (y4Cache[pixel >> 1] & 0xF0) | y4;
      else y4Cache[pixel >> 1] = (y4Cache[pixel >> 1] & 0x0F) | (y4 << 4);
    }
  }
}

uint8_t classifyYuv(uint8_t y, uint8_t u, uint8_t v) {
  if (y >= 58 && y <= 130 && u >= 140 && u <= 150 && v >= 117 && v <= 123 && u - v >= 18) return 3;
  if (y >= 115 && y <= 220 && u >= 130 && u <= 140 && v >= 121 && v <= 127) return 2;
  if (y >= 48 && y <= 150 && u >= 132 && u <= 141 && v >= 121 && v <= 128) return 1;
  return 0;
}

void setPackedPixel(size_t pixel, uint8_t yValue, uint8_t uValue, uint8_t vValue) {
  const uint8_t y4 = yValue >> 4;
  if (pixel & 1) {
    y4Cache[pixel >> 1] = (y4Cache[pixel >> 1] & 0xF0) | y4;
  } else {
    y4Cache[pixel >> 1] = (y4Cache[pixel >> 1] & 0x0F) | (y4 << 4);
  }
  const uint8_t cls = classifyYuv(yValue, uValue, vValue);
  if (!class2Cache) return;
  const size_t ci = pixel >> 2;
  const uint8_t shift = (pixel & 3) * 2;
  class2Cache[ci] = (class2Cache[ci] & ~(0x03 << shift)) | ((cls & 0x03) << shift);
}

void prepareRollingRow(uint16_t mbY) {
  if (!rollingImageData) {
    return;
  }
  if (mbY == 0) {
    memset(rollingImageData, 128, kRollingImageBytes);
    memset(mbRows, 0, kMbRowsBytes);
    return;
  }
  if (mbY > 1) {
    uint8_t *luma = rollingImageData;
    uint8_t *cb = rollingImageData + kRollingLumaBytes;
    uint8_t *cr = cb + kRollingChromaBytes;
    const size_t lumaRow = kSourceMbWidth * 16 * 16;
    const size_t chromaRow = kSourceMbWidth * 8 * 8;
    memmove(luma, luma + lumaRow, lumaRow);
    memmove(cb, cb + chromaRow, chromaRow);
    memmove(cr, cr + chromaRow, chromaRow);
    memset(luma + lumaRow, 128, lumaRow);
    memset(cb + chromaRow, 128, chromaRow);
    memset(cr + chromaRow, 128, chromaRow);
    memcpy(mbRows[0], mbRows[1], sizeof(mbRows[0]));
    memset(mbRows[1], 0, sizeof(mbRows[1]));
  }
}

void storeRealLumaMb(uint16_t mbAddr, uint16_t relMb) {
  const uint16_t mbX = mbAddr % kSourceMbWidth;
  const uint16_t mbY = mbAddr / kSourceMbWidth;
  const uint16_t relRow = relMb / kSourceMbWidth;
  const int32_t sourceX0 = mbX * 16;
  const int32_t sourceX1 = sourceX0 + 16;
  const int32_t sourceY0 = mbY * 16;
  const int32_t sourceY1 = sourceY0 + 16;
  const int32_t outX0 = max<int32_t>(0, ((sourceX0 - kCropX) * kFrameWidth + kCropSize - 1) / kCropSize);
  const int32_t outX1 = min<int32_t>(kFrameWidth, ((sourceX1 - kCropX) * kFrameWidth + kCropSize - 1) / kCropSize);
  const int32_t outY0 = max<int32_t>(0, ((sourceY0 - kCropY) * kFrameHeight + kCropSize - 1) / kCropSize);
  const int32_t outY1 = min<int32_t>(kFrameHeight, ((sourceY1 - kCropY) * kFrameHeight + kCropSize - 1) / kCropSize);
  if (outX0 >= outX1 || outY0 >= outY1) return;

  const uint8_t *yBase = rollingImageData + relRow * kSourceMbWidth * 16 * 16;
  const uint8_t *uBase = rollingImageData + kRollingLumaBytes + relRow * kSourceMbWidth * 8 * 8;
  const uint8_t *vBase = rollingImageData + kRollingLumaBytes + kRollingChromaBytes + relRow * kSourceMbWidth * 8 * 8;
  for (int32_t y = outY0; y < outY1; ++y) {
    const uint16_t sourceY = kCropY + (static_cast<uint32_t>(y) * kCropSize) / kFrameHeight;
    const uint16_t localY = sourceY - mbY * 16;
    for (int32_t x = outX0; x < outX1; ++x) {
      const uint16_t sourceX = kCropX + (static_cast<uint32_t>(x) * kCropSize) / kFrameWidth;
      const uint16_t localX = sourceX - mbX * 16;
      const uint8_t yValue = yBase[localY * kSourceWidth + mbX * 16 + localX];
      const uint8_t uValue = uBase[(localY >> 1) * (kSourceWidth / 2) + mbX * 8 + (localX >> 1)];
      const uint8_t vValue = vBase[(localY >> 1) * (kSourceWidth / 2) + mbX * 8 + (localX >> 1)];
      setPackedPixel(static_cast<size_t>(y) * kFrameWidth + x, yValue, uValue, vValue);
    }
  }
}
bool shouldDecodeNow() {
  if (!mbRows || !mbLayer || stopRequested || frameState != FrameState::Empty) {
    return false;
  }
  const uint32_t now = millis();
  if (now - stats.lastDecodeMs < kDecodeIntervalMs ||
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < kMinDecodeHeapBytes) {
    ++stats.decodeSkipped;
    return false;
  }
  stats.lastDecodeMs = now;
  return true;
}

void renderApproxCacheToLcd() {
  if (!presentBuffer) {
    return;
  }
  for (uint16_t y0 = 0; y0 < kFrameHeight; y0 += kPresentLines) {
    const uint16_t lines = min<uint16_t>(kPresentLines, kFrameHeight - y0);
    for (uint16_t line = 0; line < lines; ++line) {
      const uint16_t y = y0 + line;
      for (uint16_t x = 0; x < kFrameWidth; ++x) {
        const size_t pixel = static_cast<size_t>(y) * kFrameWidth + x;
        const int16_t dx = static_cast<int16_t>(x) - kFrameWidth / 2;
        const int16_t dy = static_cast<int16_t>(y) - kFrameHeight / 2;
        uint16_t color = 0;
        if (dx * dx + dy * dy <= (kFrameWidth / 2) * (kFrameWidth / 2)) {
          const uint8_t packedY = y4Cache[pixel >> 1];
          const uint8_t y4 = (pixel & 1) ? (packedY & 0x0F) : (packedY >> 4);
          const uint8_t gray = y4 * 17;
          color = (static_cast<uint16_t>(gray >> 3) << 11) |
                  (static_cast<uint16_t>(gray >> 2) << 5) | (gray >> 3);
        }
        const size_t output = (static_cast<size_t>(line) * kFrameWidth + x) * 2;
        presentBuffer[output] = color >> 8;
        presentBuffer[output + 1] = color;
      }
    }
    navdash_lcd::drawRgb565Block(40, y0, presentBuffer, kFrameWidth, lines);
    if (stopRequested) {
      return;
    }
  }
}

bool hasMoreRbspData(const strmData_t &stream) {
  const uint32_t totalBits = stream.strmBuffSize * 8;
  if (stream.strmBuffReadBits >= totalBits) {
    return false;
  }
  const uint32_t remaining = totalBits - stream.strmBuffReadBits;
  if (remaining > 8) {
    return true;
  }
  for (uint32_t bit = 0; bit < remaining; ++bit) {
    const uint32_t absolute = stream.strmBuffReadBits + bit;
    const uint8_t value = (stream.pStrmBuffStart[absolute >> 3] >> (7 - (absolute & 7))) & 1;
    if (value != (bit == 0 ? 1 : 0)) {
      return true;
    }
  }
  return false;
}

void beginApproxFrame() {
  prepareRollingRow(0);
  rollingImage = {rollingImageData, kSourceMbWidth, 2, nullptr, nullptr, nullptr};
  memset(mbPixelWords, 0, kMbPixelBytes);
  memset(y4Cache, 0, kY4Bytes);
  frameNextMb = 0;
  frameSliceId = 0;
  frameState = FrameState::Decoding;
}

bool decodeApproxSlice(uint8_t *nal, size_t nalLength) {
  if (!mbRows || !mbLayer || h264.firstMbInSlice != frameNextMb || lastSliceDataBit == 0) {
    return false;
  }
  const size_t rbspLength = compactRbspInPlace(nal, nalLength);
  stats.lastNalBytes = nalLength;
  stats.lastRbspBytes = rbspLength;
  stats.lastSliceBits = lastSliceDataBit;
  stats.lastNalTail = 0;
  for (size_t index = nalLength > 4 ? nalLength - 4 : 0; index < nalLength; ++index) {
    stats.lastNalTail = (stats.lastNalTail << 8) | nal[index];
  }
  strmData_t stream{};
  stream.pStrmBuffStart = nal + 1;
  stream.pStrmCurrPos = stream.pStrmBuffStart;
  stream.strmBuffSize = rbspLength;
  if (h264bsdFlushBits(&stream, lastSliceDataBit) != HANTRO_OK) {
    stats.lastFailMb = frameNextMb;
    stats.lastFailBits = lastSliceDataBit;
    stats.lastFailCode = END_OF_STREAM;
    return false;
  }

  const uint32_t sliceId = ++frameSliceId;
  const uint16_t totalMbs = kSourceMbWidth * kSourceMbHeight;
  int32_t qpY = static_cast<int32_t>(nativePps.picInitQp) + h264.sliceQpDelta;
  while (frameNextMb < totalMbs && hasMoreRbspData(stream)) {
    const uint16_t mb = frameNextMb;
    const uint16_t mbX = mb % kSourceMbWidth;
    const uint16_t mbY = mb / kSourceMbWidth;
    if (mbX == 0) {
      prepareRollingRow(mbY);
    }
    mbStorage_t *row = mbY == 0 ? mbRows[0] : mbRows[1];
    mbStorage_t *prev = mbRows[0];
    mbStorage_t *cur = &row[mbX];
    const uint16_t relMb = mbY == 0 ? mbX : kSourceMbWidth + mbX;
    memset(cur, 0, sizeof(*cur));
    cur->sliceId = sliceId;
    cur->chromaQpIndexOffset = nativePps.chromaQpIndexOffset;
    cur->mbA = mbX ? &row[mbX - 1] : nullptr;
    cur->mbB = mbY ? &prev[mbX] : nullptr;
    cur->mbC = (mbY && mbX < kSourceMbWidth - 1) ? &prev[mbX + 1] : nullptr;
    cur->mbD = (mbY && mbX) ? &prev[mbX - 1] : nullptr;
    const uint32_t result = h264bsdDecodeMacroblockLayer(&stream, mbLayer, cur, I_SLICE, 0);
    if (result != HANTRO_OK) {
      stats.lastFailMb = mb;
      stats.lastFailBits = stream.strmBuffReadBits;
      stats.lastFailCode = result;
      stats.lastBitsLeft = stream.strmBuffReadBits <= rbspLength * 8 ?
                               static_cast<uint32_t>(rbspLength * 8 - stream.strmBuffReadBits) : 0;
      return false;
    }
    const uint32_t reconstructed = h264bsdDecodeMacroblock(
        cur, mbLayer, &rollingImage, nullptr, &qpY, relMb, nativePps.constrainedIntraPredFlag,
        reinterpret_cast<uint8_t *>(mbPixelWords));
    if (reconstructed != HANTRO_OK) {
      stats.lastFailMb = mb;
      stats.lastFailBits = stream.strmBuffReadBits;
      stats.lastFailCode = reconstructed;
      return false;
    }
    storeRealLumaMb(mb, relMb);
    ++frameNextMb;
    if (stopRequested) {
      return false;
    }
  }
  stats.lastBitsLeft = stream.strmBuffReadBits <= rbspLength * 8 ?
                           static_cast<uint32_t>(rbspLength * 8 - stream.strmBuffReadBits) : 0;
  return true;
}

size_t findAnnexBStart(const uint8_t *data, size_t length, size_t from) {
  for (size_t index = from; index + 4 <= length; ++index) {
    if (data[index] == 0 && data[index + 1] == 0 && data[index + 2] == 0 && data[index + 3] == 1) {
      return index;
    }
  }
  return length;
}

void parseCompletedNal() {
  if (!liveNalLength || !h264.spsReady || !h264.ppsReady) {
    return;
  }
  bool decodeThisAu = false;
  bool frameOpen = false;
  bool failed = false;
  size_t start = 0;
  while (start < liveNalLength) {
    const size_t end = findAnnexBStart(liveNal, liveNalLength, start + 1);
    const size_t nalLength = end - start;
    if (nalLength == 0) {
      failed = true;
      break;
    }
    uint8_t *nal = liveNal + start;
    const uint8_t nalType = nal[0] & 0x1F;
    if (nalType == 5) {
      if (!parseIdrSliceHeader(nal, nalLength)) {
        failed = true;
        break;
      }
      ++stats.parsedIdr;
      Serial.printf("H264 SLICE first=%u expected=%u len=%u hdr=%u\\n", h264.firstMbInSlice, frameNextMb,
                    static_cast<unsigned>(nalLength), static_cast<unsigned>(lastSliceDataBit));
      if (h264.firstMbInSlice == 0) {
        if (frameOpen) {
          failed = true;
          break;
        }
        frameOpen = true;
        decodeThisAu = shouldDecodeNow();
        if (decodeThisAu) {
          beginApproxFrame();
        }
      }
      if (!frameOpen || (decodeThisAu && !decodeApproxSlice(nal, nalLength))) {
        failed = true;
        break;
      }
    } else if (nalType == 7) {
      if (parseSps(nal, nalLength)) ++stats.spsPackets;
    } else if (nalType == 8) {
      if (parsePps(nal, nalLength)) ++stats.ppsPackets;
    } else if (nalType == 6) {
      ++stats.seiPackets;
    } else {
      ++stats.unsupportedNal;
    }
    if (end == liveNalLength) {
      break;
    }
    start = end + 4;
  }

  if (frameOpen && decodeThisAu && !failed && frameNextMb == kSourceMbWidth * kSourceMbHeight) {
    ++stats.approxFrames;
    ++stats.liteFrames;
    stats.approxMbs += frameNextMb;
    stats.liteMbs += frameNextMb;
    frameState = FrameState::Ready;
    return;
  }
  // ponytail: route-lite presents a mostly decoded IDR; replace with full YUV reconstruction when CAVLC tail is fixed.
  if (frameOpen && decodeThisAu && failed && frameNextMb >= (kSourceMbWidth * kSourceMbHeight * 3) / 4) {
    ++stats.approxFrames;
    ++stats.liteFrames;
    stats.approxMbs += frameNextMb;
    stats.liteMbs += frameNextMb;
    frameState = FrameState::Ready;
    return;
  }
  if (frameOpen && !decodeThisAu) {
    latestIdrReady = true;
    return;
  }
  if (frameOpen) {
    ++stats.approxFail;
    ++stats.parseFail;
    h264.parseFail++;
    stats.lastFailMb = frameNextMb;
  }
  frameState = FrameState::Empty;
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
      if (liveNalOpen) liveNalCorrupt = true;
    }
  }
  stats.lastSequence = sequence;
  stats.haveSequence = true;
}

void resetLiveNal() {
  liveNalLength = 0;
  liveNalOpen = false;
  liveNalCorrupt = false;
}

bool appendLiveNal(const uint8_t *data, size_t length) {
  if (!liveNal || liveNalLength + length > kLiveNalBytes) {
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
  if (liveNalCorrupt) {
    ++stats.sequenceDrops;
    resetLiveNal();
    return;
  }
  ++stats.liveNalComplete;
  parseCompletedNal();
  const uint8_t nalType = liveNal[0] & 0x1F;
  if (nalType == 5) {
    latestIdrReady = true;
    stats.latestIdrBytes = liveNalLength;
    stats.rowWorkspaceBytes = kMbRowsBytes + kMbLayerBytes + kMbPixelBytes;
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

void parseRoyalStart(const uint8_t *payload, size_t length) {
  const uint8_t *body = payload + 2;
  const size_t bodyLength = length - 2;
  const size_t firstCode = findStartCode(body, bodyLength);
  if (firstCode == bodyLength || firstCode + 4 >= bodyLength || firstCode + 1 > 31) {
    ++stats.unsupportedNal;
    return;
  }

  uint8_t sps[32] = {0x27};
  memcpy(sps + 1, body, firstCode);
  static bool loggedSps;
  if (!loggedSps) {
    Serial.printf("VIDEO SPS len=%u bytes=", static_cast<unsigned>(firstCode + 1));
    for (size_t i = 0; i <= firstCode; ++i) Serial.printf("%02X", sps[i]);
    Serial.println();
    loggedSps = true;
  }
  const bool parsedSps = parseSps(sps, firstCode + 1);
  nativeHeadersReady = parsedSps && decodeNativeParameterSet(sps, firstCode + 1, true);
  if (parsedSps) ++stats.spsPackets;

  size_t code = firstCode;
  while (code + 4 < bodyLength) {
    size_t next = bodyLength;
    for (size_t i = code + 4; i + 4 <= bodyLength; ++i) {
      if (body[i] == 0 && body[i + 1] == 0 && body[i + 2] == 0 && body[i + 3] == 1) {
        next = i;
        break;
      }
    }
    const uint8_t *nal = body + code + 4;
    const size_t nalLength = next - (code + 4);
    if (nalLength == 0) break;
    const uint8_t nalType = nal[0] & 0x1F;
    if (nalType == 8) {
      const bool parsedPps = parsePps(nal, nalLength);
      nativeHeadersReady = nativeHeadersReady && parsedPps && decodeNativeParameterSet(nal, nalLength, false) &&
                           nativePps.seqParameterSetId == nativeSps.seqParameterSetId && nativePps.numSliceGroups == 1;
      if (parsedPps) ++stats.ppsPackets;
    } else if (nalType == 5) {
      ++stats.idrStarts;
      ++stats.accessUnits;
      startLiveNal(nal, bodyLength - (code + 4));
      return;
    } else if (nalType == 6) {
      ++stats.seiPackets;
    }
    code = next;
  }
  ++stats.unsupportedNal;
}

void trackRoyalPayload(const uint8_t *payload, size_t length, bool marker) {
  (void)marker;
  if (length < 1) return;
  if (length >= 2 && payload[0] == 0x3C && payload[1] == 0x87) {
    completeLiveNal();
    parseRoyalStart(payload, length);
    return;
  }
  if (length >= 2 && payload[0] == 0x3C && payload[1] == 0x07) {
    ++stats.continuations;
    if (!liveNalOpen) {
      ++stats.unsupportedNal;
      return;
    }
    appendLiveNal(payload + 2, length - 2);
    return;
  }
  if ((payload[0] & 0x1F) == 6) {
    ++stats.seiPackets;
    return;
  }
  ++stats.unsupportedNal;
}
void freeVideoMemory() {
  if (freeSlots) vQueueDelete(freeSlots);
  if (readySlots) vQueueDelete(readySlots);
  heap_caps_free(y4Cache);
  heap_caps_free(class2Cache);
  heap_caps_free(liveNal);
  heap_caps_free(rbspScratch);
  heap_caps_free(mbRows);
  heap_caps_free(mbLayer);
  heap_caps_free(mbPixelWords);
  heap_caps_free(rollingImageData);
  heap_caps_free(presentBuffer);
  heap_caps_free(rtpSlots);
  y4Cache = nullptr;
  class2Cache = nullptr;
  liveNal = nullptr;
  rbspScratch = nullptr;
  mbRows = nullptr;
  mbLayer = nullptr;
  mbPixelWords = nullptr;
  rollingImageData = nullptr;
  presentBuffer = nullptr;
  rtpSlots = nullptr;
  freeSlots = nullptr;
  readySlots = nullptr;
  frameState = FrameState::Empty;
  resetLiveNal();
}

void begin() {
  if (!videoStopped) {
    return;
  }
  stopRequested = false;
  videoStopped = false;
  rollingImageData = static_cast<uint8_t *>(heap_caps_malloc(kRollingImageBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  mbPixelWords = static_cast<uint32_t *>(heap_caps_malloc(kMbPixelBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  y4Cache = static_cast<uint8_t *>(heap_caps_malloc(kY4Bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  mbRows = static_cast<mbStorage_t (*)[kSourceMbWidth]>(heap_caps_malloc(kMbRowsBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  mbLayer = static_cast<macroblockLayer_t *>(heap_caps_malloc(kMbLayerBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  liveNal = static_cast<uint8_t *>(heap_caps_malloc(kLiveNalBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  rbspScratch = static_cast<uint8_t *>(heap_caps_malloc(kRbspScratchBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  presentBuffer = static_cast<uint8_t *>(heap_caps_malloc(kPresentBytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
  rtpSlots = static_cast<RtpSlot *>(heap_caps_malloc(sizeof(RtpSlot) * kPacketSlotCount, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  const bool buffersReady = y4Cache && mbRows && mbLayer && rollingImageData && mbPixelWords && liveNal && rbspScratch && presentBuffer && rtpSlots;
  if (buffersReady) {
    freeSlots = xQueueCreateStatic(kPacketSlotCount, sizeof(uint8_t), freeSlotQueueStorage, &freeSlotsStorage);
    readySlots = xQueueCreateStatic(kPacketSlotCount, sizeof(uint8_t), readySlotQueueStorage, &readySlotsStorage);
  }
  if (buffersReady && freeSlots && readySlots) {
    for (uint8_t slot = 0; slot < kPacketSlotCount; ++slot) {
      xQueueSend(freeSlots, &slot, 0);
    }
    memset(y4Cache, 0, kY4Bytes);
    memset(rollingImageData, 128, kRollingImageBytes);
    memset(mbPixelWords, 0, kMbPixelBytes);
    memset(mbRows, 0, kMbRowsBytes);
    memset(mbLayer, 0, kMbLayerBytes);
    resetLiveNal();
    stats.lastLogMs = millis();
    const BaseType_t taskStarted = xTaskCreatePinnedToCore(videoTask, "dash-video", kVideoTaskStack, nullptr, 2,
                                                            &videoTaskHandle, 1);
    if (taskStarted == pdPASS) {
      Serial.printf("VIDEO READY frame=%ux%u cache=%u live_nal=%u rolling=%u slots=%u present=%u heap=%u\n",
                    kFrameWidth, kFrameHeight, static_cast<unsigned>(kY4Bytes),
                    static_cast<unsigned>(kLiveNalBytes), static_cast<unsigned>(kMbRowsBytes + kMbLayerBytes),
                    static_cast<unsigned>(kPacketSlotCount), static_cast<unsigned>(kPresentBytes), ESP.getFreeHeap());
      return;
    }
  }
  videoStopped = true;
  freeVideoMemory();
  Serial.printf("VIDEO DISABLED alloc=FAIL heap=%u largest=%u\n", ESP.getFreeHeap(),
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void requestStop() {
  stopRequested = true;
}

bool isStopped() {
  return videoStopped;
}

void release() {
  if (videoStopped) {
    freeVideoMemory();
  }
}

void probePacket(const IPAddress &, uint16_t, const uint8_t *, size_t length) {
  ++probePackets;
  probeBytes += length;
  probeLastPacketMs = millis();
  if (millis() - probeLastLogMs >= 1000) {
    Serial.printf("RTP PROBE pps=%lu bytes=%lu\n", probePackets, probeBytes);
    probeLastLogMs = millis();
    probePackets = 0;
    probeBytes = 0;
  }
}

bool hasRecentRtpProbe() {
  return probeLastPacketMs != 0 && millis() - probeLastPacketMs < 5000;
}

void handlePacket(const IPAddress &, uint16_t, const uint8_t *data, size_t length) {
  uint8_t slotIndex;
  if (stopRequested || !freeSlots || !readySlots || !rtpSlots || length == 0 || length > kPacketBytes ||
      xQueueReceive(freeSlots, &slotIndex, 0) != pdTRUE) {
    ++stats.queueDrops;
    return;
  }
  rtpSlots[slotIndex].length = length;
  memcpy(rtpSlots[slotIndex].data, data, length);
  if (xQueueSend(readySlots, &slotIndex, 0) != pdTRUE) {
    xQueueSend(freeSlots, &slotIndex, 0);
    ++stats.queueDrops;
  }
}

void processPacket(const uint8_t *data, size_t length) {
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
  const uint32_t freeInternalNow = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const uint32_t minInternal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  if (freeInternalNow < kEmergencyHeapBytes || minInternal < kMinDecodeHeapBytes) {
    stopRequested = true;
  }
  if (!stopRequested && frameState == FrameState::Ready) {
    frameState = FrameState::Presenting;
    renderApproxCacheToLcd();
    if (!stopRequested) {
      ++stats.presentedFrames;
      frameState = FrameState::Empty;
    }
  }
  if (now - stats.lastLogMs < kLogIntervalMs) {
    return;
  }

  const uint32_t freeInternal = freeInternalNow;
  Serial.printf(
      "VIDEO pps=%lu kbps=%lu rtp=%lu pt96=%lu au=%lu idr=%lu cont=%lu sei=%lu drop_seq=%lu bad=%lu unk=%lu "
      "nal=%lu ovf=%lu qdrop=%lu qwait=%u idr_bytes=%lu rows=%lu parsed=%lu approx_fps=%lu present=%lu mbs=%lu gray_fps=%lu gmbps=%lu lite_fps=%lu lmbps=%lu skip=%lu afail=%lu pfail=%lu fail_mb=%lu fail_bits=%lu fail_code=%lu nalrbsp=%lu/%lu hdr=%lu left=%lu tail=%08lX sps=%ux%u disp=%ux%u heap=%lu min=%lu stack=%u\n",
      stats.packets, (stats.bytes * 8UL) / 1000UL, stats.rtpPackets, stats.payloadType96, stats.accessUnits,
      stats.idrStarts, stats.continuations, stats.seiPackets, stats.sequenceDrops, stats.badRtp, stats.unsupportedNal,
      stats.liveNalComplete, stats.liveNalOverflow, stats.queueDrops,
      readySlots ? static_cast<unsigned>(uxQueueMessagesWaiting(readySlots)) : 0,
      stats.latestIdrBytes, stats.rowWorkspaceBytes, stats.parsedIdr,
      stats.approxFrames, stats.presentedFrames, stats.approxMbs, stats.grayFrames, stats.grayMbs, stats.liteFrames, stats.liteMbs,
      stats.decodeSkipped, stats.approxFail, stats.parseFail,
      stats.lastFailMb, stats.lastFailBits, stats.lastFailCode,
      stats.lastNalBytes, stats.lastRbspBytes, stats.lastSliceBits, stats.lastBitsLeft, stats.lastNalTail,
      h264.mbWidth * 16, h264.mbHeight * 16, h264.displayWidth, h264.displayHeight, freeInternal, minInternal,
      videoTaskHandle ? static_cast<unsigned>(uxTaskGetStackHighWaterMark(videoTaskHandle)) : 0);
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
  stats.grayFrames = 0;
  stats.grayMbs = 0;
  stats.liteFrames = 0;
  stats.liteMbs = 0;
  stats.presentedFrames = 0;
  stats.decodeSkipped = 0;
  stats.queueDrops = 0;
  stats.lastFailMb = 0;
  stats.lastFailBits = 0;
  stats.lastFailCode = 0;
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
void probePacket(const IPAddress &, uint16_t, const uint8_t *, size_t) {}
bool hasRecentRtpProbe() { return false; }
void handlePacket(const IPAddress &, uint16_t, const uint8_t *, size_t) {}
void update() {}
void requestStop() {}
bool isStopped() { return true; }
void release() {}
bool hasLiveIdr() { return false; }
size_t latestIdrSize() { return 0; }

}  // namespace navdash_video

#endif
