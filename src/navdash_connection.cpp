#include "navdash_connection.h"

#include "royal_dash.h"

namespace navdash_connection {

void begin() {
  royal_dash::begin();
}

void update() {
  royal_dash::update();
}

bool isAuthenticated() {
  return royal_dash::isAuthenticated();
}

bool isStationConnected() {
  return royal_dash::isStationConnected();
}

void setVideoPacketHandler(VideoPacketHandler handler) {
  royal_dash::setVideoPacketHandler(handler);
}

void clearVideoPacketHandler() {
  royal_dash::setVideoPacketHandler(nullptr);
}

}  // namespace navdash_connection
