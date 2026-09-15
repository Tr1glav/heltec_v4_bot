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
bool sensorDiscPublished[SENSOR_DEV_CACHE_MAX]; // HA discovery сенсора отправлен в текущее подключение к брокеру
String sensorFwVersion[SENSOR_DEV_CACHE_MAX];   // версия прошивки из hello
String sensorBoard[SENSOR_DEV_CACHE_MAX];       // короткий код платы из hello ("h43", "h3")
String sensorEnv[SENSOR_DEV_CACHE_MAX];         // окружение сборки: по нему берётся файл релиза
int sensorBattery[SENSOR_DEV_CACHE_MAX];        // заряд % из hello; -1 — сенсор его не шлёт
float sensorRssi[SENSOR_DEV_CACHE_MAX];         // RSSI последнего пакета от сенсора
unsigned long timeSyncMs = 0;                   // millis() последнего "time:" из канала сенсоров
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
uint32_t fastRxFrames = 0;   // кадров mesh OTA, принятых на быстром канале с момента переключения
uint32_t fastRxErrors = 0;   // ошибок приёма (CRC и т.п.) на быстром канале
uint8_t seen_hashes[SEEN_HASH_COUNT * SEEN_HASH_SIZE];
int seen_next_idx = 0;
uint8_t seen_advert_hashes[SEEN_ADVERT_HASH_COUNT * SEEN_HASH_SIZE];
int seen_advert_next_idx = 0;
uint32_t duplicateCount = 0;
String logTail;
// used + печать в setup(): иначе линковщик с --gc-sections выбросит строку из образа
const char fwMarker[] __attribute__((used)) = FW_MARKER;
#ifdef MQTT_ENABLED
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// Префикс топиков: meshcore/bot/{имя узла из настроек}/
char mqttPrefix[64];

// Состояние
bool mqttConnected = false;
bool wifiConnected = false;
unsigned long lastMqttReconnectMs = 0;
unsigned long lastStatusPublishMs = 0;

// Неблокирующая машина состояния соединения (WiFi -> MQTT) из loop
bool wifiConnInProgress = false;
unsigned long wifiConnStartMs = 0;
bool discoveryPublished = false;   // discovery бота публикуется один раз
bool ntpStarted = false;           // SNTP запущен
bool ntpSyncedLogged = false;      // лог факта синхронизации — один раз
unsigned long lastNtpSyncMs = 0;
unsigned long lastSensorAvailCheckMs = 0;
unsigned long lastSensorTimeSyncMs = 0;

// Выбранный канал для отправки из HA (index в channels[])
int mqttTxChannel = 1;  // по умолчанию #connections

// Обнуление lastmsg через LASTMSG_RESET_MS после публикации (для повторных триггеров HA)
unsigned long lastmsgClearAt = 0;
bool lastmsgPendingClear = false;

// Триггер "button": через SNS_BTN_CLEAR_MS обнуляем text, чтобы повторное нажатие
// снова вызывало "state_changed" в HA
unsigned long snsBtnClearAt = 0;
bool snsBtnPendingClear = false;
char snsBtnSlug[48];       // slug датчика, текст которого нужно обнулить

uint8_t otaPhase = OTA_PHASE_IDLE;
String otaTarget = "";
File otaFile;               // открытый /ota.bin (LittleFS)
bool otaSaving = false;     // идёт HTTP-загрузка файла на бот
bool otaSaveOk = false;     // флаг успеха сохранения (для POST-ответа)
bool otaFwReady = false;    // на боте лежит годный .otaz
uint32_t otaFwSize = 0;     // длина сжатого потока
uint32_t otaFwCrc = 0;      // CRC32 распакованного образа
uint32_t otaSeq = 0;        // первый неподтверждённый чанк
uint32_t otaSentBytes = 0;  // байт, подтверждённых сенсором
uint8_t otaRetries = 0;     // повторы подряд без прогресса
unsigned long otaSince = 0; // millis() последней отправки
WebServer otaServer(3232);
unsigned long otaWriteCalls = 0;    // сколько раз вызвали WRITE
unsigned long otaWriteBytes = 0;    // сколько байт otaFile.write() подтвердил
unsigned long otaWriteSkipped = 0;  // WRITE-колбэков, где guard не прошёл
#endif // MQTT_ENABLED
#ifdef SENSOR_NODE
bool otaActive = false;     // OTA-сессия идёт (receiving)
bool otaGotStart = false;   // получили ota:start
uint32_t otaTotal = 0;      // ожидаемый размер образа (байт)
uint32_t otaGot = 0;        // записано байт образа
uint32_t otaCrcExp = 0;     // ожидаемый CRC32 образа
uint32_t otaCrcAcc = 0xFFFFFFFF;  // накапливаемый CRC32
uint32_t otaSeqExp = 0;     // следующий ожидаемый seq
unsigned long otaLastActivity = 0; // millis() последнего OTA-пакета
unsigned long otaAwaitEndMs = 0;   // millis() начала ожидания DONE после приёма всего образа
String sensorLastSent = "";        // последнее сообщение, отправленное сенсором (для экрана)
unsigned long sensorLastSentMs = 0; // millis() его отправки
unsigned long sensorHelloDueMs = 0; // когда ответить на "hello?" (0 — запроса нет)
bool fwVersionDiffers = false;      // версия бота из "time:" не совпала со своей
uint16_t pingId = 0;
unsigned long pingSentMs = 0;
unsigned long pingRttMs = 0;
float pingRssi = 0, pingSnr = 0;
int pingPeerRssi = 0;
uint8_t pingHops = 0;
unsigned long pingShowUntil = 0;
bool pingFailed = false;
#endif // SENSOR_NODE
