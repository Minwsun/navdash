#include "navdash_lcd.h"

#include <SPI.h>

#if __has_include("royal_frame.h")
#include "royal_frame.h"
#define NAVDASH_HAS_ROYAL_FRAME 1
#else
#define NAVDASH_HAS_ROYAL_FRAME 0
#endif

#ifndef TFT_BL
#define TFT_BL -1
#endif

namespace navdash_lcd {

constexpr uint16_t kTftWidth = 320;
constexpr uint16_t kTftHeight = 240;
constexpr uint16_t kViewX = 40;
constexpr uint16_t kViewY = 24;
constexpr uint16_t kViewWidth = 240;
constexpr uint16_t kViewHeight = 180;
constexpr uint16_t kByteMapWidth = 120;
constexpr uint16_t kByteMapHeight = 80;
constexpr uint32_t kDrawIntervalMs = 90;
constexpr uint32_t kFrameDrawIntervalMs = 250;

SPISettings tftSpiSettings(40000000, MSBFIRST, SPI_MODE0);
uint32_t lastDrawMs;
uint32_t lastFrameDrawMs;
uint16_t byteMapY;
bool initialized;

void select() {
  SPI.beginTransaction(tftSpiSettings);
  digitalWrite(TFT_CS, LOW);
}

void deselect() {
  digitalWrite(TFT_CS, HIGH);
  SPI.endTransaction();
}

void command(uint8_t value) {
  select();
  digitalWrite(TFT_DC, LOW);
  SPI.transfer(value);
  deselect();
}

void data(const uint8_t *value, size_t length) {
  select();
  digitalWrite(TFT_DC, HIGH);
  SPI.writeBytes(value, length);
  deselect();
}

void data8(uint8_t value) {
  data(&value, 1);
}

void setAddress(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  uint8_t value[4];
  command(0x2A);
  value[0] = x0 >> 8; value[1] = x0; value[2] = x1 >> 8; value[3] = x1;
  data(value, sizeof(value));
  command(0x2B);
  value[0] = y0 >> 8; value[1] = y0; value[2] = y1 >> 8; value[3] = y1;
  data(value, sizeof(value));
  command(0x2C);
}

void fillRect(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t color) {
  if (!initialized || x >= kTftWidth || y >= kTftHeight || width == 0 || height == 0) {
    return;
  }
  if (x + width > kTftWidth) width = kTftWidth - x;
  if (y + height > kTftHeight) height = kTftHeight - y;
  uint8_t line[kTftWidth * 2];
  for (uint16_t index = 0; index < width; ++index) {
    line[index * 2] = color >> 8;
    line[index * 2 + 1] = color;
  }
  setAddress(x, y, x + width - 1, y + height - 1);
  select();
  digitalWrite(TFT_DC, HIGH);
  for (uint16_t row = 0; row < height; ++row) {
    SPI.writeBytes(line, width * 2);
  }
  deselect();
}

uint16_t byteColor(uint8_t value) {
  const uint8_t r = value >> 3;
  const uint8_t g = (value ^ 0x55) >> 2;
  const uint8_t b = (value ^ 0xA5) >> 3;
  return (static_cast<uint16_t>(r) << 11) | (static_cast<uint16_t>(g) << 5) | b;
}

uint16_t projectedColor(uint8_t value, uint8_t edge) {
  const uint8_t y = value >> 3;
  const uint8_t contrast = (value ^ edge) >> 4;
  if (contrast > 10) {
    return 0x07FF;
  }
  if ((value & 0xC0) == 0x80) {
    return 0x039F;
  }
  if ((value & 0x30) == 0x20) {
    return 0x07E0;
  }
  return (static_cast<uint16_t>(y) << 11) | (static_cast<uint16_t>(y >> 1) << 5) | y;
}

void begin() {
  pinMode(TFT_CS, OUTPUT);
  pinMode(TFT_DC, OUTPUT);
  if (TFT_RST >= 0) {
    pinMode(TFT_RST, OUTPUT);
  }
  if (TFT_BL >= 0) {
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
  }
  digitalWrite(TFT_CS, HIGH);
  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
  if (TFT_RST >= 0) {
    digitalWrite(TFT_RST, HIGH);
    delay(5);
    digitalWrite(TFT_RST, LOW);
    delay(20);
    digitalWrite(TFT_RST, HIGH);
    delay(120);
  }

  command(0x01);
  delay(120);
  command(0x28);
  command(0x3A); data8(0x55);
  command(0x36); data8(0x28);
  command(0x11);
  delay(120);
  command(0x29);
  delay(20);
  initialized = true;

  fillRect(0, 0, kTftWidth, kTftHeight, 0x0000);
  Serial.println("TFT READY live-h264 ili9341v");
}

void updateVideoStatus(uint32_t packets, uint32_t idr, uint32_t bytes, bool live) {
  (void)packets;
  (void)idr;
  (void)bytes;
  (void)live;
}

void drawH264Bytes(const uint8_t *payload, size_t length, bool newFrame) {
  if (!initialized || length == 0 || millis() - lastDrawMs < kDrawIntervalMs) {
    return;
  }
  lastDrawMs = millis();
  if (newFrame || byteMapY >= kByteMapHeight) {
    byteMapY = 0;
    fillRect(kViewX, kViewY, kViewWidth, kViewHeight, 0x0000);
  }

  uint8_t line[kByteMapWidth * 2];
  const size_t count = min<size_t>(length, kByteMapWidth);
  for (size_t index = 0; index < count; ++index) {
    const uint16_t color = byteColor(payload[index]);
    line[index * 2] = color >> 8;
    line[index * 2 + 1] = color;
  }
  for (size_t index = count; index < kByteMapWidth; ++index) {
    line[index * 2] = 0;
    line[index * 2 + 1] = 0;
  }
  const uint16_t x = kViewX + 60;
  const uint16_t y = kViewY + 50 + byteMapY;
  setAddress(x, y, x + kByteMapWidth - 1, y);
  data(line, sizeof(line));
  ++byteMapY;
}

void drawCompressedIdrFrame(const uint8_t *payload, size_t length) {
  if (!initialized || length < 128 || millis() - lastFrameDrawMs < kFrameDrawIntervalMs) {
    return;
  }
  lastFrameDrawMs = millis();

  uint8_t line[kViewWidth * 2];
  for (uint16_t y = 0; y < kViewHeight; ++y) {
    for (uint16_t x = 0; x < kViewWidth; ++x) {
      const size_t index = (static_cast<size_t>(y) * kViewWidth + x) * length / (kViewWidth * kViewHeight);
      const uint8_t value = payload[index];
      const uint8_t edge = payload[(index + 97) % length];
      const uint16_t color = projectedColor(value, edge);
      line[x * 2] = color >> 8;
      line[x * 2 + 1] = color;
    }
    setAddress(kViewX, kViewY + y, kViewX + kViewWidth - 1, kViewY + y);
    data(line, sizeof(line));
  }
}

void drawRgb565Line(uint16_t x, uint16_t y, const uint8_t *rgb565, uint16_t width) {
  drawRgb565Block(x, y, rgb565, width, 1);
}

void drawRgb565Block(uint16_t x, uint16_t y, const uint8_t *rgb565, uint16_t width, uint16_t height) {
  if (!initialized || y >= kTftHeight || x >= kTftWidth || width == 0 || height == 0) {
    return;
  }
  if (x + width > kTftWidth) {
    width = kTftWidth - x;
  }
  if (y + height > kTftHeight) {
    height = kTftHeight - y;
  }
  setAddress(x, y, x + width - 1, y + height - 1);
  data(rgb565, static_cast<size_t>(width) * height * 2);
}

void drawRoyalFrame() {
#if NAVDASH_HAS_ROYAL_FRAME
  if (!initialized) {
    return;
  }
  uint8_t line[kRoyalFrameWidth * 2];
  const uint16_t x0 = (kTftWidth - kRoyalFrameWidth) / 2;
  const uint16_t y0 = (kTftHeight - kRoyalFrameHeight) / 2;
  for (uint16_t y = 0; y < kRoyalFrameHeight; ++y) {
    for (uint16_t x = 0; x < kRoyalFrameWidth; ++x) {
      const uint16_t color = pgm_read_word(&kRoyalFrameRgb565[static_cast<size_t>(y) * kRoyalFrameWidth + x]);
      line[x * 2] = color >> 8;
      line[x * 2 + 1] = color;
    }
    drawRgb565Line(x0, y0 + y, line, kRoyalFrameWidth);
  }
  Serial.println("TFT REAL_FRAME drawn");
#else
  Serial.println("TFT REAL_FRAME missing include/royal_frame.h");
#endif
}

}  // namespace navdash_lcd
