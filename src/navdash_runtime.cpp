#include "navdash_runtime.h"

#include <Arduino.h>

#include "navdash_connection.h"
#include "navdash_lcd.h"
#include "navdash_video.h"

namespace navdash_runtime {

namespace {

bool videoStarted;
uint32_t nextVideoStartMs;

}  // namespace

void begin() {
#if NAVDASH_ENABLE_LCD
  navdash_lcd::begin();
#if NAVDASH_SHOW_REAL_FRAME
  navdash_lcd::drawRoyalFrame();
#endif
#endif

  navdash_connection::begin();
#if NAVDASH_ENABLE_VIDEO
  navdash_connection::setVideoPacketHandler(navdash_video::probePacket);
#endif
  Serial.println("RUNTIME control=READY baseline-cadence");
}

void update() {
  navdash_connection::update();
#if NAVDASH_ENABLE_VIDEO
  // ponytail: first RTP packet is sacrificed; Royal all-IDR recovers on the next frame.
  if (!videoStarted && navdash_video::hasRecentRtpProbe() && millis() >= nextVideoStartMs) {
    navdash_video::begin();
    if (!navdash_video::isStopped()) {
      navdash_connection::setVideoPacketHandler(navdash_video::handlePacket);
      videoStarted = true;
    } else {
      nextVideoStartMs = millis() + 30000;
    }
  }
  if (videoStarted && !navdash_connection::isStationConnected()) {
    navdash_connection::clearVideoPacketHandler();
    navdash_video::requestStop();
  }
  if (videoStarted && navdash_video::isStopped()) {
    navdash_video::release();
    videoStarted = false;
    navdash_connection::setVideoPacketHandler(navdash_video::probePacket);
    nextVideoStartMs = millis() + 30000;
  }
#endif
}

}  // namespace navdash_runtime