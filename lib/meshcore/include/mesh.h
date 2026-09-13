#pragma once

#include "config.h"

void initAdvertIdentity();
uint8_t* findPeerPub(uint8_t hash);
void rememberPeerPub(uint8_t hash, const uint8_t* pub);
bool checkAndMarkSeen(uint8_t* data, int len);
void addChannelKey16(const char* name, const uint8_t* key16);
void deriveChannels();
int findChannelByName(const char* name);
// Завести или заменить канал в ячейке idx (idx == numChannels — добавить в конец).
// Возвращает номер канала или -1. Сохранением занимается вызывающий.
int channelSetSlot(int idx, const char* name, const uint8_t* key16);
void loadPrivateChannel();
void loadSensorChannel();
#ifdef MQTT_ENABLED
void loadTxChannel();
#endif
void buildPingReply(char* out, size_t outlen, const uint8_t* path, uint8_t hop_count, uint8_t path_hash_size);
int buildGroupEnc(int chIdx, const String& msg, uint8_t* enc);
int buildGroupFrameFlood(int chIdx, const String& msg, uint8_t* frame, int maxlen);
int buildPrivateTextFrame(uint8_t dest_hash, const uint8_t* dest_pub,
                          const String& msg, uint8_t* frame, int maxlen);
int sendFrame(int chIdx, const uint8_t* frame, int f);
// Одно и то же сообщение уходит в эфир несколько раз: приёмник отбрасывает дубликаты по
// хэшу, а лишняя копия спасает от коллизии. Двух копий достаточно — третья только занимала
// эфир и задерживала следующую передачу.
void floodSend(int chIdx, const uint8_t* frame, int f, unsigned int gapMs = FLOOD_RETRY_MS,
               int repeats = 2);
void sensorSendMsg(const char* msg, unsigned int gapMs = FLOOD_RETRY_MS, int repeats = 2);
#ifdef SENSOR_NODE
void sensorSendHello();
#ifdef SENSOR_NODE
void sensorPingSend();   // эхо-запрос к координатору (тройное нажатие)
void sensorPingTick();   // сторож ожидания ответа
#endif
#endif
void sendAdvert(uint8_t route_type);
void sendSensorTimeSync();
String channelListStr();
bool parseMeshCorePacket(uint8_t* data, int len);

// forward decls referenced from parseMeshCorePacket
#ifdef MQTT_ENABLED
void otaHandleAck();
bool publishSensorMessage();
#endif
#ifdef SENSOR_NODE
void otaSensorHandle();
#endif
