#pragma once

#include <Arduino.h>

namespace navdash_lcd {

void begin();
void updateVideoStatus(uint32_t packets, uint32_t idr, uint32_t bytes, bool live);
void drawH264Bytes(const uint8_t *data, size_t length, bool newFrame);
void drawCompressedIdrFrame(const uint8_t *data, size_t length);
void drawRgb565Line(uint16_t x, uint16_t y, const uint8_t *rgb565, uint16_t width);

}  // namespace navdash_lcd
