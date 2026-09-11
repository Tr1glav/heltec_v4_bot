#define DISPLAY_DEFINE_HERE
#include "config.h"
#include "globals.h"

unsigned long lastRxDisplay = 0;        // millis() последнего экрана, связанного с приёмом RX
unsigned long lastDisplayUpdate = 0;    // millis() последнего обновления idle-экрана

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);
MeshChannel channels[MAX_CHANNELS];
int numChannels = 0;
String privateChannelName = "";
int privateChannelIdx = -1;
String sensorChannelName = "";
int sensorChannelIdx = -1;
String sensorDeviceDisc[SENSOR_DEV_CACHE_MAX];
int sensorDeviceDiscCount = 0;
unsigned long sensorLastActive[SENSOR_DEV_CACHE_MAX];
bool sensorOnlineNow[SENSOR_DEV_CACHE_MAX];
bool isListening = false;
int packetCount = 0;
String lastMessage = "";
String lastSender = "";
String lastChannelName = "#public";
char lastPath[100] = "";
float lastRSSI = 0;
float lastSNR = 0;
uint8_t lastHopCount = 0;
int lastChannelIdx = -1;   // индекс канала последнего сообщения (-1 = не определён)
bool screenOff = false;
unsigned long lastScreenActivityMs = 0;
uint8_t replyPath[MAX_REPLY_PATH];
uint8_t replyHopCount = 0;
uint8_t replyHashSize = 1;
String pingReplyText = "";
uint8_t pingReplyEnc[256];
int pingReplyEncLen = 0;
uint8_t dmSrcHash = 0;
uint8_t dmReplyFrame[300];
PeerEntry peerCache[PEER_CACHE_MAX];
uint8_t bot_priv[32];
uint8_t bot_pub[32];
uint8_t bot_prv64[64];   // ed25519 private key (seed-расширенный) для X25519
uint8_t ownShortHash = 0;
unsigned long lastDirectAdvertMs = 0;
unsigned long lastFloodAdvertMs = 0;
bool advertBootSent = false;
bool otaFastMode = false;
volatile bool otaRawDidTx = false;
unsigned long lastReArmMs = 0;
uint8_t seen_hashes[SEEN_HASH_COUNT * SEEN_HASH_SIZE];
int seen_next_idx = 0;
uint8_t seen_advert_hashes[SEEN_ADVERT_HASH_COUNT * SEEN_HASH_SIZE];
int seen_advert_next_idx = 0;
uint32_t duplicateCount = 0;
char sysTimeStr[32] = "?";
String logTail;
#ifdef MQTT_ENABLED
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// Префикс топиков: meshcore/bot/{DEVICE_NAME}/
char mqttPrefix[64];
char mqttDiscoveryPrefix[64];   // homeassistant

// Состояние
bool mqttConnected = false;
bool wifiConnected = false;
unsigned long lastMqttReconnectMs = 0;
unsigned long lastStatusPublishMs = 0;
#define MQTT_STATUS_INTERVAL_MS  60000
#define MQTT_RECONNECT_INTERVAL_MS 5000

// Неблокирующая машина состояния соединения (WiFi -> MQTT) из loop
bool wifiConnInProgress = false;
unsigned long wifiConnStartMs = 0;
bool discoveryPublished = false;   // discovery публикуется один раз (плюс при смене канала)
bool ntpStarted = false;           // SNTP-синхронизация не чаще одного раза
bool ntpSyncedLogged = false;      // лог факта синхронизации — один раз
#define NTP_RESYNC_INTERVAL_MS (60UL * 60 * 1000)   // ре-синк раз в час
unsigned long lastNtpSyncMs = 0;
unsigned long lastSensorAvailCheckMs = 0;
unsigned long lastSensorTimeSyncMs = 0;
#define SENSOR_TIME_SYNC_INTERVAL_MS (5UL * 60 * 1000)   // время сенсорам — раз в 5 минут

// Выбранный канал для отправки из HA (index в channels[])
int mqttTxChannel = 1;  // по умолчанию #connections

// Обнуление lastmsg через некоторое время после публикации (для повторных триггеров HA)
#define LASTMSG_RESET_MS 4000
unsigned long lastmsgClearAt = 0;
bool lastmsgPendingClear = false;

// ---- Триггер "button": после публикации обнуляем text через N сек,
//      чтобы повторное нажатие снова вызывало "state_changed" в HA. ----
#define SNS_BTN_CLEAR_MS 500
unsigned long snsBtnClearAt = 0;
bool snsBtnPendingClear = false;
char snsBtnSlug[48];       // slug датчика, текст которого нужно обнулить

// Буферы для кадров TX из HA
uint8_t mqttTxFrame[300];
int mqttTxFrameLen = 0;
bool mqttTxPending = false;
uint8_t otaPhase = OTA_PHASE_IDLE;
String otaTarget = "";
File otaFile;               // открытый /ota.bin (LittleFS)
bool otaSaving = false;     // идёт HTTP-загрузка .bin на бот
bool otaSaveOk = false;     // флаг успеха сохранения (для POST-ответа)
bool otaFwReady = false;    // /ota.bin сохранён на боте
uint32_t otaFwSize = 0;
uint32_t otaFwCrc = 0;
uint32_t otaSeq = 0;        // seq последнего отправленного чанка
uint32_t otaSentBytes = 0;  // байт, подтверждённых сенсором
uint8_t otaRetries = 0;     // повторы подряд по таймауту/nack
unsigned long otaSince = 0; // millis() последней отправки
bool otaRawMode = false;    // чистая LoRa OTA (raw-фреймы) вместо legacy mesh-текста
WebServer otaServer(3232);
bool otaFlashing = false;
unsigned long otaStartMs = 0;
unsigned long otaWriteCalls = 0;    // сколько раз вызвали WRITE
unsigned long otaWriteBytes = 0;    // сколько байт otaFile.write() подтвердил
unsigned long otaWriteSkipped = 0;  // WRITE-колбэков, где guard не прошёл
#endif // MQTT_ENABLED
#ifdef SENSOR_NODE
bool otaActive = false;     // OTA-сессия идёт (receiving)
bool otaGotStart = false;   // получили ota:start
uint32_t otaTotal = 0;      // ожидаемый размер (байт)
uint32_t otaGot = 0;        // принято байт
uint32_t otaCrcExp = 0;     // ожидаемый CRC32 всего файла
uint32_t otaCrcAcc = 0xFFFFFFFF;  // накапливаемый CRC32
uint32_t otaSeqExp = 0;     // следующий ожидаемый seq
unsigned long otaLastActivity = 0; // millis() последнего OTA-пакета
#endif // SENSOR_NODE
