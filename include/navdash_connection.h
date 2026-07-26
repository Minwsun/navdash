#pragma once

#include <Arduino.h>
#include <IPAddress.h>

namespace navdash_connection {

using VideoPacketHandler = void (*)(const IPAddress &remote, uint16_t remotePort, const uint8_t *data, size_t length);

void begin();
void update();
bool isAuthenticated();
bool isStationConnected();
void setVideoPacketHandler(VideoPacketHandler handler);
void clearVideoPacketHandler();

}  // namespace navdash_connection
