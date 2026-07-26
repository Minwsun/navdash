#pragma once

#include <Arduino.h>
#include <IPAddress.h>

namespace navdash_h264_collector {

void begin();
void handlePacket(const IPAddress &remote, uint16_t remotePort, const uint8_t *data, size_t length);
void update();

}  // namespace navdash_h264_collector