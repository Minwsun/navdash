#include "royal_dash.h"
#include "navdash_lcd.h"
#include "navdash_video.h"

void setup() {
  Serial.begin(115200);
#if NAVDASH_ENABLE_LCD
  navdash_lcd::begin();
#endif
#if NAVDASH_ENABLE_VIDEO
  navdash_video::begin();
  royal_dash::setVideoPacketHandler(navdash_video::handlePacket);
#endif
  royal_dash::begin();
}

void loop() {
  royal_dash::update();
#if NAVDASH_ENABLE_VIDEO
  navdash_video::update();
#endif
}
