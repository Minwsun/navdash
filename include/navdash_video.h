#pragma once

#include <Arduino.h>
#include <IPAddress.h>

namespace navdash_video {

void begin();
void probePacket(const IPAddress &remote, uint16_t remotePort, const uint8_t *data, size_t length);
bool hasRecentRtpProbe();
void handlePacket(const IPAddress &remote, uint16_t remotePort, const uint8_t *data, size_t length);
void update();
void requestStop();
bool isStopped();
void release();
bool hasLiveIdr();
size_t latestIdrSize();

}  // namespace navdash_video
