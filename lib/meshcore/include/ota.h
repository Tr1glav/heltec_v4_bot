#pragma once

#include "config.h"

void slog(const char* fmt, ...);
#ifdef MQTT_ENABLED
void otaTxGroup(const String& msg);
void otaBotAbort(const char* why);
void otaDrawProgress();
void otaSendStart();
void otaSendChunk();
void otaSendEnd();
void otaHandleAck();
void otaBotTick();
String buildDiagReport();
void setupOtaServer();
#endif
#ifdef SENSOR_NODE
void otaSensorSend(const String& msg, bool flood = false, unsigned int staggerMs = 0);
void otaSensorDraw();
void otaSensorAbort(const char* why);
void otaSensorTick();
void otaSensorHandle();
#endif
void sensorSendMsg(const char* msg);
void otaHandleRawFrame(const uint8_t* buf, int len);
