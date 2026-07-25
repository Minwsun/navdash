#pragma once

#include <Arduino.h>
#include <IPAddress.h>

namespace royal_dash {

using VideoPacketHandler = void (*)(const IPAddress &remote, uint16_t remotePort, const uint8_t *data, size_t length);

void begin();
void update();
void setVideoPacketHandler(VideoPacketHandler handler);

}
