#pragma once

#include "config.h"

void initAdvertIdentity();
uint8_t* findPeerPub(uint8_t hash);
void rememberPeerPub(uint8_t hash, const uint8_t* pub);
bool checkAndMarkSeen(uint8_t* data, int len);
void addChannelKey16(const char* name, const uint8_t* key16);
void deriveChannels();
int findChannelByName(const char* name);
int setPrivateChannel(const String& name, const String& keyb64);
void loadPrivateChannel();
int setSensorChannel(const String& name, const String& keyb64);
void loadSensorChannel();
#ifdef MQTT_ENABLED
void loadTxChannel();
#endif
void buildPingReply(char* out, size_t outlen, const uint8_t* path, uint8_t hop_count, uint8_t path_hash_size);
int buildGroupEnc(int chIdx, const String& msg, uint8_t* enc);
int buildGroupFrameFlood(int chIdx, const String& msg, uint8_t* frame, int maxlen,
                         const uint8_t* enc_in = NULL, int enc_in_len = 0);
int buildGroupFrameReturnPath(int chIdx, const String& msg,
                              const uint8_t* path, uint8_t hop_count, uint8_t hash_size,
                              uint8_t* frame, int maxlen,
                              const uint8_t* enc_in = NULL, int enc_in_len = 0);
int buildPrivateTextFrame(uint8_t dest_hash, const uint8_t* dest_pub,
                          const String& msg, uint8_t* frame, int maxlen);
int sendFrame(int chIdx, const uint8_t* frame, int f);
void floodSend3(int chIdx, const uint8_t* frame, int f, unsigned int gapMs = 100);
void sendAdvert(uint8_t route_type);
void sendSensorTimeSync();
String channelListStr();
bool parseMeshCorePacket(uint8_t* data, int len);

// forward decls referenced from parseMeshCorePacket
#ifdef MQTT_ENABLED
void otaHandleAck();
#endif
#ifdef SENSOR_NODE
void otaSensorHandle();
#endif
bool publishSensorMessage();
