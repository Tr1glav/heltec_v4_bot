#pragma once

#include "config.h"

#ifdef MQTT_ENABLED
void mqttCallback(char* topic, byte* payload, unsigned int length);
void publishDiscovery();
void publishMessage();
void clearLastMsg();
void clearSensorBtnText();
void mqttSlug(const char* name, char* out, int maxLen);
// env — имя окружения сборки узла: по нему карточка в HA получает роль в названии
void publishSensorDisc(const String& sender, const char* slug, const String& env);
void publishSensorAvailability(int idx);
bool publishSensorMessage();
void publishStatus();
void setupMQTT();
void tickRetryConnections();
#endif
