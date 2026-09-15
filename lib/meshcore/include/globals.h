#pragma once

#include "config.h"

extern unsigned long lastRxDisplay;
extern unsigned long lastDisplayUpdate;
extern SX1262 radio;
extern MeshChannel channels[MAX_CHANNELS];
extern int numChannels;
extern String privateChannelName;
extern int privateChannelIdx;
extern String sensorChannelName;
extern int sensorChannelIdx;
extern String sensorDeviceDisc[SENSOR_DEV_CACHE_MAX];
extern int sensorDeviceDiscCount;
extern unsigned long sensorLastActive[SENSOR_DEV_CACHE_MAX];
extern bool sensorOnlineNow[SENSOR_DEV_CACHE_MAX];
extern bool sensorDiscPublished[SENSOR_DEV_CACHE_MAX];
extern String sensorFwVersion[SENSOR_DEV_CACHE_MAX];
extern String sensorBoard[SENSOR_DEV_CACHE_MAX];
extern String sensorEnv[SENSOR_DEV_CACHE_MAX];   // окружение сборки из hello
extern int sensorBattery[SENSOR_DEV_CACHE_MAX];
extern float sensorRssi[SENSOR_DEV_CACHE_MAX];
extern unsigned long timeSyncMs;
extern bool isListening;
extern int packetCount;
extern String lastMessage;
extern String lastSender;
extern String lastChannelName;
extern char lastPath[100];
extern float lastRSSI;
extern float lastSNR;
extern uint8_t lastHopCount;
extern int lastChannelIdx;
extern PeerEntry peerCache[PEER_CACHE_MAX];
extern uint8_t bot_priv[32];
extern uint8_t bot_pub[32];
extern uint8_t bot_prv64[64];
extern uint8_t ownShortHash;
extern unsigned long lastDirectAdvertMs;
extern unsigned long lastFloodAdvertMs;
extern bool advertBootSent;
extern bool otaFastMode;
extern volatile bool otaRawDidTx;   // rawTxFrame выставляет = true; main сбрасывает перед otaHandleRawFrame
extern unsigned long lastReArmMs;
extern uint32_t fastRxFrames;
extern uint32_t fastRxErrors;
extern uint8_t seen_hashes[SEEN_HASH_COUNT * SEEN_HASH_SIZE];
extern int seen_next_idx;
extern uint8_t seen_advert_hashes[SEEN_ADVERT_HASH_COUNT * SEEN_HASH_SIZE];
extern int seen_advert_next_idx;
extern uint32_t duplicateCount;
extern String logTail;
extern const char fwMarker[];   // FW_MARKER, зашит в образ для проверки платы

#ifdef MQTT_ENABLED
extern WiFiClient wifiClient;
extern PubSubClient mqtt;
extern char mqttPrefix[64];
extern bool mqttConnected;
extern bool wifiConnected;
extern unsigned long lastMqttReconnectMs;
extern unsigned long lastStatusPublishMs;
extern bool wifiConnInProgress;
extern unsigned long wifiConnStartMs;
extern bool discoveryPublished;
extern bool ntpStarted;
extern bool ntpSyncedLogged;
extern unsigned long lastNtpSyncMs;
extern unsigned long lastSensorAvailCheckMs;
extern unsigned long lastSensorTimeSyncMs;
extern int mqttTxChannel;
extern unsigned long lastmsgClearAt;
extern bool lastmsgPendingClear;
extern unsigned long snsBtnClearAt;
extern bool snsBtnPendingClear;
extern char snsBtnSlug[48];
extern uint8_t otaPhase;
extern String otaTarget;
extern File otaFile;
extern bool otaSaving;
extern bool otaSaveOk;
extern bool otaFwReady;
extern uint32_t otaFwSize;
extern uint32_t otaFwCrc;
extern uint32_t otaSeq;
extern uint32_t otaSentBytes;
extern uint8_t otaRetries;
extern unsigned long otaSince;
extern WebServer otaServer;
extern unsigned long otaWriteCalls;
extern unsigned long otaWriteBytes;
extern unsigned long otaWriteSkipped;
#endif

#ifdef SENSOR_NODE
extern bool otaActive;
extern bool otaGotStart;
extern uint32_t otaTotal;
extern uint32_t otaGot;
extern uint32_t otaCrcExp;
extern uint32_t otaCrcAcc;
extern uint32_t otaSeqExp;
extern unsigned long otaLastActivity;
extern unsigned long otaAwaitEndMs;   // таймер «образ принят, жду DONE» (0 — не вошло)
extern String sensorLastSent;
extern unsigned long sensorLastSentMs;
extern unsigned long sensorHelloDueMs;
extern bool fwVersionDiffers;
// Проверка связи по тройному нажатию: запрос, ответ и что показать на экране
extern uint16_t pingId;             // номер текущего запроса, 0 — запроса не было
extern unsigned long pingSentMs;    // когда ушёл запрос; 0 — ответа не ждём
extern unsigned long pingRttMs;     // время ответа, мс
extern float pingRssi, pingSnr;     // как сенсор слышит ответ
extern int pingPeerRssi;            // как координатор слышит сенсор (из ответа)
extern uint8_t pingHops;            // через сколько ретрансляторов пришёл ответ
extern unsigned long pingShowUntil; // до какого времени держать результат на экране
extern bool pingFailed;             // ответа не было
#endif
