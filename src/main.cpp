#include <Arduino.h>

#if NAVDASH_H264_PROBE
#include "navdash_h264_collector.h"
#include "royal_dash.h"
#elif NAVDASH_PAIRING_BASELINE
#include "royal_dash.h"
#else
#include "navdash_runtime.h"
#endif

void setup() {
  Serial.begin(115200);
#if NAVDASH_H264_PROBE
  royal_dash::begin();
  navdash_h264_collector::begin();
  royal_dash::setVideoPacketHandler(navdash_h264_collector::handlePacket);
#elif NAVDASH_PAIRING_BASELINE
  royal_dash::begin();
#else
  navdash_runtime::begin();
#endif
}

void loop() {
#if NAVDASH_H264_PROBE
  royal_dash::update();
  navdash_h264_collector::update();
#elif NAVDASH_PAIRING_BASELINE
  royal_dash::update();
#else
  navdash_runtime::update();
#endif
}