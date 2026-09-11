#pragma once

#include "config.h"

#ifdef MQTT_ENABLED
void mqttCallback(char* topic, byte* payload, unsigned int length);
void publishDiscovery();
void publishMessage();
void clearLastMsg();
void clearSensorBtnText();
void mqttSlug(const char* name, char* out, int maxLen);
void publishSensorDisc(const String& sender, const char* slug);
bool publishSensorMessage();
void publishStatus();
void setupMQTT();
void tickRetryConnections();
#endif
