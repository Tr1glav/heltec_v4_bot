#include <Arduino.h>
#include <Wire.h>
#include <RadioLib.h>
#include <SPI.h>
#include <mbedtls/md.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <Ed25519.h>
#include <ed_25519.h>   // Nightcracker ed25519: X25519 key exchange для ответов в личку
#include <string.h>
#include <stdarg.h>
#include <sys/time.h>
#include <time.h>
#include <Update.h>        // OTA-запись (бот по WiFi + сенсор по mesh)

#ifdef MQTT_ENABLED
#include <WiFi.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <esp_sntp.h>
#include <esp_partition.h>
#endif

// Per-board pins (Heltec V4.3 by default), radio params and display driver
// are mapped here from build_flags — see README / platformio.ini.
#include "board_config.h"

// Имя устройства (идёт в заголовке исходящих сообщений)
#ifndef DEVICE_NAME
#define DEVICE_NAME "Tr1glav_esp_bot"
#endif
#if defined(SENSOR_NODE) && defined(DEVICE_NAME_SENSOR)
#undef DEVICE_NAME
#define DEVICE_NAME DEVICE_NAME_SENSOR
#endif

// Hash ноды (этим хэшем приложение адресует ЛИЧНОЕ сообщение) — в MeshCore это
// просто первый байт Ed25519-публичного ключа устройства (PATH_HASH_SIZE=1).
// Мы определяем его автоматически из нашего advertise-key; можно переопределить
// принудительно через -DBOT_ID_HASH=0xNN в platformio.ini.
#ifdef BOT_ID_HASH
uint8_t ownShortHash = BOT_ID_HASH;
#else
uint8_t ownShortHash = 0xFF;
#endif

// Часовой пояс: фиксированное смещение от UTC (Европа/Москва = UTC+3, без DST).
// Делаем вручную, т.к. setenv("TZ")/tzset на ESP-IDF капризны, а settimeofday
// с tz=NULL сбрасывает пояс в UTC.
#define TZ_OFFSET_HOURS 3
#define LOCAL_TZ "Europe/Moscow UTC+3"

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);

// ===== КАНАЛЫ =====
// #public использует FIXED PSK (base64 "izOH6cXN6mrJ5e26oRXNcg==" -> 16 байт).
// #connections создан приложением с автоключом: SHA256("#connections")[0:16].
// В обоих случаях секрет в эфире: 32 байта = 16 ключа + 16 нулей,
// хэш канала = SHA256(ключ16)[0], AES по первым 16 байтам, HMAC по 32.
struct MeshChannel {
    uint8_t secret[32];
    uint8_t hash;
    const char* name;
};
#define MAX_CHANNELS 4
MeshChannel channels[MAX_CHANNELS];
int numChannels = 0;

// Приватный канал, задаваемый из build-флагов (secrets.ini), не из HA.
// Имя типа "#garage" + ключ PSK (base64, 16 байт, как у #public). Если ключ
// пустой — автоключ SHA256(name)[0:16] (как у #connections).
String privateChannelName = "";
int privateChannelIdx = -1;

// Второй приватный канал — для взаимодействия датчиков между собой.
// Сообщения с него публикуются в MQTT как ОТДЕЛЬНОЕ устройство HA по имени отправителя
// (см. publishSensorMessage). Задаётся из build-флагов (secrets.ini).
String sensorChannelName = "";
int sensorChannelIdx = -1;

// Кэш имён отправителей, для которых discovery уже опубликован (retained).
#define SENSOR_DEV_CACHE_MAX 8
String sensorDeviceDisc[SENSOR_DEV_CACHE_MAX];
int sensorDeviceDiscCount = 0;
unsigned long sensorLastActive[SENSOR_DEV_CACHE_MAX];
bool sensorOnlineNow[SENSOR_DEV_CACHE_MAX];

// Пауза без сообщений от датчика, после которой его availability уходит в offline.
// Должен быть больше SENSOR_HEARTBEAT_MS (10 мин), чтобы hello не попадал на границу.
#ifndef SENSOR_OFFLINE_MS
#define SENSOR_OFFLINE_MS (15UL * 60 * 1000)
#endif

// ---- Сенсорные сообщения: кнопка = триггер, hello = регулярный heartbeat ----
#define SENSOR_MSG_BUTTON "button"      // одиночное нажатие кнопки (триггер для автоматизаций)
#define SENSOR_MSG_BUTTON2 "button2"    // двойное нажатие кнопки
#define SENSOR_MSG_HELLO   "hello"      // периодический heartbeat (доступность)
// Окно ожидания второго нажатия после отпускания кнопки (двойной клик).
#ifndef SNS_BTN_DBL_WINDOW_MS
#define SNS_BTN_DBL_WINDOW_MS 700
#endif
// Период авто-рассылки heartbeat сенсором (кнопка шлёт "button", hello — таймером).
#ifndef SENSOR_HEARTBEAT_MS
#define SENSOR_HEARTBEAT_MS (10UL * 60 * 1000)
#endif

// ===== ДЕФОЛТЫ ИЗ BUILD-ФЛАГОВ (secrets.ini) =====
// Применяются только если NVS пуст (приоритет: MQTT/NVS > эти флаги).
#ifndef PRIVATE_CHANNEL_NAME
#define PRIVATE_CHANNEL_NAME ""
#endif
#ifndef PRIVATE_CHANNEL_KEY
#define PRIVATE_CHANNEL_KEY ""
#endif
#ifndef SENSOR_CHANNEL_NAME
#define SENSOR_CHANNEL_NAME ""
#endif
#ifndef SENSOR_CHANNEL_KEY
#define SENSOR_CHANNEL_KEY ""
#endif
#ifndef TX_CHANNEL
#define TX_CHANNEL "#connections"
#endif

// ===== ПЕРЕМЕННЫЕ =====
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
bool buttonPressed = false;
unsigned long lastDisplayUpdate = 0;
unsigned long lastRxDisplay = 0;

// ===== АВТОВЫКЛЮЧЕНИЕ ЭКРАНА =====
// Через 5 минут без активности (приём пакета/кнопка) гасим OLED; будит кнопка.
#define SCREEN_AUTO_OFF_MS (5UL * 60 * 1000)
bool screenOff = false;
unsigned long lastScreenActivityMs = 0;

// ===== ТЕМПЕРАТУРА (встроенный датчик ESP32-S3) =====
// У многих модулей eFuse-калибровка датчика не прошита, и чтение «плавает»
// (типично ~50-60°C на холодной плате). Коррекция — build-флагом:
//   -DTEMP_SENSOR_OFFSET=35   (вычитается из значения датчика)
#ifndef TEMP_SENSOR_OFFSET
#define TEMP_SENSOR_OFFSET 0
#endif
float cpuTempC() {
    return temperatureRead() - (float)TEMP_SENSOR_OFFSET;
}

// ===== ОТВЕТ НА /ping: текст, зашифрованный блок =====
// Кадр строится на лету (flood x3), здесь храним только enc и replyPath
// для вычисления хеша seen-фильтра.
#define MAX_REPLY_PATH 63
uint8_t replyPath[MAX_REPLY_PATH];
uint8_t replyHopCount = 0;
uint8_t replyHashSize = 1;
String pingReplyText = "";
uint8_t pingReplyEnc[256];
int pingReplyEncLen = 0;

// ===== ОТВЕТ В ЛИЧКУ (TXT_MSG) =====
uint8_t dmSrcHash = 0;
uint8_t dmReplyFrame[300];

// ===== КЭШ ПУБЛИЧНЫХ КЛЮЧЕЙ НОД =====
// Для шифрования ответа в личку нужен ПОЛНЫЙ pubkey отправителя (32 Б), а в
// TXT_MSG его нет — только 1-байтовый хэш. Собираем ключи из ADVERT-пакетов.
#define PEER_CACHE_MAX 8
struct PeerEntry {
    uint8_t hash;
    uint8_t pub[32];
    uint32_t last_seen;
};
PeerEntry peerCache[PEER_CACHE_MAX];

uint8_t* findPeerPub(uint8_t hash) {
    for (int i = 0; i < PEER_CACHE_MAX; i++) {
        if (peerCache[i].hash == hash && peerCache[i].pub[0] != 0) return peerCache[i].pub;
    }
    return NULL;
}

void rememberPeerPub(uint8_t hash, const uint8_t* pub) {
    int slot = -1;
    uint32_t oldest = 0xFFFFFFFF;
    for (int i = 0; i < PEER_CACHE_MAX; i++) {
        if (peerCache[i].hash == hash) { slot = i; break; }
        if (peerCache[i].last_seen < oldest) { oldest = peerCache[i].last_seen; slot = i; }
    }
    peerCache[slot].hash = hash;
    memcpy(peerCache[slot].pub, pub, 32);
    peerCache[slot].last_seen = millis();
}

// ===== ИДЕНТИЧНОСТЬ НОДЫ ДЛЯ ADVERT =====
// Advert'ы подписываются Ed25519 (rweather/Crypto) — ровно тот же verify,
// который используют ноды MeshCore. Ключ стабильный: seed = SHA256(имя).
uint8_t bot_priv[32];
uint8_t bot_pub[32];
uint8_t bot_prv64[64];   // ed25519 private key (seed-расширенный) для X25519

// Периодичность: direct advert раз в 5 мин, flood advert раз в 30 мин.
#define ADVERT_PERIOD_MS        (5UL * 60 * 1000)
#define ADVERT_FLOOD_PERIOD_MS (30UL * 60 * 1000)
#define ADV_ROUTE_DIRECT 0x02
#define ADV_ROUTE_FLOOD  0x01
unsigned long lastDirectAdvertMs = 0;
unsigned long lastFloodAdvertMs = 0;
bool advertBootSent = false;

// ===== MQTT / HOME ASSISTANT (опционально, -DMQTT_ENABLED) =====
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

// Forward declarations
void setupMQTT();
void tickRetryConnections();
void publishDiscovery();
void publishMessage();
bool publishSensorMessage();   // fwd-decl (bool): вызывается из parseMeshCorePacket
void publishStatus();
void clearLastMsg();
void clearSensorBtnText();
void mqttCallback(char* topic, byte* payload, unsigned int length);
#endif

// Декларация вне #ifdef: вызов из parseMeshCorePacket компилируется во всех сборках
// (в non-MQTT-сборках сенсорный канал не создаётся и вызов недостижим).
bool publishSensorMessage();
void otaSensorHandle();   // sensor side mesh OTA (SENSOR_NODE) — forward decl
void otaHandleAck();      // bot side mesh OTA (MQTT_ENABLED) — forward decl
void slog(const char* fmt, ...);  // Serial + кольцевой хвост для web (/logs)

// ===== MESH OTA (бот -> сенсоры по сенсорному каналу) =====
// Бинарь передаётся пакетами "mesh ota" поверх обычных групповых сообщений.
// Каждый пакет адресуется ПО ИМЕНИ цели (<бот>: ota:<target>:<payload>).
// Сенсор, чьё DEVICE_NAME совпадает с <target>, применяет пакет.
// Протокол (направление бот -> сенсор; сенсор отвечает своим сообщением):
//   bot ->  ota:start:<target>:<total_len>:<crc32_hex>  — инициализация
//   sens -> ota:ackstart                                — готов к приёму
//   bot ->  ota:data:<seq>:<crc16hex>:<hexdata>         — чанк <= OTA_CHUNK_HEX
//   sens -> ota:ack:<seq>                               — чанк принят (CRC ok)
//   sens -> ota:nack:<seq>                              — чанк битый, повторить
//   bot ->  ota:end                                     — всё отправлено
//   sens -> ota:ackend                                  — все байты записаны, CRC32 ок
//   sens -> ota:nackcrc                                 — CRC32 всего файла не совпал
//   sens -> ota:reboot                                  — Update.end() ок, перезагрузка
// Таймаут на ответ сенсора — OTA_ACK_TIMEOUT_MS; ретраев — OTA_MAX_RETRIES.
// Пакет-чанк: "ota:data:<seq>:<crc16>:<hex>" суммарно обязан уложиться в
// лимит текстового сообщения buildGroupEnc (219 символов) + рамки кадра 255 Б.
// 200 hex = 100 байт: кадр = 3 + 2(MAC) + 240(clen) = 245 Б, в бюджет влезает.
#define OTA_CHUNK_HEX 200              // hex-символов данных на пакет (100 байт)
#define OTA_CHUNK_BYTES (OTA_CHUNK_HEX / 2)
#define OTA_ACK_TIMEOUT_MS 400         // весь цикл ~50 мс; 400 мс — запас на случай потери ack
#define OTA_MAX_RETRIES 4

// Повтор ack именно в флуде смертелен: пока сенсор гоняет 3 кадра, его RX-FIFO
// не вычитывается, и приходящий в это время следующий чанк затирается новой
// передачей -> потеря чанка -> таймаут бота (~пакет в секунду). Поэтому ack/nack
// чанка уходит ОДИН раз; флуд оставлен только для ackstart.
#define OTA_ACK_STAGGER_MS 15  // пауза перед ack: бот только закончил свой TX и вернулся в RX,
                               // иначе преамбула ack попадает в его разворот и сгорает
// Сколько приёмников держать прогресс-бар: OLED через I2C занимает ~25 мс на кадр,
// поэтому рисовать на КАЖДЫЙ чанк (±4000 пакетов) — это минуты впустую.
#define OTA_DRAW_MS 100

// Быстрый конфиг радио для mesh OTA (~9× быстрее, ~12 дБ хуже — для связки бот↔сенсор
// на 1-2 м не критично; возврат к штатным параметрам гарантирован при abort/error/reboot).
#define OTA_FAST_FREQ       868.950
#define OTA_FAST_BW         500.0
#define OTA_FAST_SF         7
#define OTA_FAST_CR         5
#define OTA_FAST_SETTLE_MS  800    // задержка бота после ackstart: сенсор (~70 мс) переключается почти сразу

// ===== CRC32 (для сверки целостности всего бинаря после передачи) =====
uint32_t crc32buf(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320UL & -(crc & 1));
    }
    return ~crc;
}
// CRC32 инкрементально: crc должен стартовать 0xFFFFFFFF, завершить ~crc32()
uint32_t crc32_upd(uint32_t crc, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320UL & -(crc & 1));
    }
    return crc;
}
// CRC16-CCITT для чанка
uint16_t crc16buf(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
    }
    return crc;
}
// 4 hex-символа -> uint16 (для crc16 в пакете)
uint16_t hexToU16(const char* s) {
    uint16_t v = 0;
    for (int i = 0; i < 4 && s[i]; i++) {
        v <<= 4;
        char c = s[i];
        if (c >= '0' && c <= '9') v |= (c - '0');
        else if (c >= 'a' && c <= 'f') v |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (c - 'A' + 10);
    }
    return v;
}
uint8_t hexToChar(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}
// hex-строка (чётная длина) -> байты
int hexToBytes(const char* hex, uint8_t* dst, int maxlen) {
    int n = 0;
    while (hex[n * 2] && hex[n * 2 + 1] && n < maxlen) {
        dst[n] = (uint8_t)(hexToChar(hex[n * 2]) << 4) | hexToChar(hex[n * 2 + 1]);
        n++;
    }
    return n;
}
// байты -> hex (len*2 символов + NUL)
void bytesToHex(const uint8_t* src, size_t len, char* dst) {
    static const char* H = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        dst[i * 2] = H[(src[i] >> 4) & 0x0F];
        dst[i * 2 + 1] = H[src[i] & 0x0F];
    }
    dst[len * 2] = 0;
}

// ===== Состояние mesh OTA =====
enum {
    OTA_PHASE_IDLE = 0,
    OTA_PHASE_WAIT_START,   // ждём ota:ackstart от сенсора
    OTA_PHASE_DATA,         // передаём чанки (ждём ota:ack/<seq>)
    OTA_PHASE_WAIT_END,     // всё отправлено, ждём финальный статус
    OTA_PHASE_DONE
};

// Признак: OTA идёт на быстром конфиге радио. Общий для обоих билдов (бот и сенсор):
// блокирует adverts и ненужные экраны, пока работает быстрый mesh OTA.
bool otaFastMode = false;

#ifdef MQTT_ENABLED
// --- Бот (передающая сторона) ---
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
#endif

#ifdef SENSOR_NODE
// --- Сенсор (принимающая сторона) ---
// Гарантия отката: если OTA завис (пакеты не идут), через OTA_SENSOR_STALL_MS
// сессия прерывается Update.abort() и сенсор остаётся на прежней прошивке.
// ESP32 сам игнорирует неполный образ в неактивном app-слоте при ребуте.
#define OTA_SENSOR_STALL_MS 60000
bool otaActive = false;     // OTA-сессия идёт (receiving)
bool otaGotStart = false;   // получили ota:start
uint32_t otaTotal = 0;      // ожидаемый размер (байт)
uint32_t otaGot = 0;        // принято байт
uint32_t otaCrcExp = 0;     // ожидаемый CRC32 всего файла
uint32_t otaCrcAcc = 0xFFFFFFFF;  // накапливаемый CRC32
uint32_t otaSeqExp = 0;     // следующий ожидаемый seq
unsigned long otaLastActivity = 0; // millis() последнего OTA-пакета
#endif

void initAdvertIdentity() {
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
               (const uint8_t*)DEVICE_NAME, strlen(DEVICE_NAME), bot_priv);
    Ed25519::derivePublicKey(bot_pub, bot_priv);

    // Ed25519 private key (64 Б) для X25519-обмена при ответе в личку.
    // seed = тот же bot_priv, pub должен совпасть с bot_pub (RFC8032).
    uint8_t pub_check[32];
    ed25519_create_keypair(pub_check, bot_prv64, bot_priv);
    if (memcmp(pub_check, bot_pub, 32) != 0) {
        Serial.println("[ADV] WARNING: ed25519 pub mismatch (!)");
    }

    #ifndef BOT_ID_HASH
    ownShortHash = bot_pub[0];   // авто: hash ноды = первый байт pubkey
    #endif
    Serial.printf("[ADV] identity pub: ");
    for (int i = 0; i < 32; i++) Serial.printf("%02X", bot_pub[i]);
    Serial.printf(", own short hash: 0x%02X\n", ownShortHash);
}

// ===== СТРАЖ ЗДОРОВЬЯ LORA =====
// Как в Dispatcher: периодически сбрасываем AGC (warm sleep -> startReceive),
// иначе SX1262 со временем глохнет и перестаёт ловить пакеты ("засыпает").
// Сброс откладывается, пока идёт активный приём.
#define RADIO_REARM_INTERVAL_MS 30000
unsigned long lastReArmMs = 0;

// Warm sleep сбрасывает аналоговый фронтенд (AGC/LNA), затем заново в RX.
void rearmRadioAGC() {
    radio.sleep();
    radio.startReceive();
    lastReArmMs = millis();
}

// ===== ДЕДУПЛИКАЦИЯ (как SimpleMeshTables в MeshCore) =====
// Хэш считается по payload_type + payload (без header/transport/path),
// поэтому копии одного сообщения, пришедшие разными путями, совпадают.
#define SEEN_HASH_SIZE 8
#define SEEN_HASH_COUNT 64
uint8_t seen_hashes[SEEN_HASH_COUNT * SEEN_HASH_SIZE];
int seen_next_idx = 0;
// Отдельный (меньший) кэш для ADVERT: у них уникальный ts, и они не должны
// вытеснять хэши текстовых сообщений — иначе повторы GRP_TXT будут проходить
// повторно и дублироваться в HA.
#define SEEN_ADVERT_HASH_COUNT 16
uint8_t seen_advert_hashes[SEEN_ADVERT_HASH_COUNT * SEEN_HASH_SIZE];
int seen_advert_next_idx = 0;
uint32_t duplicateCount = 0;

// Возвращает true, если пакет уже видели, и помечает его как виденный.
bool checkAndMarkSeen(uint8_t* data, int len) {
    if (len < 2) return true;  // битый пакет
    uint8_t header = data[0];
    uint8_t payload_type = (header >> 2) & 0x0F;
    int offset = 1;
    if (((header & 0x03) == 0x00) || ((header & 0x03) == 0x03)) offset += 4;
    if (offset >= len) return true;
    uint8_t path_len = data[offset++];
    uint8_t hash_size = (path_len >> 6) + 1;
    uint8_t hop_count = path_len & 0x3F;
    offset += hop_count * hash_size;
    if (offset >= len) return true;

    uint8_t hash_ctx[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, &payload_type, 1);
    mbedtls_md_update(&ctx, &data[offset], len - offset);
    mbedtls_md_finish(&ctx, hash_ctx);
    mbedtls_md_free(&ctx);

    // выбираем кольцевой буфер по типу пакета
    int count  = (payload_type == 0x04) ? SEEN_ADVERT_HASH_COUNT : SEEN_HASH_COUNT;
    uint8_t* hashes = (payload_type == 0x04) ? seen_advert_hashes : seen_hashes;
    int* nextIdx    = (payload_type == 0x04) ? &seen_advert_next_idx : &seen_next_idx;

    // ищем в кольцевом буфере
    for (int i = 0; i < count; i++) {
        if (memcmp(hash_ctx, &hashes[i * SEEN_HASH_SIZE], SEEN_HASH_SIZE) == 0) {
            return true;
        }
    }
    // помечаем
    memcpy(&hashes[*nextIdx * SEEN_HASH_SIZE], hash_ctx, SEEN_HASH_SIZE);
    *nextIdx = (*nextIdx + 1) % count;
    return false;
}

// ===== ВЫЧИСЛЕНИЕ КЛЮЧЕЙ КАНАЛОВ =====
// Устанавливает канал: secret32 = key16 + 16 нулей, hash = SHA256(key16)[0].
void addChannelKey16(const char* name, const uint8_t* key16) {
    MeshChannel& ch = channels[numChannels];
    memset(ch.secret, 0, sizeof(ch.secret));
    memcpy(ch.secret, key16, 16);
    uint8_t sha256_result[32];
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), ch.secret, 16, sha256_result);
    ch.hash = sha256_result[0];
    ch.name = name;
    numChannels++;
    Serial.printf("Channel %s: key ", name);
    for (int i = 0; i < 16; i++) Serial.printf("%02x", ch.secret[i]);
    Serial.printf(", hash 0x%02X\n", ch.hash);
}

void loadPrivateChannel();   // fwd-decl (определён ниже, вызывается из deriveChannels)
void loadSensorChannel();    // fwd-decl (сенсорам нужна без MQTT)

void deriveChannels() {
    uint8_t key16[16];

    // #public: фиксированный PSK из MeshCore
    const char* psk_b64 = "izOH6cXN6mrJ5e26oRXNcg==";
    size_t olen = 0;
    memset(key16, 0, sizeof(key16));
    mbedtls_base64_decode(key16, 16, &olen, (const uint8_t*)psk_b64, strlen(psk_b64));
    addChannelKey16("#public", key16);

    // #connections: автоключ = SHA256("#connections")[0:16]
    const char* name = "#connections";
    uint8_t sha256_result[32];
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
               (const uint8_t*)name, strlen(name), sha256_result);
    memcpy(key16, sha256_result, 16);
    addChannelKey16(name, key16);

    // Приватный канал MQTT-бота — из build-флагов.
    #ifdef MQTT_ENABLED
    loadPrivateChannel();
    #endif
    // Сенсорный канал: нужен и MQTT-боту, и сенсорным платам (без WiFi),
    // поэтому добавляем всегда. Источник — build-флаги (secrets.ini).
    loadSensorChannel();
}

// ===== ПРИВАТНЫЙ КАНАЛ + КАНАЛ СЕНСОРОВ (имя + ключ из build-флагов) =====
// Каналы добавляются в ту же таблицу и шифруются так же, как #public/#connections.
// Задаются ТОЛЬКО из secrets.ini (-DPRIVATE_CHANNEL_NAME/KEY, -DSENSOR_CHANNEL_NAME/KEY),
// в HA настраивать нельзя. Ведение каналов не зависит от MQTT — функции определены
// вне #ifdef MQTT_ENABLED (используются и сенсорной платой без WiFi).
int findChannelByName(const char* name) {
    for (int i = 0; i < numChannels; i++) {
        if (strcmp(channels[i].name, name) == 0) return i;
    }
    return -1;
}

// Ключ для приватного канала: "" = автоключ, иначе base64(16 байт).
// Возвращает 0 при неудаче (тогда применяется автоключ).
int privateKeyTo16(const String& keyb64, uint8_t key16[16]) {
    if (keyb64.length() == 0) return -1;   // автоключ
    size_t olen = 0;
    uint8_t tmp[32];
    int rc = mbedtls_base64_decode(tmp, sizeof(tmp), &olen,
                                   (const uint8_t*)keyb64.c_str(), keyb64.length());
    if (rc != 0 || olen != 16) {
        Serial.printf("[PRV] bad PSK '%s' (need 16 raw bytes in base64), using auto key\n", keyb64.c_str());
        return -1;
    }
    memcpy(key16, tmp, 16);
    return 1;                              // PSK-ключ
}

// Добавляет/обновляет приватный канал. name + keyb64 ("" = автоключ).
int setPrivateChannel(const String& name, const String& keyb64) {
    if (name.length() == 0 || name.length() > 32) return -1;
    uint8_t key16[16];
    int keyType = privateKeyTo16(keyb64, key16);
    if (keyType < 0) {   // автоключ
        uint8_t sha256_result[32];
        mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                   (const uint8_t*)name.c_str(), name.length(), sha256_result);
        memcpy(key16, sha256_result, 16);
    }

    int idx = findChannelByName(name.c_str());
    if (idx >= 0) {
        // канал существует — проверяем, не поменялся ли ключ
        if (memcmp(channels[idx].secret, key16, 16) != 0) {
            memset(channels[idx].secret, 0, sizeof(channels[idx].secret));
            memcpy(channels[idx].secret, key16, 16);
            uint8_t sha256_result[32];
            mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                       channels[idx].secret, 16, sha256_result);
            channels[idx].hash = sha256_result[0];
            Serial.printf("[PRV] channel %s key UPDATED, hash 0x%02X\n",
                          name.c_str(), channels[idx].hash);
        }
        privateChannelIdx = idx;
        privateChannelName = channels[idx].name;
        return idx;
    }
    if (numChannels >= MAX_CHANNELS) {
        Serial.println("[PRV] MAX_CHANNELS reached, channel not added");
        return -1;
    }
    privateChannelName = name;             // держим имя в String
    addChannelKey16(privateChannelName.c_str(), key16);
    privateChannelIdx = numChannels - 1;
    Serial.printf("[PRV] private channel added: %s key+hash 0x%02X (idx %d)\n",
                  privateChannelName.c_str(), channels[privateChannelIdx].hash,
                  privateChannelIdx);
    return privateChannelIdx;
}

void loadPrivateChannel() {
    // Каналы задаются ТОЛЬКО из build-флагов (secrets.ini), не из HA.
    if (strlen(PRIVATE_CHANNEL_NAME) > 0) {
        Serial.printf("[PRV] channel from build flags: %s\n", PRIVATE_CHANNEL_NAME);
        setPrivateChannel(PRIVATE_CHANNEL_NAME, PRIVATE_CHANNEL_KEY);
    }
}

// Канал отправки из HA не настраивается — только из build-флагов (TX_CHANNEL).
#ifdef MQTT_ENABLED
void loadTxChannel() {
    int idx = findChannelByName(TX_CHANNEL);
    if (idx < 0) idx = 1;   // #connections
    if (idx < numChannels) {
        mqttTxChannel = idx;
        Serial.printf("[MQTT] TX channel from build flags: %s\n", channels[idx].name);
    }
}
#endif // MQTT_ENABLED

// ===== ВТОРОЙ ПРИВАТНЫЙ КАНАЛ (сенсоры) =====
// Добавляет/обновляет канал датчиков (name + keyb64; "" = автоключ). Зеркало
// setPrivateChannel, но ведёт свою пару сенсорных настроек.
int setSensorChannel(const String& name, const String& keyb64) {
    if (name.length() == 0 || name.length() > 32) return -1;
    uint8_t key16[16];
    int keyType = privateKeyTo16(keyb64, key16);
    if (keyType < 0) {   // автоключ
        uint8_t sha256_result[32];
        mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                   (const uint8_t*)name.c_str(), name.length(), sha256_result);
        memcpy(key16, sha256_result, 16);
    }

    int idx = findChannelByName(name.c_str());
    if (idx >= 0) {
        // канал существует — проверяем, не поменялся ли ключ
        if (memcmp(channels[idx].secret, key16, 16) != 0) {
            memset(channels[idx].secret, 0, sizeof(channels[idx].secret));
            memcpy(channels[idx].secret, key16, 16);
            uint8_t sha256_result[32];
            mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                       channels[idx].secret, 16, sha256_result);
            channels[idx].hash = sha256_result[0];
            Serial.printf("[SNS] channel %s key UPDATED, hash 0x%02X\n",
                          name.c_str(), channels[idx].hash);
        }
        sensorChannelIdx = idx;
        sensorChannelName = channels[idx].name;
        return idx;
    }
    if (numChannels >= MAX_CHANNELS) {
        Serial.println("[SNS] MAX_CHANNELS reached, channel not added");
        return -1;
    }
    sensorChannelName = name;
    addChannelKey16(sensorChannelName.c_str(), key16);
    sensorChannelIdx = numChannels - 1;
    Serial.printf("[SNS] sensor channel added: %s key+hash 0x%02X (idx %d)\n",
                  sensorChannelName.c_str(), channels[sensorChannelIdx].hash,
                  sensorChannelIdx);
    return sensorChannelIdx;
}

void loadSensorChannel() {
    // Каналы задаются ТОЛЬКО из build-флагов (secrets.ini), не из HA.
    if (strlen(SENSOR_CHANNEL_NAME) > 0) {
        Serial.printf("[SNS] sensor channel from build flags: %s\n", SENSOR_CHANNEL_NAME);
        setSensorChannel(SENSOR_CHANNEL_NAME, SENSOR_CHANNEL_KEY);
    }
}

// ===== ШИФРОВАНИЕ GRP_TXT (encrypt-then-MAC как в MeshCore) =====
// src_len >= 1. dest: [MAC 2B][ciphertext]; возвращает 2 + padded len.
int encryptGroupText(const uint8_t* secret32, uint8_t* dest, const uint8_t* src, int src_len) {
    if (src_len <= 0) return 0;
    int padded = (src_len + 15) & ~15;
    uint8_t* cipher = dest + 2;  // MAC спереди

    // AES-128-ECB по блокам, хвост добиваем нулями
    uint8_t block[16];
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, secret32, 128);
    for (int i = 0; i < padded; i += 16) {
        memset(block, 0, 16);
        memcpy(block, src + i, min(16, src_len - i));
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, block, cipher + i);
    }
    mbedtls_aes_free(&aes);

    // HMAC-SHA256(cipher) с полным 32-байтным секретом
    uint8_t hmac_out[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    secret32, 32, cipher, padded, hmac_out);
    dest[0] = hmac_out[0];
    dest[1] = hmac_out[1];
    return 2 + padded;
}

// ===== РАСШИФРОВКА GRP_TXT (MAC-then-decrypt как в MeshCore) =====
String decryptGroupText(const uint8_t* secret32, uint8_t* mac, uint8_t* ciphertext, int len) {
    if (len <= 0 || len % 16 != 0) return "";

    // Проверяем HMAC-SHA256(ciphertext) с полным 32-байтным секретом
    uint8_t hmac_out[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    secret32, 32,
                    ciphertext, len, hmac_out);
    if (hmac_out[0] != mac[0] || hmac_out[1] != mac[1]) return "";

    // AES-128-ECB расшифровка
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, secret32, 128);

    uint8_t plaintext[256];
    for (int i = 0; i < len; i += 16) {
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, ciphertext + i, plaintext + i);
    }
    mbedtls_aes_free(&aes);

    // plaintext: [timestamp 4B][txt_type 1B][text...]
    String message = "";
    for (int i = 5; i < len; i++) {
        if (plaintext[i] == 0) break;
        message += (char)plaintext[i];
    }
    return message;
}

// ===== ОТВЕТ НА /ping =====
int buildGroupEnc(int chIdx, const String& msg, uint8_t* enc);  // fwd decl
int buildGroupFrameFlood(int chIdx, const String& msg, uint8_t* frame, int maxlen,
                         const uint8_t* enc_in = NULL, int enc_in_len = 0);  // fwd decl
int buildGroupFrameReturnPath(int chIdx, const String& msg, const uint8_t* path,
                              uint8_t hop_count, uint8_t hash_size,
                              uint8_t* frame, int maxlen,
                              const uint8_t* enc_in = NULL, int enc_in_len = 0);  // fwd decl
int sendFrame(int chIdx, const uint8_t* frame, int f);  // fwd decl
int txFrame(uint8_t* frame, int f);  // fwd decl
void floodSend3(int chIdx, const uint8_t* frame, int f, unsigned int gapMs = 100);  // fwd decl
int buildPrivateTextFrame(uint8_t dest_hash, const uint8_t* dest_pub,
                          const String& msg, uint8_t* frame, int maxlen);  // fwd decl
// Формат ответа как в bot.py: "hops:direct" либо "hops:N, route:aa → bb → cc"
void buildPingReply(char* out, size_t outlen, const uint8_t* path, uint8_t hop_count, uint8_t path_hash_size) {
    if (hop_count == 0) {
        snprintf(out, outlen, "hops:direct");
        return;
    }
    snprintf(out, outlen, "hops:%u, route:", hop_count);
    size_t pos = strlen(out);
    for (int h = 0; h < hop_count; h++) {
        if (pos + 3 >= outlen) break;
        if (h > 0) { out[pos++] = ' '; out[pos++] = 0xE2; out[pos++] = 0x86; out[pos++] = 0x92; }  // →
        const uint8_t* ph = path + h * path_hash_size;
        for (int b = 0; b < path_hash_size; b++) {
            snprintf(&out[pos], outlen - pos, "%02x", ph[b]);
            pos += 2;
        }
        out[pos] = 0;
    }
}

// ===== ПАРСИНГ MESHCORE ПАКЕТА =====
bool parseMeshCorePacket(uint8_t* data, int len) {
    if (len < 6) return false;
    
    uint8_t header = data[0];
    uint8_t payload_type = (header >> 2) & 0x0F;
    uint8_t route_type = header & 0x03;
    
    // GRP_TXT (0x05) — групповые сообщения, TXT_MSG (0x02) — личные (ДМ).
    if (payload_type != 0x05 && payload_type != 0x02) {
        // ADVERT (0x04): кэшируем публичные ключи нод — без них не ответить
        // в личку (нужен полный pubkey для X25519). В кадре: [pub 32][ts 4][sig 64][app...].
        if (payload_type == 0x04) {
            int o = 1;
            if (route_type == 0x00 || route_type == 0x03) o += 4;
            if (o < len) {
                uint8_t pl = data[o++];
                o += (pl & 0x3F) * (((pl >> 6) & 3) + 1);   // path bytes
                if (o + 32 + 4 + 64 <= len) {
                    uint8_t* pub = &data[o];
                    uint8_t* ts  = &data[o + 32];
                    uint8_t* sig = &data[o + 36];
                    uint8_t* app = &data[o + 100];
                    int applen = len - (o + 100);
                    if (applen < 0) applen = 0;
                    if (applen > 64) applen = 64;
                    uint8_t msg[32 + 4 + 64];
                    int mlen = 0;
                    memcpy(&msg[mlen], pub, 32); mlen += 32;
                    memcpy(&msg[mlen], ts, 4); mlen += 4;
                    memcpy(&msg[mlen], app, applen); mlen += applen;
                    if (Ed25519::verify(sig, pub, msg, mlen)) {
                        rememberPeerPub(pub[0], pub);
                        Serial.printf("[ADV] cached pubkey for <%02X>\n", pub[0]);
                    } else {
                        Serial.printf("[ADV] bad signature for <%02X>\n", pub[0]);
                    }
                }
            }
        }
        return false;
    }
    
    int offset = 1;
    if (route_type == 0x00 || route_type == 0x03) offset += 4;  // transport codes
    
    if (offset >= len) return false;
    uint8_t path_len = data[offset++];
    uint8_t path_hash_size = (path_len >> 6) + 1;
    uint8_t hop_count = path_len & 0x3F;
    const uint8_t* path_bytes = &data[offset];
    
    // сохраняем путь (хэши ретрансляторов) для отображения
    lastPath[0] = 0;
    if (hop_count > 0 && offset + hop_count * path_hash_size <= len) {
        char tmp[8];
        for (int h = 0; h < hop_count; h++) {
            const uint8_t* ph = &data[offset + h * path_hash_size];
            if (path_hash_size == 1) {
                snprintf(tmp, sizeof(tmp), "%02X ", ph[0]);
            } else {
                snprintf(tmp, sizeof(tmp), "%02X%02X ", ph[0], ph[1]);
            }
            strcat(lastPath, tmp);
        }
    }

    // копия пути для обратного маршрута ответа (хэши ретрансляторов)
    replyHopCount = (hop_count > 0 && offset + hop_count * path_hash_size <= len) ? hop_count : 0;
    if (hop_count > 0 && offset + hop_count * path_hash_size <= len) {
        replyHashSize = path_hash_size;
        memcpy(replyPath, &data[offset], hop_count * path_hash_size);
    }
    offset += hop_count * path_hash_size;
    
    if (offset >= len) return false;
    
    // === Разбор тела пакета: ДМ (TXT_MSG) или групповое (GRP_TXT) ===
    bool personalDm = false;
    int chIdx = -1;
    
    if (payload_type == 0x02) {
        // Личное сообщение: payload = [dest_hash 1B][src_hash 1B][MAC 2B][cipher...].
        // Шифруется общим секретом X25519 — текст без ключей ноды не прочитать,
        // но dest_hash (первый байт) показывает, адресовано ли сообщение НАМ.
        if (offset + 2 > len) return false;
        uint8_t dest_hash = data[offset];
        uint8_t src_hash  = data[offset + 1];
        if (dest_hash != ownShortHash) {
            Serial.printf("[DM] dest=%02X (не нам, наш=%02X) src=%02X, игнор\n", dest_hash, ownShortHash, src_hash);
            return false;
        }
        personalDm = true;
        dmSrcHash = src_hash;
        
        // В ДМ нет хэша канала — отвечаем в #connections (личный канал).
        chIdx = 1;
        if (chIdx >= numChannels) chIdx = 0;
        lastChannelIdx = chIdx;
        lastChannelName = "DM #connections";
        
        char senderHex[8];
        snprintf(senderHex, sizeof(senderHex), "<%02X>", src_hash);
        lastSender = senderHex;
        lastMessage = "(личное сообщение)";
    } else {
        uint8_t channel_hash = data[offset++];
        if (offset + 2 > len) return false;
        
        // ищем канал по хэшу
        for (int i = 0; i < numChannels; i++) {
            if (channel_hash == channels[i].hash) { chIdx = i; break; }
        }
        if (chIdx < 0) return false;
        lastChannelIdx = chIdx;
        lastChannelName = channels[chIdx].name;
        
        uint8_t* mac = &data[offset];         // 2 байта MAC
        uint8_t* ciphertext = &data[offset + 2];  // шифротекст после MAC
        int ciphertext_len = len - (offset + 2);
        
        // обрезаем до кратного 16
        int ciphertext_len_trunc = ciphertext_len & ~15;
        if (ciphertext_len_trunc <= 0) return false;
        String message = decryptGroupText(channels[chIdx].secret, mac, ciphertext, ciphertext_len_trunc);
        
        if (message.length() == 0) {
            Serial.println("[!] HMAC не совпал или пустое сообщение");
            return false;
        }
        
        int colonPos = message.indexOf(": ");
        if (colonPos > 0) {
            lastSender = message.substring(0, colonPos);
            lastMessage = message.substring(colonPos + 2);
        } else {
            lastSender = "?";
            lastMessage = message;
        }
    }
    
    packetCount++;
    lastRSSI = radio.getRSSI();
    lastSNR = radio.getSNR();
    lastHopCount = hop_count;
    
    // убираем хвостовые пробелы/переносы (у некоторых клиентов "/ping \n")
    lastMessage.trim();

    // не обрабатываем собственные сообщения (эхо собственного флуда)
    if (lastSender == DEVICE_NAME) return false;
    
    Serial.printf("\n=== PACKET #%d (%s) ===\n", packetCount, lastChannelName.c_str());
    Serial.printf("From: %s\n", lastSender.c_str());
    Serial.printf("Msg: %s\n", lastMessage.c_str());
    Serial.printf("Route: %s (hops=%u)\n", lastPath[0] ? lastPath : "direct", hop_count);
    Serial.printf("RSSI: %.1f dBm, SNR: %.1f dB\n", lastRSSI, lastSNR);
    
    lastRxDisplay = millis();
    
    // Показать сообщение на экране (если экран не погашен автовыключением)
    if (!screenOff) {
        display.clearDisplay();
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.println(lastChannelName.c_str());
        display.printf("From: %s\n", lastSender.c_str());
        display.printf("RSSI:%.0f SNR:%.0f\n", lastRSSI, lastSNR);
        if (hop_count > 0) {
            display.drawLine(0, 24, 128, 24, SSD1306_WHITE);
            display.setCursor(0, 26);
            display.print("via: ");
            String pathStr = lastPath;
            if (pathStr.length() > 24) pathStr = pathStr.substring(0, 20) + "..";
            display.println(pathStr);
            display.drawLine(0, 36, 128, 36, SSD1306_WHITE);
            display.setCursor(0, 40);
        } else {
            display.drawLine(0, 32, 128, 32, SSD1306_WHITE);
            display.setCursor(0, 36);
        }
        String showMsg = lastMessage;
        if (showMsg.length() > 26) showMsg = showMsg.substring(0, 24) + "..";
        display.println(showMsg);
        display.display();
    }

    // === СЕНСОРНЫЙ КАНАЛ: сообщение уходит в MQTT как отдельное устройство ===
    if (sensorChannelIdx >= 0 && chIdx == sensorChannelIdx) {
        // === MESH OTA: бот принимает ack, сенсор — чанки/управление ===
        if (lastMessage.startsWith("ota:")) {
            #ifdef MQTT_ENABLED
            otaHandleAck();
            #elif defined(SENSOR_NODE)
            otaSensorHandle();
            #endif
            return true;
        }

        // === Синхронизация времени: бот шлёт "time:<epoch>" от NTP ===
        if (lastMessage.startsWith("time:")) {
            uint64_t epoch = (uint64_t)strtoull(lastMessage.c_str() + 5, NULL, 10);
            if (epoch > (uint64_t)BUILD_UNIX_TIME) {
                struct timeval tv;
                tv.tv_sec = (time_t)epoch;
                tv.tv_usec = 0;
                settimeofday(&tv, NULL);
                time_t local = (time_t)epoch + (time_t)TZ_OFFSET_HOURS * 3600;
                struct tm tm_now;
                gmtime_r(&local, &tm_now);
                char tbuf[32];
                strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm_now);
                Serial.printf("[RTC] SYNCED from channel: %s\n", tbuf);
            }
        }
        #ifdef MQTT_ENABLED
        bool snsPub = publishSensorMessage();
        // Диагностика на экран: канал приёма -> статус публикации в MQTT.
        display.drawLine(0, 48, 128, 48, SSD1306_WHITE);
        display.setCursor(0, 50);
        if (snsPub) display.printf("SNS -> MQTT RSSI:%.0f", lastRSSI);
        else        display.printf("SNS RX, MQTT %s", wifiConnected ? "off" : "no-wifi");
        display.display();
        #else
        Serial.printf("[SNS] %s: %s (MQTT disabled)\n", lastSender.c_str(), lastMessage.c_str());
        #endif
        return true;
    }

    // === ОТВЕТ НА СООБЩЕНИЕ ===
    // Только для MQTT-бота. Сенсорный узел — пассивный: отвечает ТОЛЬКО кнопкой
    // ("button") и heartbeat ("hello"), пинг/DM он не обслуживает.
    #ifndef SENSOR_NODE
    // - Личное (TXT_MSG) с dest_hash == BOT_ID_HASH: отвечаем ВСЕГДА, как на /ping.
    // - Групповой GRP_TXT: на текст "/ping" в #connections либо на любое
    //   DIRECT-сообщение, адресованное устройству.
    bool isDirect = (route_type == 0x02 || route_type == 0x03);
    if (personalDm || (chIdx == 1 && lastMessage == "/ping") || isDirect) {
        // Пауза перед ответом: даём отправителю выйти из TX и перейти в RX,
        // иначе его приёмник «задирается» на старт нашей передачи (desense).
        delay(250);

        char reply[100];
        buildPingReply(reply, sizeof(reply), replyPath, replyHopCount, replyHashSize);
        Serial.printf("[PING] reply: %s\n", reply);
        pingReplyText = String(reply);

        // === Личное сообщение: ответ уходит В ЛИЧКУ (TXT_MSG), а не в канал ===
        if (personalDm) {
            uint8_t* peerPub = findPeerPub(dmSrcHash);
            if (peerPub != NULL) {
                int dl = buildPrivateTextFrame(dmSrcHash, peerPub, pingReplyText,
                                               dmReplyFrame, sizeof(dmReplyFrame));
                if (dl > 0) {
                    Serial.printf("\n[TX DM] to <%02X>: %s (%dB, flood x3)\n", dmSrcHash, pingReplyText.c_str(), dl);
                    floodSend3(-1, dmReplyFrame, dl);
                }
            } else {
                Serial.printf("[DM] pubkey <%02X> неизвестен (нет advert) — ответ не отправлен\n", dmSrcHash);
            }
            return true;
        }

        int enclen = buildGroupEnc(chIdx, pingReplyText, pingReplyEnc);
        if (enclen <= 0) return true;
        pingReplyEncLen = enclen;

        uint8_t frame[300];
        int f = buildGroupFrameFlood(chIdx, pingReplyText, frame, sizeof(frame),
                                     pingReplyEnc, pingReplyEncLen);
        if (f > 0) {
            Serial.printf("\n[TX] %s: %s (%dB, flood x3)\n", channels[chIdx].name,
                          (DEVICE_NAME ": " + pingReplyText).c_str(), f);
            floodSend3(chIdx, frame, f);
        }
    }
    #endif  // !SENSOR_NODE (ответ на пинг/DM — только MQTT-бот)

    return true;
}

// ===== ОБЩАЯ ПЕРЕДАЧА =====
// Фем-переключатель + transmit + гарантированный возврат в RX.
int txFrame(uint8_t* frame, int f) {
    #if HAS_FEM
    digitalWrite(FEM_TX_PIN, HIGH);
    #endif
    int st = radio.transmit(frame, f);
    #if HAS_FEM
    digitalWrite(FEM_TX_PIN, LOW);
    #endif

    if (st == RADIOLIB_ERR_NONE) {
        Serial.println("[TX] OK");
    } else {
        Serial.printf("[TX] FAILED %d\n", st);
    }

    // после TX обязательно вернуться в RX; при ошибке — полный ре-арм AGC
    if (radio.startReceive() != RADIOLIB_ERR_NONE) {
        rearmRadioAGC();
    }
    return st;
}

// ===== ОТПРАВКА ADVERT =====
// Пакет: header(тип ADVERT 0x04 | route) + path_len 0 + pub[32] + ts[4]
//       + signature[64] + app_data. Подпись по pub||ts||app.
void sendAdvert(uint8_t route_type) {
    uint8_t app[32];
    int applen = 0;
    app[applen++] = 0x80 | 0x01;  // ADV_TYPE_CHAT + имя
    const char* name = DEVICE_NAME;
    int nlen = strlen(name);
    if (nlen > 31) nlen = 31;
    memcpy(app + applen, name, nlen);
    applen += nlen;

    uint8_t frame[190];
    int f = 0;
    frame[f++] = (uint8_t)((0x04 << 2) | (route_type & 0x03));  // ADVERT | route
    frame[f++] = 0x00;  // path_len: hash_size=1, 0 хопов

    memcpy(frame + f, bot_pub, 32); f += 32;
    uint32_t ts = (uint32_t)time(NULL);
    memcpy(frame + f, &ts, 4); f += 4;

    uint8_t msg[32 + 4 + 32];
    int mlen = 0;
    memcpy(msg + mlen, bot_pub, 32); mlen += 32;
    memcpy(msg + mlen, &ts, 4); mlen += 4;
    memcpy(msg + mlen, app, applen); mlen += applen;

    uint8_t sig[64];
    Ed25519::sign(sig, bot_priv, bot_pub, msg, mlen);
    memcpy(frame + f, sig, 64); f += 64;
    memcpy(frame + f, app, applen); f += applen;

    Serial.printf("\n[TX ADV] route=%u (%dB)\n", route_type, f);
    for (int i = 0; i < f; i++) Serial.printf("%02X", frame[i]);
    Serial.println();
    txFrame(frame, f);
}

// ===== ОТПРАВКА GRP_TXT =====
// Общая часть для обоих вариантов: формирует "DEVICE_NAME: msg",
// шифрует по ключу канала и возвращает зашифрованный блок.
int buildGroupEnc(int chIdx, const String& msg, uint8_t* enc) {
    if (chIdx < 0 || chIdx >= numChannels) return 0;
    MeshChannel& ch = channels[chIdx];

    uint8_t plaintext[256];
    int plen = 0;
    // В ts[4] уходит Unix-время в СЕКУНДАХ (как у adverts и личных сообщений):
    // epoch-ms не помещается в uint32 (переполняется и клиенты показывают ~2046 год).
    // Если время синхронизировано/из BUILD_UNIX_TIME — шлём секунды, иначе millis.
    uint32_t ts = ((uint32_t)time(NULL) > 1000000000)
        ? (uint32_t)time(NULL)
        : (uint32_t)millis();
    memcpy(plaintext, &ts, 4); plen += 4;           // timestamp (LE)
    plaintext[plen++] = 0;                          // TXT_TYPE_PLAIN
    const char* prefix = DEVICE_NAME ": ";
    memcpy(plaintext + plen, prefix, strlen(prefix)); plen += strlen(prefix);
    size_t mlen = min((size_t)219, msg.length());
    memcpy(plaintext + plen, msg.c_str(), mlen); plen += mlen;

    return encryptGroupText(ch.secret, enc, plaintext, plen);
}

// Флуд-броадкаст (header 0x15, path_len = 0; ретрансляторы сами достроят путь).
// Если enc_in задан — используем готовый зашифрованный блок (тот же timestamp),
// иначе шифруем заново. Только СОБИРАЕТ кадр; отправка — sendFrame().
int buildGroupFrameFlood(int chIdx, const String& msg, uint8_t* frame, int maxlen,
                         const uint8_t* enc_in, int enc_in_len) {
    if (chIdx < 0 || chIdx >= numChannels) return 0;
    uint8_t enc[256];
    int enclen;
    if (enc_in != NULL && enc_in_len > 0) {
        enclen = min(enc_in_len, (int)sizeof(enc));
        memcpy(enc, enc_in, enclen);
    } else {
        enclen = buildGroupEnc(chIdx, msg, enc);
    }
    if (enclen <= 0 || 3 + enclen > maxlen) return 0;

    int f = 0;
    frame[f++] = 0x15;        // GRP_TXT | ROUTE_TYPE_FLOOD
    frame[f++] = 0x00;        // path_len: hash_size=1, 0 хопов (построится ретрансляторами)
    frame[f++] = channels[chIdx].hash;
    memcpy(frame + f, enc, enclen); f += enclen;
    return f;
}

// Ответ ПО ОБРАТНОМУ МАРШРУТУ (header 0x16 = GRP_TXT | ROUTE_TYPE_DIRECT).
// Путь, которым пришёл запрос, разворачивается: первый хэш в кадре —
// ближайший к нам ретранслятор, дальше до источника, который получает
// пакет как zero-hop. Ретрансляторы пересылают DIRECT-пакет, только если
// первый хэш пути совпадает с их собственным. Только СОБИРАЕТ кадр.
int buildGroupFrameReturnPath(int chIdx, const String& msg,
                              const uint8_t* path, uint8_t hop_count, uint8_t hash_size,
                              uint8_t* frame, int maxlen,
                              const uint8_t* enc_in, int enc_in_len) {
    if (chIdx < 0 || chIdx >= numChannels) return 0;
    if (hop_count == 0 || hop_count > 0x3F || hash_size == 0 || hash_size > 8) return 0;
    uint8_t enc[256];
    int enclen;
    if (enc_in != NULL && enc_in_len > 0) {
        enclen = min(enc_in_len, (int)sizeof(enc));
        memcpy(enc, enc_in, enclen);
    } else {
        enclen = buildGroupEnc(chIdx, msg, enc);
    }
    if (enclen <= 0) return 0;

    int pathBytes = hop_count * hash_size;
    if (3 + pathBytes + enclen > maxlen) return 0;

    int f = 0;
    frame[f++] = 0x16;        // GRP_TXT | ROUTE_TYPE_DIRECT
    frame[f++] = (uint8_t)(((hash_size - 1) << 6) | hop_count);
    for (int h = 0; h < hop_count; h++) {
        int src = (hop_count - 1 - h) * hash_size;   // разворачиваем путь
        for (int b = 0; b < hash_size; b++) frame[f++] = path[src + b];
    }
    frame[f++] = channels[chIdx].hash;
    memcpy(frame + f, enc, enclen); f += enclen;
    return f;
}

// ===== ОТПРАВКА TXT_MSG (ответ В ЛИЧКУ) =====
// Кадр: header 0x09 (TXT_MSG|FLOOD) + path_len 0x40 +
//       payload[dest_hash 1B][src_hash 1B][MAC 2B][cipher...]
// Данные: [ts u32 LE][attempt 1B][text\0]; шифр AES-128-ECB + HMAC-SHA256
// по X25519 shared secret (ed25519_key_exchange) — как Utils::encryptThenMAC.
int buildPrivateTextFrame(uint8_t dest_hash, const uint8_t* dest_pub,
                          const String& msg, uint8_t* frame, int maxlen) {
    uint8_t secret[32];
    ed25519_key_exchange(secret, dest_pub, bot_prv64);

    uint8_t data[128];
    int dlen = 0;
    uint32_t ts = (uint32_t)time(NULL);   // Unix-секунды (epoch-ms не лезет в uint32)
    memcpy(data, &ts, 4); dlen += 4;
    data[dlen++] = 0;                        // attempt = 0
    size_t ml = min((size_t)96, msg.length());
    memcpy(data + dlen, msg.c_str(), ml); dlen += ml;
    data[dlen++] = 0;                        // null terminator

    uint8_t enc[128];
    int enclen = encryptGroupText(secret, enc, data, dlen);   // [MAC 2B][cipher]
    if (enclen <= 0 || 4 + enclen > maxlen) return 0;

    int f = 0;
    frame[f++] = 0x09;                       // TXT_MSG | ROUTE_TYPE_FLOOD
    frame[f++] = 0x40;                       // path_len: hash_size=2, 0 хопов
    frame[f++] = dest_hash;
    frame[f++] = ownShortHash;
    memcpy(frame + f, enc, enclen); f += enclen;
    return f;
}

// Отправка уже собранного кадра + hex-лог. Возвращает статус RadioLib.
int sendFrame(int chIdx, const uint8_t* frame, int f) {
    if (chIdx < 0 || chIdx >= numChannels) return RADIOLIB_ERR_UNKNOWN;
    // hex-лог кадра стоит ~40 мс на UART для 245-байтного чанка — при mesh OTA
    // на каждый аck/чанк это минуты суммарно, поэтому в fast-режиме молчим.
    if (!otaFastMode) {
        for (int i = 0; i < f; i++) Serial.printf("%02X", frame[i]);
        Serial.println();
    }
    return txFrame((uint8_t*)frame, f);
}

// ===== ФЛУД-ОТПРАВКА С 3 РЕТРАЯМИ =====
// chIdx >= 0: sendFrame (с валидацией канала, hex-лог), chIdx < 0: txFrame (личка).
// Отправляется тот же кадр 3 раза с паузой gapMs между ними; приёмник дедуплицирует.
// OTA-ack'и идут с малым зазором (OTA_ACK_FLOOD_MS), обычные сообщения — 100 мс.
#ifndef FLOOD_RETRY_MS
#define FLOOD_RETRY_MS 100
#endif
void floodSend3(int chIdx, const uint8_t* frame, int f, unsigned int gapMs) {
    for (int i = 0; i < 3; i++) {
        if (chIdx >= 0) sendFrame(chIdx, frame, f);
        else            txFrame((uint8_t*)frame, f);
        if (i < 2) delay(gapMs);
    }
}

// ===== РАССЫЛКА ВРЕМЕНИ В СЕНСОРНЫЙ КАНАЛ =====
// Бот синхронизирован по NTP и периодически шлёт в сенсорный канал команду
// "time:<epoch>". Датчики без WiFi/радио берут из неё точное время.
void sendSensorTimeSync() {
    if (sensorChannelIdx < 0) return;
    uint8_t frame[300];
    char msg[40];
    snprintf(msg, sizeof(msg), "time:%llu", (unsigned long long)time(NULL));
    int f = buildGroupFrameFlood(sensorChannelIdx, msg, frame, sizeof(frame), NULL, 0);
    if (f > 0) {
        Serial.printf("\n[TIME] -> %s: %s\n", channels[sensorChannelIdx].name, msg);
        floodSend3(sensorChannelIdx, frame, f);
    }
}

// ===== ИНИЦИАЛИЗАЦИЯ LORA =====
bool initLoRa() {
    Serial.println("Init LoRa...");
    
    // Вызов begin() БЕЗ параметра TCXO.
    // RadioLib возьмёт значение из макроса SX126X_DIO3_TCXO_VOLTAGE,
    // который мы определим в platformio.ini.
    int state = radio.begin(
        LORA_FREQ, LORA_BW, LORA_SF, LORA_CR,
        LORA_SYNC_WORD, LORA_TX_POWER, LORA_PREAMBLE, 1.8
    );
    
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println("LoRa OK (TCXO from macro)");
        radio.setCRC(true);

        // SX1262-специфичные настройки включаются build_flags'ами
        // (см. platformio.ini / board_config.h), чтобы на других платах
        // не применять опции, нужные только Heltec V4.
        #ifdef SX126X_DIO2_AS_RF_SWITCH
        radio.setDio2AsRfSwitch(true);
        #endif
        #ifdef SX126X_RX_BOOSTED_GAIN
        radio.setRxBoostedGainMode(true);
        #endif
        #ifdef SX126X_CURRENT_LIMIT
        radio.setCurrentLimit(SX126X_CURRENT_LIMIT);
        #endif
        #ifdef SX126X_REGISTER_PATCH
        // патч регистра 0x8B5 для улучшенного приёма на Heltec v4
        uint8_t r_data = 0;
        radio.readRegister(0x8B5, &r_data, 1);
        r_data |= 0x01;
        radio.writeRegister(0x8B5, &r_data, 1);
        #endif
        return true;
    }
    
    Serial.printf("LoRa FAILED: %d\n", state);
    return false;
}

// ===== Переключение параметров радио (для mesh OTA) =====
// Любое переключение заканчивается startReceive(); возврат к штатным параметрам
// гарантирован вызовом radioSetNormalConfig() в otaBotAbort / otaSensorAbort.
void radioSetParams(float freq, float bw, int sf, int cr) {
    radio.standby();
    radio.setFrequency(freq);
    radio.setBandwidth(bw);
    radio.setSpreadingFactor(sf);
    radio.setCodingRate(cr);
    lastReArmMs = 0;           // разрешить AGC rearm сразу
    radio.startReceive();
    isListening = true;
}

void radioSetNormalConfig() { radioSetParams(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR); }
void radioSetFastConfig()   { radioSetParams(OTA_FAST_FREQ, OTA_FAST_BW, OTA_FAST_SF, OTA_FAST_CR); }

// Компилятором задавалось BUILD_UNIX_TIME из времени хоста (см. scripts/gen_build_time.py)
#ifndef BUILD_UNIX_TIME
#define BUILD_UNIX_TIME 0
#endif

char sysTimeStr[32] = "?";

// Устанавливает системные часы ESP32-S3 в момент старта из времени хоста
// в момент сборки/прошивки. NTP не используется.
void initSystemClock() {
    struct timeval tv;
    tv.tv_sec = BUILD_UNIX_TIME;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);

    // Локальное время считаем вручную: UTC + фиксированное смещение.
    time_t local = (time_t)BUILD_UNIX_TIME + (time_t)TZ_OFFSET_HOURS * 3600;
    struct tm tm_now;
    gmtime_r(&local, &tm_now);
    strftime(sysTimeStr, sizeof(sysTimeStr), "%Y-%m-%d %H:%M:%S", &tm_now);
    Serial.printf("[RTC] SysTime set from host: %s (%s)\n", sysTimeStr, LOCAL_TZ);
    Serial.printf("[RTC] epoch=%lld\n", (long long)time(NULL));
}

// =====================================================================
// MQTT / HOME ASSISTANT  (только при -DMQTT_ENABLED)
// =====================================================================
#ifdef MQTT_ENABLED

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    char msg[256];
    int mlen = min((unsigned int)255, length);
    memcpy(msg, payload, mlen);
    msg[mlen] = 0;

    // --- meshcore/bot/.../cmd/send ---
    if (strstr(topic, "/cmd/send")) {
        if (!isListening) {
            Serial.println("[MQTT] radio off, ignoring send cmd");
            return;
        }
        String txt = msg;
        uint8_t frame[300];
        int fl = buildGroupFrameFlood(mqttTxChannel, txt, frame, sizeof(frame), NULL, 0);
        if (fl > 0) {
            Serial.printf("[MQTT TX] %s: %s (%dB)\n", channels[mqttTxChannel].name,
                          (String(DEVICE_NAME) + ": " + txt).c_str(), fl);
            floodSend3(mqttTxChannel, frame, fl);
        }
        return;
    }

    // --- meshcore/bot/.../cmd/listening ---
    if (strstr(topic, "/cmd/listening")) {
        if (strncmp(msg, "ON", 2) == 0 && !isListening) {
            isListening = true;
            radio.startReceive();
            Serial.println("[MQTT] listening ON");
        } else if (strncmp(msg, "OFF", 3) == 0 && isListening) {
            isListening = false;
            radio.standby();
            Serial.println("[MQTT] listening OFF");
        }
        char stateTopic[96];
        snprintf(stateTopic, sizeof(stateTopic), "%s/state", mqttPrefix);
        mqtt.publish(stateTopic, isListening ? "ON" : "OFF", true);
        return;
    }

}

void publishDiscovery() {
    // Префикс HA: homeassistant
    // Device block общий для всех сущностей
    char devBlock[256];
    snprintf(devBlock, sizeof(devBlock),
        "\"identifiers\":[\"meshcore_bot_%s\"],"
        "\"name\":\"%s\","
        "\"manufacturer\":\"MeshCore\","
        "\"model\":\"ESP32-S3 Listener\"",
        DEVICE_NAME, DEVICE_NAME);

    char topic[128], payload[512];

    // --- Sensor: последний отправитель (state = имя отправителя) ---
    snprintf(topic, sizeof(topic), "homeassistant/sensor/meshcore_%s/last_sender/config", DEVICE_NAME);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s LastSender\","
        "\"state_topic\":\"%s/state\","
        "\"value_template\":\"{{ value_json.sender }}\","
        "\"json_attributes_topic\":\"%s/state\","
        "\"unique_id\":\"meshcore_%s_lastsender\","
        "\"device\":{%s}}",
        DEVICE_NAME, mqttPrefix, mqttPrefix, DEVICE_NAME, devBlock);
    mqtt.publish(topic, payload, true);

    // --- Sensor: статус ---
    snprintf(topic, sizeof(topic), "homeassistant/sensor/meshcore_%s/status/config", DEVICE_NAME);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s Status\","
        "\"state_topic\":\"%s/status\","
        "\"value_template\":\"{{ value_json.uptime }}\","
        "\"json_attributes_topic\":\"%s/status\","
        "\"unique_id\":\"meshcore_%s_stat\","
        "\"icon\":\"mdi:server\","
        "\"device\":{%s}}",
        DEVICE_NAME, mqttPrefix, mqttPrefix, DEVICE_NAME, devBlock);
    mqtt.publish(topic, payload, true);

    // --- Sensor: IP адрес _mqtt (атрибут ip из /status) ---
    snprintf(topic, sizeof(topic), "homeassistant/sensor/meshcore_%s/ip/config", DEVICE_NAME);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s IP\","
        "\"state_topic\":\"%s/status\","
        "\"value_template\":\"{{ value_json.ip }}\","
        "\"unique_id\":\"meshcore_%s_ip\","
        "\"icon\":\"mdi:ip-network\","
        "\"device\":{%s}}",
        DEVICE_NAME, mqttPrefix, DEVICE_NAME, devBlock);
    mqtt.publish(topic, payload, true);

    // --- Sensor: температура CPU (встроенный датчик ESP32-S3) ---
    snprintf(topic, sizeof(topic), "homeassistant/sensor/meshcore_%s/temp/config", DEVICE_NAME);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s Temp\","
        "\"state_topic\":\"%s/status\","
        "\"value_template\":\"{{ value_json.temp | float }}\","
        "\"unit_of_measurement\":\"°C\","
        "\"unique_id\":\"meshcore_%s_temp\","
        "\"icon\":\"mdi:thermometer\","
        "\"device\":{%s}}",
        DEVICE_NAME, mqttPrefix, DEVICE_NAME, devBlock);
    mqtt.publish(topic, payload, true);

    // --- Sensor: last_msg выбранного канала (для триггеров) ---
    snprintf(topic, sizeof(topic), "homeassistant/sensor/meshcore_%s/lastmsg/config", DEVICE_NAME);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s LastMsg\","
        "\"state_topic\":\"%s/lastmsg\","
        "\"force_update\":true,"
        "\"unique_id\":\"meshcore_%s_lastmsg\","
        "\"icon\":\"mdi:message-arrow-right\","
        "\"device\":{%s}}",
        DEVICE_NAME, mqttPrefix, DEVICE_NAME, devBlock);
    mqtt.publish(topic, payload, true);

    // --- Text: отправка сообщения ---
    snprintf(topic, sizeof(topic), "homeassistant/text/meshcore_%s/send/config", DEVICE_NAME);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s Send\","
        "\"command_topic\":\"%s/cmd/send\","
        "\"unique_id\":\"meshcore_%s_send\","
        "\"icon\":\"mdi:message-text\","
        "\"device\":{%s}}",
        DEVICE_NAME, mqttPrefix, DEVICE_NAME, devBlock);
    mqtt.publish(topic, payload, true);

    // --- Switch: listening ---
    snprintf(topic, sizeof(topic), "homeassistant/switch/meshcore_%s/listening/config", DEVICE_NAME);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s Listening\","
        "\"command_topic\":\"%s/cmd/listening\","
        "\"state_topic\":\"%s/lstate\","
        "\"unique_id\":\"meshcore_%s_sw\","
        "\"icon\":\"mdi:radio\","
        "\"device\":{%s}}",
        DEVICE_NAME, mqttPrefix, mqttPrefix, DEVICE_NAME, devBlock);
    mqtt.publish(topic, payload, true);

    Serial.printf("[MQTT] discovery published for <%s>\n", DEVICE_NAME);

    // Удаляем старый сенсор "Message" (переименован в LastSender): пустой
    // retained в его discovery-топике убирает сущность из HA. Только один раз,
    // при первой публикации discovery.
    if (!discoveryPublished) {
        char oldTopic[128];
        snprintf(oldTopic, sizeof(oldTopic),
                 "homeassistant/sensor/meshcore_%s/message/config", DEVICE_NAME);
        mqtt.publish(oldTopic, "", true);
    }
    discoveryPublished = true;
}

// ===== JSON-экранирование (для publishMessage/publishStatus) =====
// Экранирует ", \ и управляющие символы, чтобы имена/тексты не ломали JSON.
void jsonEscape(const char* in, char* out, size_t outlen) {
    size_t n = 0;
    for (size_t i = 0; in[i] != 0 && n + 3 < outlen; i++) {
        char c = in[i];
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = c; }
        else if (c == '\n') { out[n++] = '\\'; out[n++] = 'n'; }
        else if (c == '\r') { out[n++] = '\\'; out[n++] = 'r'; }
        else if (c == '\t') { out[n++] = '\\'; out[n++] = 't'; }
        else if ((uint8_t)c < 0x20) { /* остальные управляющие пропускаем */ }
        else out[n++] = c;
    }
    out[n] = 0;
}

void publishMessage() {
    if (!mqttConnected) return;
    char escSender[64], escText[128], escChan[64], escRoute[128];
    jsonEscape(lastSender.c_str(), escSender, sizeof(escSender));
    jsonEscape(lastMessage.c_str(), escText, sizeof(escText));
    jsonEscape(lastChannelName.c_str(), escChan, sizeof(escChan));
    jsonEscape(lastPath[0] ? lastPath : "direct", escRoute, sizeof(escRoute));
    char topic[96], payload[384];
    snprintf(topic, sizeof(topic), "%s/state", mqttPrefix);
    snprintf(payload, sizeof(payload),
        "{\"sender\":\"%s\",\"text\":\"%s\",\"channel\":\"%s\","
        "\"rssi\":%.1f,\"snr\":%.1f,\"hops\":%d,\"route\":\"%s\"}",
        escSender, escText, escChan,
        lastRSSI, lastSNR, lastHopCount, escRoute);
    mqtt.publish(topic, payload);
    Serial.printf("[MQTT] message published\n");

    // last_msg: публикуется только для ВЫБРАННОГО в HA канала (для триггеров).
    // Значение меняется только когда пришло сообщение с канала mqttTxChannel.
    // Спустя LASTMSG_RESET_MS после публикации обнуляется: HA-триггер на
    // одинаковый текст срабатывает снова (состояние менялось /gate -> "" -> /gate).
    if (lastChannelIdx == mqttTxChannel && lastMessage.length() > 0) {
        char lmTopic[96];
        snprintf(lmTopic, sizeof(lmTopic), "%s/lastmsg", mqttPrefix);
        mqtt.publish(lmTopic, lastMessage.c_str(), true);
        lastmsgClearAt = millis() + LASTMSG_RESET_MS;
        lastmsgPendingClear = true;
        Serial.printf("[MQTT] lastmsg published (ch %s)\n", channels[mqttTxChannel].name);
    }
}

// Обнуляет lastmsg (если прошёл срок после последней публикации).
void clearLastMsg() {
    if (!lastmsgPendingClear || !mqttConnected) return;
    if ((int32_t)(millis() - lastmsgClearAt) < 0) return;   // ещё не время (учёт wrap)
    lastmsgPendingClear = false;
    char lmTopic[96];
    snprintf(lmTopic, sizeof(lmTopic), "%s/lastmsg", mqttPrefix);
    mqtt.publish(lmTopic, "", true);
    Serial.println("[MQTT] lastmsg cleared");
}

// Обнуляет text-топик сенсора после триггера "button":
// следующее нажатие снова вызывает state_changed → повторная автоматизация.
void clearSensorBtnText() {
    if (!snsBtnPendingClear || !mqttConnected) return;
    if ((int32_t)(millis() - snsBtnClearAt) < 0) return;
    snsBtnPendingClear = false;
    if (snsBtnSlug[0] == 0) return;
    char tText[128];
    snprintf(tText, sizeof(tText), "%s/sensor/%s/text", mqttPrefix, snsBtnSlug);
    mqtt.publish(tText, "", false);
    Serial.printf("[SNS] button text cleared for %s\n", snsBtnSlug);
    snsBtnSlug[0] = 0;
}

// Имя отправителя -> безопасный фрагмент топика/идентификатора.
void mqttSlug(const char* name, char* out, int maxLen) {
    int n = 0;
    for (int i = 0; name[i] && n < maxLen - 1; i++) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') out[n++] = c;
        else out[n++] = '_';
    }
    out[n] = 0;
}

// Публикует discovery устройству-датчику (отправитель из сенсорного канала).
void publishSensorDisc(const String& sender, const char* slug) {
    if (!mqttConnected) return;
    char devBlock[160];
    snprintf(devBlock, sizeof(devBlock),
        "\"identifiers\":[\"meshcore_sensor_%s\"],\"name\":\"MeshBot Sensor %s\","
        "\"manufacturer\":\"MeshCore\",\"model\":\"Sensor node\"",
        slug, slug);
    char topic[128], payload[512];
    // Данные: sensor с текстовым state (HA MQTT text требует command_topic,
    // а у нас сущность read-only — это state от сенсора).
    snprintf(topic, sizeof(topic), "homeassistant/sensor/meshcore_sensor_%s/text/config", slug);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s data\",\"state_topic\":\"%s/sensor/%s/text\","
        "\"icon\":\"mdi:sprout\",\"unique_id\":\"meshcore_sensor_%s_text\","
        "\"entity_category\":\"diagnostic\","
        "\"device\":{%s}}",
        sender.c_str(), mqttPrefix, slug, slug, devBlock);
    mqtt.publish(topic, payload, true);
    Serial.printf("[MQTT] sensor device discovery: %s\n", slug);

    char retTopic[128], retPayload[512];
    snprintf(retTopic, sizeof(retTopic), "homeassistant/sensor/meshcore_sensor_%s/rssi/config", slug);
    snprintf(retPayload, sizeof(retPayload),
        "{\"name\":\"%s RSSI\",\"state_topic\":\"%s/sensor/%s/rssi\","
        "\"unit_of_measurement\":\"dBm\",\"device_class\":\"signal_strength\","
        "\"unique_id\":\"meshcore_sensor_%s_rssi\","
        "\"device\":{%s}}",
        sender.c_str(), mqttPrefix, slug, slug, devBlock);
    mqtt.publish(retTopic, retPayload, true);
    Serial.printf("[MQTT] sensor rssi discovery: %s\n", slug);

    // Availability: бинарник device_class=connectivity, «online» пока датчик шлёт.
    char alvTopic[128], alvPayload[512];
    snprintf(alvTopic, sizeof(alvTopic),
             "homeassistant/binary_sensor/meshcore_sensor_%s/available/config", slug);
    snprintf(alvPayload, sizeof(alvPayload),
        "{\"name\":\"%s available\",\"state_topic\":\"%s/sensor/%s/available\","
        "\"payload_on\":\"online\",\"payload_off\":\"offline\",\"device_class\":\"connectivity\","
        "\"unique_id\":\"meshcore_sensor_%s_available\","
        "\"device\":{%s}}",
        sender.c_str(), mqttPrefix, slug, slug, devBlock);
    mqtt.publish(alvTopic, alvPayload, true);
    Serial.printf("[MQTT] sensor availability discovery: %s\n", slug);

    // Event entity: MQTT event platform — появляется как device-trigger "Fired"
    // в автоматизациях HA.  При нажатии кнопки сенсор шлёт "button", бот
    // публикует payload "button" в event_type_topic, HA генерирует событие.
    char evtTopic[128], evtPayload[512];
    snprintf(evtTopic, sizeof(evtTopic),
             "homeassistant/event/meshcore_sensor_%s/button/config", slug);
    snprintf(evtPayload, sizeof(evtPayload),
        "{\"name\":\"%s Button\","
        "\"state_topic\":\"%s/sensor/%s/button\","
        "\"event_types\":[\"button\",\"button2\"],"
        "\"unique_id\":\"meshcore_sensor_%s_button\","
        "\"device\":{%s}}",
        sender.c_str(), mqttPrefix, slug, slug, devBlock);
    mqtt.publish(evtTopic, evtPayload, true);
    Serial.printf("[MQTT] sensor button event discovery: %s\n", slug);
}

// Публикует сообщение из сенсорного канала: устройство HA на каждого отправителя.
// hello = heartbeat (только availability, без text/rssi).
// button = триггер: text + rssi + availability + event entity (для автоматизаций).
// Возвращает true, если сообщение реально отправлено в MQTT.
bool publishSensorMessage() {
    #ifdef MQTT_ENABLED
    if (!mqttConnected) {
        Serial.printf("[SNS] %s: %s (MQTT not connected, skipped)\n",
                      lastSender.c_str(), lastMessage.c_str());
        return false;
    }
    char slug[48];
    mqttSlug(lastSender.c_str(), slug, sizeof(slug));
    if (strlen(slug) == 0) snprintf(slug, sizeof(slug), "unknown");
    // discovery публикуется один раз на отправителя (кэш имён).
    bool known = false;
    int discIdx = -1;
    for (int i = 0; i < sensorDeviceDiscCount; i++) {
        if (sensorDeviceDisc[i] == lastSender) { known = true; discIdx = i; break; }
    }
    if (!known) {
        publishSensorDisc(lastSender, slug);
        if (mqttConnected && sensorDeviceDiscCount < SENSOR_DEV_CACHE_MAX) {
            discIdx = sensorDeviceDiscCount;
            sensorDeviceDisc[sensorDeviceDiscCount++] = lastSender;
        } else {
            discIdx = -1;
        }
    }
    // availability: любой приём от датчика = online.
    if (discIdx >= 0) {
        sensorLastActive[discIdx] = millis();
        if (!sensorOnlineNow[discIdx]) {
            sensorOnlineNow[discIdx] = true;
            char tAvail[128];
            snprintf(tAvail, sizeof(tAvail), "%s/sensor/%s/available", mqttPrefix, slug);
            mqtt.publish(tAvail, "online", true);
            Serial.printf("[SNS] %s AVAILABLE (online)\n", lastSender.c_str());
        }
    }
    // --- heartbeat: только availability, без text/rssi ---
    if (lastMessage == SENSOR_MSG_HELLO) {
        Serial.printf("[SNS] heartbeat from %s\n", lastSender.c_str());
        return true;
    }
    // --- данные: text + rssi ---
    char escText[128];
    jsonEscape(lastMessage.c_str(), escText, sizeof(escText));
    char tText[128], tRssi[128];
    snprintf(tText, sizeof(tText), "%s/sensor/%s/text", mqttPrefix, slug);
    snprintf(tRssi, sizeof(tRssi), "%s/sensor/%s/rssi", mqttPrefix, slug);
    mqtt.publish(tText, escText);
    char rssiStr[24];
    snprintf(rssiStr, sizeof(rssiStr), "%.1f", lastRSSI);
    mqtt.publish(tRssi, rssiStr);
    // --- button: event entity trigger для автоматизаций ---
    if ((lastMessage == SENSOR_MSG_BUTTON) || (lastMessage == SENSOR_MSG_BUTTON2)) {
        char tEvt[128];
        snprintf(tEvt, sizeof(tEvt), "%s/sensor/%s/button", mqttPrefix, slug);
        // MQTT event entity ожидает JSON с "event_type" (docs event.mqtt),
        // ретранслированные retained-сообщения отбрасываются.
        char evtJson[64];
        snprintf(evtJson, sizeof(evtJson), "{\"event_type\":\"%s\"}", lastMessage.c_str());
        mqtt.publish(tEvt, evtJson, false);
        // auto-clear: через SNS_BTN_CLEAR_MS текст сбрасывается "",
        // чтобы следующее нажатие снова вызвало "state_changed" в HA.
        snsBtnClearAt = millis() + SNS_BTN_CLEAR_MS;
        snsBtnPendingClear = true;
        strlcpy(snsBtnSlug, slug, sizeof(snsBtnSlug));
        Serial.printf("[SNS] %s from %s — trigger published\n", lastMessage.c_str(), lastSender.c_str());
    } else {
        Serial.printf("[SNS] %s: %s (rssi %.1f)\n", lastSender.c_str(), lastMessage.c_str(), lastRSSI);
    }
    return true;
    #else
    Serial.printf("[SNS] %s: %s (MQTT disabled)\n", lastSender.c_str(), lastMessage.c_str());
    return false;
    #endif
}

void publishStatus() {
    if (!mqttConnected) return;
    // Экранируем кавычки в именах/сообщениях для JSON
    char prvEsc[64];
    jsonEscape(privateChannelName.c_str(), prvEsc, sizeof(prvEsc));
    char chEsc[64];
    const char* chName = (mqttTxChannel >= 0 && mqttTxChannel < numChannels)
                         ? channels[mqttTxChannel].name : "?";
    jsonEscape(chName, chEsc, sizeof(chEsc));
    char topic[96], payload[320];
    unsigned long upSec = millis() / 1000;
    float tempC = cpuTempC();
    snprintf(topic, sizeof(topic), "%s/status", mqttPrefix);
    snprintf(payload, sizeof(payload),
        "{\"wifi\":true,\"mqtt\":true,\"lora_rx\":%s,"
        "\"uptime\":%lu,\"packets\":%d,\"duplicates\":%lu,"
        "\"temp\":%.1f,\"ip\":\"%s\","
        "\"channel\":\"%s\",\"private\":\"%s\"}",
        isListening ? "true" : "false",
        upSec, packetCount, duplicateCount,
        tempC,
        wifiConnected ? WiFi.localIP().toString().c_str() : "0.0.0.0",
        chEsc, prvEsc);
    mqtt.publish(topic, payload, true);

    // Listening state
    char lTopic[96];
    snprintf(lTopic, sizeof(lTopic), "%s/lstate", mqttPrefix);
    mqtt.publish(lTopic, isListening ? "ON" : "OFF", true);
}

// Только конфигурация MQTT (префиксы, сервер, коллбэк, буферы) — без блокирующего
// connect. Соединение настраивается фоном в tickRetryConnections() из loop.
void setupMQTT() {
    // Собираем префиксы топиков
    snprintf(mqttPrefix, sizeof(mqttPrefix), "meshcore/bot/%s", DEVICE_NAME);
    snprintf(mqttDiscoveryPrefix, sizeof(mqttDiscoveryPrefix), "homeassistant");

    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    mqtt.setBufferSize(768);   // discovery-селект (options) весит до ~600 Б
    mqtt.setSocketTimeout(3);  // ограничиваем блокировку connect() до ~3 с
}

// Неблокирующая машина состояния: WiFi -> MQTT. Вызывается раз в 5 с из loop().
// Никогда не блокирует приём радио дольше, чем сам mqtt.connect() (<=3 с).
void tickRetryConnections() {
    // ===== WiFi =====
    bool connectedNow = (WiFi.status() == WL_CONNECTED);
    if (wifiConnected && !connectedNow) {
        // обрыв — сбрасываем флаги, дальше переподключаемся
        wifiConnected = false;
        mqttConnected = false;
    }
    if (!connectedNow) {
        if (!wifiConnInProgress) {
            wifiConnInProgress = true;
            wifiConnStartMs = millis();
            Serial.println("[WiFi] connecting...");
            WiFi.disconnect();
            WiFi.begin(WIFI_SSID, WIFI_PASS);
        } else if ((int32_t)(millis() - wifiConnStartMs) > 15000) {
            Serial.println("[WiFi] timeout, retry in 5s");
            wifiConnInProgress = false;
        }
        return;   // без WiFi MQTT не трогаем
    }
    if (!wifiConnected) {
        wifiConnected = true;
        wifiConnInProgress = false;
        Serial.printf("[WiFi] connected (%s)\n", WiFi.localIP().toString().c_str());
        // SNTP сразу при появлении WiFi (не ждём MQTT): часы уточняются с
        // сервера времени, build-time устаревает уже через пару дней.
        if (!ntpStarted) {
            ntpStarted = true;
            lastNtpSyncMs = millis();
            configTime(0, 0, "pool.ntp.org");   // синхронизация в UTC
        }
    }

    // ===== MQTT =====
    if (mqttConnected && mqtt.connected()) return;
    if (mqttConnected) mqttConnected = false;

    Serial.printf("[MQTT] connecting to %s:%d ...", MQTT_BROKER, MQTT_PORT);
    char clientId[48];
    snprintf(clientId, sizeof(clientId), "meshcore_%s_%lu", DEVICE_NAME, millis() % 100000);

    if (mqtt.connect(clientId, MQTT_USER, MQTT_PASS)) {
        mqttConnected = true;
        Serial.println(" OK");
        char cmdTopic[96];
        snprintf(cmdTopic, sizeof(cmdTopic), "%s/cmd/send", mqttPrefix);
        mqtt.subscribe(cmdTopic);
        snprintf(cmdTopic, sizeof(cmdTopic), "%s/cmd/listening", mqttPrefix);
        mqtt.subscribe(cmdTopic);

        // Discovery публикуем только при ПЕРВОМ подключении (reconnect его
        // повторяет поток retained-конфигов). Обновления — по факту изменений.
        if (!discoveryPublished) publishDiscovery();
        publishStatus();
    } else {
        mqttConnected = false;
        Serial.printf(" FAILED (rc=%d)\n", mqtt.state());
    }
}

#endif // MQTT_ENABLED

// Список каналов через " + " для начального экрана/лога
String channelListStr() {
    String s = "";
    for (int i = 0; i < numChannels; i++) {
        if (i > 0) s += " + ";
        s += channels[i].name;
    }
    return s;
}

// Idle-экран состояния (часы, WiFi/MQTT, TX-канал, uptime, температура, счётчики).
void drawIdleStatus() {
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    #ifdef MQTT_ENABLED
    // WiFi/MQTT статус: 1 строка, "+" = подключено, "-" = нет
    const char* w = wifiConnected ? "+" : "-";
    const char* m = mqttConnected ? "+" : "-";
    display.printf("WiFi:%s MQTT:%s\n", w, m);
    if (mqttTxChannel >= 0 && mqttTxChannel < numChannels) {
        display.printf("TX: %s\n", channels[mqttTxChannel].name);
    }
    #endif
    // часы из системного времени (обновляются каждые 500 мс вместе с экраном)
    time_t now = time(NULL) + (time_t)TZ_OFFSET_HOURS * 3600;
    struct tm tm_now;
    gmtime_r(&now, &tm_now);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S  %d.%m", &tm_now);
    display.println(tbuf);
    // uptime + температура CPU (встроенный датчик ESP32-S3)
    unsigned long up = millis() / 1000;
    int t = (int)cpuTempC();
    display.printf("Up:%luh%02lum T:%dC\n", up / 3600, (up % 3600) / 60, t);
    display.printf("Pkts: %d\n", packetCount);
    if (lastMessage.length() > 0) {
        display.printf("Last: %s\n", lastMessage.substring(0, 20).c_str());
    }
    display.display();
}

// ===== MESH OTA: передача прошивки на сенсор (бот, ACK-режим) =====
// Хранит .bin в LittleFS (/ota.bin), шлёт чанки по сенсорному каналу.
// Каждый чанк: 80 байт (160 hex-символов) + CRC16; целостность файла — CRC32.
// Сенсор шлёт ota:ack/<seq>, при потере бот повторяет чанк по таймауту.
#ifdef MQTT_ENABLED

// Одиночная отправка группового сообщения в сенсорный канал.
void otaTxGroup(const String& msg) {
    if (sensorChannelIdx < 0) return;
    uint8_t frame[300];
    int f = buildGroupFrameFlood(sensorChannelIdx, msg, frame, sizeof(frame), NULL, 0);
    if (f > 0) {
        sendFrame(sensorChannelIdx, frame, f);
        otaSince = millis();
    }
}

void otaBotAbort(const char* why) {
    // сообщаем сенсору, чтобы он сразу откатился на прежнюю прошивку
    // (не дожидаясь сторожевого времени 60 с)
    if (otaTarget.length() > 0) {
        slog("[OTA] abort (%s) -> ota:abort %s\n", why, otaTarget.c_str());
        otaTxGroup("ota:abort");
    }
    radioSetNormalConfig();
    otaFastMode = false;
    otaPhase = OTA_PHASE_IDLE;
    otaTarget = "";
    if (otaFile) otaFile.close();
    #if HAS_OLED
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("OTA abort");
    display.println(why);
    display.display();
    #endif
}

void otaDrawProgress() {
    #if (HAS_OLED != 0)
    // Полная перерисовка OLED (I2C) стоит ~25 мс — на каждые OTA_DRAW_MS хватает
    // одного кадра; иначе прошивка замедляется на минуты из-за экрана.
    static uint32_t lastDraw = 0;
    if (millis() - lastDraw < OTA_DRAW_MS) return;
    lastDraw = millis();
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.printf("OTA %s\n", otaTarget.c_str());
    uint32_t pct = otaFwSize ? (otaSentBytes * 100 / otaFwSize) : 0;
    if (pct > 100) pct = 100;
    display.printf("%u%%\n", (unsigned)pct);
    int bw = otaFwSize ? (int)((long)otaSentBytes * 128 / otaFwSize) : 0;
    if (bw > 128) bw = 128;
    display.drawRect(0, 28, 128, 10, SSD1306_WHITE);
    display.fillRect(0, 28, bw, 10, SSD1306_WHITE);
    display.setCursor(0, 44);
    display.printf("Pkts: %d", packetCount);
    display.display();
    #endif
}

void otaSendStart() {
    char msg[96];
    snprintf(msg, sizeof(msg), "ota:start:%s:%u:%08X", otaTarget.c_str(),
             (unsigned)otaFwSize, (unsigned)otaFwCrc);
    slog("[OTA] -> %s: %s\n", otaTarget.c_str(), msg);
    otaTxGroup(msg);
}

void otaSendChunk() {
    if (!otaFile) { otaBotAbort("no file"); return; }
    uint32_t off = otaSeq * OTA_CHUNK_BYTES;
    if (off >= otaFwSize) return;
    int n = min((int)OTA_CHUNK_BYTES, (int)(otaFwSize - off));
    uint8_t chunk[OTA_CHUNK_BYTES];
    otaFile.seek(off);
    if (otaFile.read(chunk, n) != n) { otaBotAbort("read err"); return; }

    char hex[OTA_CHUNK_HEX + 1];
    bytesToHex(chunk, n, hex);
    char crcHex[5];
    snprintf(crcHex, sizeof(crcHex), "%04X", crc16buf(chunk, n));
    String msg = "ota:data:" + String(otaSeq) + ":" + crcHex + ":" + hex;
    uint32_t sentPct = (uint64_t)otaSentBytes * 100 / otaFwSize;
    unsigned long gap = millis() - otaSince;   // мс с момента прошлой отправки (RTT чанка)
    Serial.printf("[OTA] seq=%u %u%% retr=%u gap=%lums\n",
                  (unsigned)otaSeq, (unsigned)sentPct, otaRetries, gap);
    otaTxGroup(msg);
}

void otaSendEnd() {
    slog("[OTA] -> ota:end\n");
    otaTxGroup("ota:end");
}

// ACK/NACK от сенсора сенсорного канала. Вызывается из parseMeshCorePacket.
void otaHandleAck() {
    if (lastSender != otaTarget) return;   // ответ не целивого сенсора
    String m = lastMessage;

    if (m == "ota:ackstart") {
        if (otaPhase != OTA_PHASE_WAIT_START) return;
        otaPhase = OTA_PHASE_DATA;
        otaSeq = 0;
        otaSentBytes = 0;
        otaRetries = 0;
        slog("[OTA] ackstart -> быстрый конфиг (%.1f MHz SF%d), пауза %dms\n",
             OTA_FAST_FREQ, OTA_FAST_SF, OTA_FAST_SETTLE_MS);
        radioSetFastConfig();
        otaFastMode = true;
        delay(OTA_FAST_SETTLE_MS);
        otaSendChunk();
        otaDrawProgress();
        return;
    }
    if (m.startsWith("ota:nack:")) {
        if (otaPhase != OTA_PHASE_DATA) return;
        uint32_t s = strtoul(m.c_str() + 9, NULL, 10);
        if (s < otaSeq) return;   // устаревший (повторный nack старого чанка)
        if (s > otaSeq) {
            // Потерянные ack: сенсор уже принял чанки до s-1 и просит s.
            // Прыгаем вперёд вместо бесконечного повтора seq.
            uint32_t totalChunks = (otaFwSize + OTA_CHUNK_BYTES - 1) / OTA_CHUNK_BYTES;
            if (s > totalChunks) return;       // мусор/мусор от гранки
            uint32_t off = s * OTA_CHUNK_BYTES;
            otaSentBytes = min(otaFwSize, off);
            otaSeq = s;
            otaRetries = 0;
            slog("[OTA] nack=%u (ack потерялся) -> прыжок на seq=%u\n", s, s);
            otaDrawProgress();
            if (otaSentBytes >= otaFwSize) {
                otaPhase = OTA_PHASE_WAIT_END;
                slog("[OTA] все байты подтверждены, ожидаем финал\n");
                otaSendEnd();
            } else {
                otaSendChunk();
            }
            return;
        }
        // s == otaSeq — настоящее nack текущего чанка
        otaRetries++;
        if (otaRetries > OTA_MAX_RETRIES) { otaBotAbort("nack"); return; }
        otaSendChunk();
        return;
    }
    if (m.startsWith("ota:ack:")) {
        if (otaPhase != OTA_PHASE_DATA) return;
        uint32_t s = strtoul(m.c_str() + 8, NULL, 10);
        if (s != otaSeq) return;            // устаревший/повторный ACK
        otaRetries = 0;
        uint32_t off = otaSeq * OTA_CHUNK_BYTES;
        int n = min((int)OTA_CHUNK_BYTES, (int)(otaFwSize - off));
        otaSentBytes += n;
        otaSeq++;
        otaDrawProgress();
        if (otaSentBytes >= otaFwSize) {
            otaPhase = OTA_PHASE_WAIT_END;
            slog("[OTA] все байты подтверждены, ожидаем финал\n");
            otaSendEnd();
        } else {
            otaSendChunk();
        }
        return;
    }
    if (m == "ota:ackend") {
        if (otaPhase != OTA_PHASE_WAIT_END) return;
        otaPhase = OTA_PHASE_DONE;
        slog("[OTA] DONE: сенсор %s применил прошивку, CRC32 OK\n", otaTarget.c_str());
        #if (HAS_OLED != 0)
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("OTA OK");
        display.println(otaTarget);
        display.println("reboot sensor");
        display.display();
        #endif
        if (otaFile) otaFile.close();
        radioSetNormalConfig();
        otaFastMode = false;
        return;
    }
    if (m == "ota:nackcrc") {
        if (otaPhase == OTA_PHASE_WAIT_END) otaBotAbort("crc mismatch");
        return;
    }
    if (m == "ota:reboot") {
        slog("[OTA] сенсор перезагружается с новой прошивкой\n");
        return;
    }
}

// Неблокирующий таймаут-обработчик в loop().
void otaBotTick() {
    if (otaPhase != OTA_PHASE_WAIT_START &&
        otaPhase != OTA_PHASE_DATA &&
        otaPhase != OTA_PHASE_WAIT_END) return;
    if (millis() - otaSince < OTA_ACK_TIMEOUT_MS) return;

    otaRetries++;
    if (otaRetries > OTA_MAX_RETRIES) {
        char why[24];
        snprintf(why, sizeof(why), "timeout p%d", otaPhase);
        otaBotAbort(why);
        return;
    }
    if (otaPhase == OTA_PHASE_WAIT_START) otaSendStart();
    else if (otaPhase == OTA_PHASE_DATA) { slog("[OTA] resend seq=%u\n", (unsigned)otaSeq); otaSendChunk(); }
    else if (otaPhase == OTA_PHASE_WAIT_END) otaSendEnd();
    otaDrawProgress();
}
// === HTTP OTA и MESH OTA (порт 3232): обновление прошивки _mqtt / сенсоров ====
WebServer otaServer(3232);
bool otaFlashing = false;
unsigned long otaStartMs = 0;

// ===== УДАЛЁННАЯ ДИАГНОСТИКА (web-страница /logs) =====
// Хвост лога держим в String (~4 KB) — виден на странице узла даже без serial.
// logTail, а не кольцевой буфер: проще, а записи уже усекаются спереди.
String logTail;
#define LOG_TAIL_MAX 4000
// slog = Serial + кольцевой хвост для web. Используется в диагностических местах
// (LittleFS, mesh OTA, partition). Частые служебные выводы (каждый чанк) НЕ логируем.
void slog(const char* fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    Serial.write((const uint8_t*)tmp, n);
    if (logTail.length() + (size_t)n > LOG_TAIL_MAX) {
        size_t drop = logTail.length() + (size_t)n - LOG_TAIL_MAX;
        if (drop < logTail.length()) logTail.remove(0, drop);
        logTail = "…(обрезано)…" + logTail;
    }
    logTail += tmp;
}

// Живой отчёт для /logs: uptime, разделы, LiveFS probe, состояние mesh OTA, хвост лога.
String buildDiagReport() {
    String r;
    r.reserve(3072);
    r += "===== MESHCORE BOT DIAG =====\r\n";
    r += "uptime: " + String((unsigned long)(millis() / 1000)) + " s\r\n";
    r += "free heap: " + String(ESP.getFreeHeap()) + " B\r\n";
    r += "flash chip: " + String(ESP.getFlashChipSize()) + " B\r\n";
    r += "-- partitions (runtime) --\r\n";
    {
        esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY,
                                                         ESP_PARTITION_SUBTYPE_ANY, NULL);
        for (; it != NULL; it = esp_partition_next(it)) {
            const esp_partition_t* p = esp_partition_get(it);
            char line[96];
            snprintf(line, sizeof(line),
                     "  %-10s type=%d sub=%d off=0x%06X size=%uK\r\n",
                     p->label, (int)p->type, (int)p->subtype,
                     (unsigned)p->address, p->size / 1024);
            r += line;
        }
        if (it) esp_partition_iterator_release(it);
    }
    r += "-- LittleFS (live probe) --\r\n";
    {
        File t = LittleFS.open("/.probe", "w");
        if (!t) {
            r += "  open(w) FAILED — LittleFS не работает\r\n";
        } else {
            int w = (int)t.write((const uint8_t*)"probe", 5);
            t.close();
            if (w != 5) {
                r += "  write FAILED\r\n";
            } else {
                File t2 = LittleFS.open("/.probe", "r");
                if (!t2) {
                    r += "  open(r) FAILED\r\n";
                } else {
                    char b[8] = {0};
                    int rd = (int)t2.read((uint8_t*)b, 5);
                    t2.close();
                    r += "  read ok: \"" + String(b) + "\" (" + String(rd) + " B)\r\n";
                }
                LittleFS.remove("/.probe");
            }
        }
    }
    r += "-- mesh OTA state --\r\n";
    r += "  otaFwReady=" + String(otaFwReady ? "true" : "false");
    r += " otaFwSize=" + String(otaFwSize);
    r += " otaPhase=" + String(otaPhase);
    r += " otaSaveOk=" + String(otaSaveOk ? "true" : "false") + "\r\n";
    if (LittleFS.exists("/ota.bin")) {
        File f = LittleFS.open("/ota.bin", "r");
        r += "  /ota.bin exists, size=" + String(f ? (long)f.size() : -1L) + "\r\n";
        if (f) f.close();
    } else {
        r += "  /ota.bin НЕТ\r\n";
    }
    r += "  sensors known: " + String(sensorDeviceDiscCount) + "\r\n";
    r += "  sensors online: ";
    int on = 0;
    for (int i = 0; i < sensorDeviceDiscCount; i++) if (sensorOnlineNow[i]) on++;
    r += String(on) + "\r\n";
    r += "\r\n===== LOG TAIL =====\r\n";
    r += logTail;
    return r;
}

void otaHandleRoot() {
    static const char PAGE[] PROGMEM =
        "<!DOCTYPE html><html lang='ru'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>MeshBot OTA</title><style>"
        "body{font-family:system-ui,sans-serif;background:#1c2333;color:#e8ecf3;margin:0;min-height:100vh;"
        "display:flex;align-items:center;justify-content:center;padding:12px}"
        ".card{background:#242d40;border:1px solid #35405a;border-radius:10px;padding:14px;max-width:440px;width:100%}"
        "h2{margin:0;font-size:17px;display:inline}h4{margin:0 0 10px;color:#93a4c4;font-weight:400;font-size:13px}"
        ".row{display:flex;gap:8px;align-items:center}"
        "select{flex:1;min-width:0;padding:6px;border:1px solid #35405a;border-radius:6px;background:#1e2638;"
        "color:#e8ecf3;font-size:13px}"
        "label{font-size:11px;color:#93a4c4}"
        "#drop{border:2px dashed #4a5775;border-radius:8px;padding:12px;text-align:center;cursor:pointer;"
        "background:#1e2638;font-size:13px}"
        "#drop.hover{border-color:#5eead4;background:#1b2233}"
        "#drop .big{display:inline;font-size:18px;vertical-align:middle}"
        "#drop span{color:#93a4c4}#fname{display:block;margin-top:6px;color:#5eead4;word-break:break-all;font-size:12px}"
        "button{flex:1;padding:9px;border:0;border-radius:6px;background:#0ea5e9;color:#fff;font-size:14px;"
        "font-weight:600;cursor:pointer}"
        "button:hover{background:#38bdf8}button:disabled{background:#33415c;cursor:not-allowed}"
        "button.sec{background:#475569}button.sec:hover{background:#64748b}"
        "#go{display:block;width:100%;margin-top:10px}#secrow{display:flex;gap:8px;margin-top:8px}"
        "#bar{display:none;margin-top:10px}#bar .t{display:flex;justify-content:space-between;font-size:11px;color:#93a4c4}"
        "#bar .w{height:7px;background:#33415c;border-radius:5px;overflow:hidden;margin-top:4px}"
        "#bar .f{height:100%;width:0;background:#0ea5e9;transition:width .15s}"
        "#st{margin-top:8px;font-size:12px;min-height:15px}#st.err{color:#f87171}#st.ok{color:#5eead4}"
        "#logs{display:none;max-height:200px;overflow:auto;background:#1e2638;border:1px solid #35405a;"
        "border-radius:6px;padding:8px;margin-top:10px;font-size:10px;line-height:1.4;"
        "white-space:pre-wrap;word-break:break-all}"
        "</style></head><body>"
        "<div class='card'><div class='row'><h2>MeshBot</h2><h4>" DEVICE_NAME "</h4></div>"
        "<label>Куда прошиваем:</label><div class='row'><select id='target'>__OPTIONS__</select></div>"
        "<div id='drop'><span class='big'>&#128190;</span> <span id='hint'>"
        "Перетащи .bin сюда или нажми</span><span id='fname'></span></div>"
        "<input id='file' type='file' accept='.bin' style='display:none'>"
        "<button id='go' disabled>&#10133; Начать обновление</button>"
        "<div id='secrow'><button id='ab' class='sec' style='display:none'>&#10060; Прервать</button>"
        "<button class='sec' onclick='loadLogs()'>&#128220; Логи</button></div>"
        "<div id='bar'><div class='t'><span id='pc'>0%</span><span id='sz'></span></div>"
        "<div class='w'><div class='f'></div></div></div>"
        "<div id='st'></div>"
        "<pre id='logs'></pre></div>"
        "<script>var file=null,input=document.getElementById('file'),drop=document.getElementById('drop'),"
        "go=document.getElementById('go'),ab=document.getElementById('ab'),bar=document.getElementById('bar'),"
        "st=document.getElementById('st'),sel=document.getElementById('target'),poll=null;"
        "function setF(f){file=f;if(!f)return;document.getElementById('fname').textContent=f.name+' ('+(f.size/1024|0)+' KB)';"
        "document.getElementById('hint').style.display='none';go.disabled=false;}"
        "drop.onclick=function(){input.click()};drop.ondragover=function(e){e.preventDefault();drop.classList.add('hover')};"
        "drop.ondragleave=function(){drop.classList.remove('hover')};drop.ondrop=function(e){e.preventDefault();"
        "drop.classList.remove('hover');if(e.dataTransfer.files[0])setF(e.dataTransfer.files[0])};"
        "input.onchange=function(){setF(input.files[0])};"
        "function setP(p,s){document.getElementById('pc').textContent=p+'%';document.getElementById('sz').textContent=s;"
        "document.getElementById('bar').firstElementChild.nextElementSibling.firstElementChild.style.width=p+'%';}"
        "function loadLogs(){var x=new XMLHttpRequest();x.open('GET','/logs');"
        "x.onload=function(){document.getElementById('logs').textContent=x.response;"
        "document.getElementById('logs').style.display='block';};x.send()}"
        "function pollStatus(){var x=new XMLHttpRequest();x.open('GET','/ota/status');x.onload=function(){"
        "if(x.status!=200){stopPoll();return;}var j;try{j=JSON.parse(x.response)}catch(e){return;}st.className='ok';"
        "if(j.phase==4){bar.style.display='block';setP(100,j.msg);st.textContent=j.msg;stopPoll();ab.style.display='none';return;}"
        "if(j.phase==0){stopPoll();if(!j.fw)st.textContent=j.msg;return;}"
        "bar.style.display='block';setP(j.pct,j.msg);st.textContent=(j.retr>0)?('retyr '+j.retr):''};"
        "x.onerror=stopPoll;x.send()}"
        "function stopPoll(){if(poll){clearInterval(poll);poll=null}}"
        "ab.onclick=function(){var x=new XMLHttpRequest();x.open('POST','/ota/abort');x.onload=function(){stopPoll();"
        "st.className='err';st.textContent='Прервано';ab.style.display='none'};x.send()};"
        "go.onclick=function(){if(!file)return;go.disabled=true;st.className='';st.textContent='Загрузка на бот...';"
        "bar.style.display='block';var selfMode=sel.value=='__self__';var fd=new FormData();fd.append('fw',file);"
        "var x=new XMLHttpRequest();x.open('POST',selfMode?'/update':'/savefw');"
        "x.upload.onprogress=function(e){if(e.lengthComputable){var p=e.loaded/e.total*100|0;setP(p,'upload')}};"
        "x.onload=function(){if(!selfMode){if(x.response.indexOf('OK')<0){st.className='err';st.textContent=x.response||'FAIL';go.disabled=false;return;}"
        "var s=new XMLHttpRequest();s.open('POST','/ota/start?target='+encodeURIComponent(sel.value));"
        "s.onload=function(){if(s.status!=200){st.className='err';st.textContent=s.response;go.disabled=false;return;}"
        "st.className='ok';st.textContent='Запущено, ждём сенсор...';ab.style.display='block';"
        "poll=setInterval(pollStatus,2000)};s.onerror=function(){st.className='err';st.textContent='no start';go.disabled=false};s.send()}"
        "else{st.className=x.response.indexOf('OK')>=0?'ok':'err';st.textContent=x.response;"
        "if(st.className=='ok')setTimeout(function(){location.reload()},3000)}};x.send(fd)};</script>"
        "</body></html>";

    String page = FPSTR(PAGE);
    String opts;
    opts += "<option value='__self__'>" DEVICE_NAME " (этот бот, HTTP)</option>";
    for (int i = 0; i < sensorDeviceDiscCount; i++) {
        String n = sensorDeviceDisc[i];
        n.replace("'", "");
        opts += "<option value='" + n + "'>" + n + " (mesh OTA)</option>";
    }
    page.replace("__OPTIONS__", opts);

    otaServer.sendHeader("Connection", "close");
    otaServer.send(200, "text/html", page);
    #if HAS_OLED
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("OTA page open");
    display.display();
    #endif
}

// multipart с нетипичным размером partition — работаем потоково через Update
void otaHandleUpdate() {
    HTTPUpload& up = otaServer.upload();
    switch (up.status) {
    case UPLOAD_FILE_START:
    {
        otaFlashing = true;
        otaStartMs = millis();
        Serial.printf("\n[OTA] загрузка: %s (%u байт)\n", up.filename.c_str(), (unsigned)up.totalSize);
        #if HAS_OLED
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("OTA update...");
        display.display();
        #endif
        // NAS: выключаем радио и сеть на время прошивки
        radio.sleep();
        isListening = false;
        if (mqttConnected) mqtt.disconnect();
        #if HAS_FEM
        digitalWrite(FEM_EN_PIN, LOW);
        #endif
        // totalSize на UPLOAD_FILE_START равен 0 в Arduino core (растёт только при
        // WRITE), поэтому размер берём из свободного места под прошивку.
        uint32_t otaMax = UPDATE_SIZE_UNKNOWN;
        if (!Update.begin(otaMax)) {
            Update.printError(Serial);
        }
        break;
    break;
    }
    case UPLOAD_FILE_WRITE:
    {
        if (Update.write(up.buf, up.currentSize) != up.currentSize) {
            Update.printError(Serial);
        }
        if ((millis() - otaStartMs) / 1000 > 3) {
            otaStartMs = millis();   // только для прогресса не критично
        }
        break;
    }
    case UPLOAD_FILE_END:
    {
        if (Update.end(true)) {
            Serial.printf("[OTA] OK, %u bytes, reboot...\n", (unsigned)up.totalSize);
            #if HAS_OLED
            display.println("OK! Rebooting...");
            display.display();
            #endif
            otaServer.send(200, "text/plain", "OK rebooting");
            delay(300);
            ESP.restart();
        } else {
            Update.printError(Serial);
            #if HAS_OLED
            display.println("OTA FAILED!");
            display.display();
            #endif
            otaFlashing = false;
            radio.startReceive();
            isListening = true;
        }
        break;
    }
    case UPLOAD_FILE_ABORTED:
    {
        Update.abort();
        otaFlashing = false;
        radio.startReceive();
        isListening = true;
        Serial.println("[OTA] прервано");
        break;
    }
    }
}

// Сохранение прошивки для mesh OTA в LittleFS (/ota.bin), без записи в себя.
unsigned long otaWriteCalls = 0;    // сколько раз вызвали WRITE
unsigned long otaWriteBytes = 0;    // сколько байт otaFile.write() подтвердил
unsigned long otaWriteSkipped = 0;  // WRITE-колбэков, где guard не прошёл
void otaHandleSaveFw() {
    HTTPUpload& up = otaServer.upload();
    switch (up.status) {
    case UPLOAD_FILE_START:
    {
        if (otaPhase != OTA_PHASE_IDLE && otaPhase != OTA_PHASE_DONE) otaBotAbort("новый .bin");
        otaSaving = true;
        otaSaveOk = false;
        otaWriteCalls = 0;
        otaWriteBytes = 0;
        otaWriteSkipped = 0;
        // закрываем любые остатки прошлой сессии, иначе на /ota.bin висит чужой handle
        if (otaFile) { otaFile.close(); otaFile = File(); }
        slog("\n[OTA-SAVE] %s (%u байт)\n", up.filename.c_str(), (unsigned)up.totalSize);
        otaFile = LittleFS.open("/ota.bin", "w");
        if (!otaFile) {
            slog("[OTA-SAVE] LittleFS.open FAILED\n");
        } else {
            slog("[OTA-SAVE] file opened OK\n");
        }
        break;
    }
    case UPLOAD_FILE_WRITE:
    {
        otaWriteCalls++;
        if (otaSaving && otaFile) {
            size_t w = otaFile.write(up.buf, up.currentSize);
            otaWriteBytes += w;
            if (w != up.currentSize) slog("[OTA-SAVE] write short (%u/%u)\n",
                                          (unsigned)w, (unsigned)up.currentSize);
        } else {
            otaWriteSkipped++;
            slog("[OTA-SAVE] WRITE skipped (saving=%d, file=%d)\n",
                 (int)otaSaving, (int)(bool)otaFile);
        }
        break;
    }
    case UPLOAD_FILE_END:
    {
        slog("[OTA-SAVE] END: saving=%d file=%d total=%u | writeCalls=%lu writeBytes=%lu skipped=%lu\n",
             (int)otaSaving, (int)(bool)otaFile, (unsigned)up.totalSize,
             otaWriteCalls, otaWriteBytes, otaWriteSkipped);
        if (otaSaving && otaFile) {
            // В ESP-IDF VFS fstat() НЕ видит буферизованные данные до fflush/close,
            // поэтому size() возвращает 0. Закрываем first → flush на диск → reopen для size.
            otaFile.close();
            otaFile = File();
            otaSaving = false;
            if (otaWriteBytes == 0) {
                otaFwSize = 0;
                otaFwReady = false;
                otaSaveOk = false;
                slog("[OTA-SAVE] FAIL — write вернул 0 байт\n");
            } else {
                // Пересоздаём для проверки размера (flush при close записал на диск)
                File f = LittleFS.open("/ota.bin", "r");
                uint32_t sz = f ? (uint32_t)f.size() : 0;
                if (f) f.close();
                slog("[OTA-SAVE] after close: size()=%u (wrote %lu)\n",
                     (unsigned)sz, otaWriteBytes);
                otaFwSize = sz;
                otaFwCrc = 0;
                otaFwReady = true;
                otaSaveOk = true;
                slog("[OTA-SAVE] OK, %u bytes\n", (unsigned)sz);
                if (sz != (uint32_t)otaWriteBytes) {
                    slog("[OTA-SAVE] ВНИМАНИЕ: size()=%u != writeBytes=%lu\n",
                         (unsigned)sz, otaWriteBytes);
                }
            }
        } else {
            otaSaving = false;
            otaFwReady = false;
            otaSaveOk = false;
            slog("[OTA-SAVE] FAIL — file not open\n");
        }
        break;
    }
    case UPLOAD_FILE_ABORTED:
    {
        otaSaving = false;
        otaSaveOk = false;
        if (otaFile) { otaFile.close(); otaFile = File(); };
        otaFwReady = false;
        slog("[OTA-SAVE] прервано\n");
        break;
    }
    }
}

// Запуск mesh OTA на сенсор: ?target=<имя сенсора> (bin уже в /ota.bin).
void otaHandleStartOta() {
    String target = otaServer.arg("target");
    if (otaPhase != OTA_PHASE_IDLE && otaPhase != OTA_PHASE_DONE) {
        slog("[WEB] /ota/start busy (phase=%d)\n", otaPhase);
        otaServer.send(409, "text/plain", "busy");
        return;
    }
    if (!otaFwReady || otaFwSize == 0) {
        slog("[WEB] /ota/start: нет .bin (ready=%d size=%u)\n",
             (int)otaFwReady, (unsigned)otaFwSize);
        otaServer.send(400, "text/plain", "нет .bin на боте");
        return;
    }
    if (target.length() == 0 || target.length() > 31) {
        slog("[WEB] /ota/start bad target: '%s'\n", target.c_str());
        otaServer.send(400, "text/plain", "bad target");
        return;
    }
    // считаем CRC32 по файлу (один раз перед сессией)
    otaFile = LittleFS.open("/ota.bin", "r");
    if (!otaFile) { slog("[WEB] /ota/start: fs open fail\n"); otaServer.send(500, "text/plain", "fs open fail"); return; }
    uint32_t crc = 0xFFFFFFFF;
    uint8_t tmp[256];
    while (otaFile.available()) {
        int nr = otaFile.read(tmp, sizeof(tmp));
        if (nr > 0) crc = crc32_upd(crc, tmp, nr);
    }
    otaFile.seek(0);
    otaFwCrc = ~crc;

    otaTarget = target;
    otaPhase = OTA_PHASE_WAIT_START;
    otaSeq = 0;
    otaSentBytes = 0;
    otaRetries = 0;
    slog("[WEB] /ota/start -> '%s' (%u байт, crc=%08X)\n",
         otaTarget.c_str(), (unsigned)otaFwSize, (unsigned)otaFwCrc);
    otaSendStart();
    otaDrawProgress();
    otaServer.send(200, "text/plain", "started");
}

void otaHandleAbort() {
    if (otaPhase != OTA_PHASE_IDLE) otaBotAbort("manual");
    otaServer.send(200, "text/plain", "aborted");
}

void otaHandleStatus() {
    char json[160];
    if (otaPhase == OTA_PHASE_IDLE) {
        snprintf(json, sizeof(json),
                 "{\"phase\":0,\"fw\":%s,\"msg\":\"%s\"}",
                 otaFwReady ? "true" : "false",
                 otaFwReady ? "готово, .bin на боте" : "нет прошивки на боте");
        otaServer.send(200, "application/json", json);
        return;
    }
    if (otaPhase == OTA_PHASE_DONE) {
        snprintf(json, sizeof(json),
                 "{\"phase\":4,\"pct\":100,\"msg\":\"готово: %s\",\"retr\":0}",
                 otaTarget.c_str());
        otaServer.send(200, "application/json", json);
        return;
    }
    uint32_t pct = otaFwSize ? (otaSentBytes * 100 / otaFwSize) : 0;
    if (pct > 100) pct = 100;
    const char* ph = "start";
    if (otaPhase == OTA_PHASE_DATA) ph = "data";
    else if (otaPhase == OTA_PHASE_WAIT_END) ph = "finalize";
    snprintf(json, sizeof(json),
             "{\"phase\":%u,\"pct\":%u,\"msg\":\"%s %u%% (%u/%u)\",\"retr\":%u}",
             otaPhase, pct, ph, pct,
             (unsigned)otaSentBytes, (unsigned)otaFwSize, otaRetries);
    otaServer.send(200, "application/json", json);
}

void setupOtaServer() {
    otaServer.on("/", HTTP_GET, otaHandleRoot);
    otaServer.on("/update", HTTP_POST, []() {
        otaServer.sendHeader("Connection", "close");
        otaServer.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    }, otaHandleUpdate);
    otaServer.on("/savefw", HTTP_POST, []() {
        otaServer.sendHeader("Connection", "close");
        if (otaSaveOk && otaFwSize > 0) {
            slog("[WEB] /savefw -> OK (%u байт)\n", (unsigned)otaFwSize);
            otaServer.send(200, "text/plain", "OK");
        } else {
            slog("[WEB] /savefw -> FAIL (saveOk=%d fwSize=%u)\n",
                 (int)otaSaveOk, (unsigned)otaFwSize);
            otaServer.send(200, "text/plain",
                           otaSaveOk ? "FAIL: файл пустой (LittleFS)" : "FAIL: файл не открылся (LittleFS)");
        }
    }, otaHandleSaveFw);
    otaServer.on("/ota/start", HTTP_POST, otaHandleStartOta);
    otaServer.on("/ota/abort", HTTP_POST, otaHandleAbort);
    otaServer.on("/ota/status", HTTP_GET, otaHandleStatus);
    otaServer.on("/logs", HTTP_GET, []() {
        otaServer.sendHeader("Connection", "close");
        otaServer.send(200, "text/plain", buildDiagReport());
    });
    otaServer.begin();
    Serial.println("OTA server: http://<ip>:3232/update | /ota/start");
}
#endif

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== MESHCORE LISTENER ===\n");
    
    initSystemClock();
    
    // ===== ПИТАНИЕ ПЕРИФЕРИИ (VEXT) =====
    #if HAS_OLED && defined(VEXT_PIN)
    pinMode(VEXT_PIN, OUTPUT);
    digitalWrite(VEXT_PIN, VEXT_EN_ACTIVE);
    delay(300);
    #endif
    
    // ===== FEM (усилитель KCT8103L на V4) =====
    #if HAS_FEM
    Serial.println("Init FEM...");
    pinMode(FEM_VCC_PIN, OUTPUT);
    pinMode(FEM_EN_PIN, OUTPUT);
    pinMode(FEM_TX_PIN, OUTPUT);
    digitalWrite(FEM_VCC_PIN, HIGH);
    delay(10);
    digitalWrite(FEM_EN_PIN, HIGH);
    delay(10);
    digitalWrite(FEM_TX_PIN, LOW);  // RX
    delay(10);
    Serial.println("FEM OK");
    #endif
    
    // ===== OLED =====
    #if HAS_OLED
    pinMode(OLED_RESET, OUTPUT);
    digitalWrite(OLED_RESET, LOW);
    delay(10);
    digitalWrite(OLED_RESET, HIGH);
    delay(200);

    #if defined(SDA_PIN) && defined(SCL_PIN)
    Wire.begin(SDA_PIN, SCL_PIN);
    #else
    Wire.begin();
    #endif
    Wire.setClock(100000);   // 400k часть OLED-панелей V4 "мусорит" — снижаем

    // Сканер I2C: некоторые экземпляры Heltec V4 живут на 0x3D, а не 0x3C.
    // Полоски/мусор на экране часто = неверный адрес или ранний инит.
    uint8_t oledAddr = SCREEN_ADDRESS;
    bool addrFound = false;
    for (uint8_t a = 0x03; a < 0x78; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            Serial.printf("[I2C] device found at 0x%02X\n", a);
            if (a == 0x3C || a == 0x3D) {
                oledAddr = a;
                addrFound = true;
            }
        }
    }
    if (!addrFound) {
        Serial.printf("[OLED] no 0x3C/0x3D found, defaulting to 0x%02X\n", oledAddr);
    } else {
        Serial.printf("[OLED] address = 0x%02X\n", oledAddr);
    }

    if (!display.begin(SSD1306_SWITCHCAPVCC, oledAddr)) {
        // пробуем альтернативный адрес
        uint8_t alt = (oledAddr == 0x3C) ? 0x3D : 0x3C;
        if (!display.begin(SSD1306_SWITCHCAPVCC, alt)) {
            Serial.println("Display FAILED!");
            while (1) delay(1000);
        } else {
            Serial.printf("OLED ok at 0x%02X (alt)\n", alt);
        }
    } else {
        Serial.printf("OLED ok at 0x%02X\n", oledAddr);
    }

    // Принудительно очищаем 2 раза и снимаем dim — уже фактически
    // устраняет "полоски" начального мусора на SSD1306
    display.dim(false);
    display.clearDisplay();
    display.display();
    delay(50);
    display.clearDisplay();
    display.display();
    
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("MeshCore");
    display.println("V4.3 init...");
    display.display();
    #endif
    
    #if BUTTON_PIN >= 0
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    #endif
    
    // ===== SPI =====
    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
    
    // ===== RESET =====
    pinMode(LORA_RST, OUTPUT);
    digitalWrite(LORA_RST, LOW);
    delay(20);
    digitalWrite(LORA_RST, HIGH);
    delay(100);
    
    // ===== LORA =====
    if (!initLoRa()) {
        #if HAS_OLED
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("LoRa ERROR!");
        display.display();
        #endif
        while (1) {
            delay(1000);
            Serial.println("LoRa init FAILED");
        }
    }
    
    deriveChannels();
    initAdvertIdentity();

#ifdef MQTT_ENABLED
    loadTxChannel();
    // Сеть НЕ блокируем: WiFi/MQTT поднимаются фоном в loop (tickRetryConnections).
    // Радио начинает слушать сразу, без задержки на TCP/а-коннект.
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);   // без modem-sleep: меньше задержка отвёта
    // Дамп РЕАЛЬНОЙ таблицы разделов с устройства — для отладки LittleFS.
    // Если spiffs нет/другой офсет — mesh OTA работать не будет.
    {
        slog("[PART] flash chip size: %u KB\n", ESP.getFlashChipSize() / 1024);
        esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY,
                                                         ESP_PARTITION_SUBTYPE_ANY, NULL);
        for (; it != NULL; it = esp_partition_next(it)) {
            const esp_partition_t* p = esp_partition_get(it);
            slog("[PART] %-10s type=%d sub=%d off=0x%06X size=%uK\n",
                 p->label, (int)p->type, (int)p->subtype,
                 (unsigned)p->address, p->size / 1024);
        }
        if (it) esp_partition_iterator_release(it);
    }
    // хранилище /ota.bin для mesh OTA сенсоров (partition "spiffs", 1.5 MB)
    if (LittleFS.begin(true)) {
        slog("[LITTLEFS] OK\n");
        // Самотест: write/read/remove — доказывает, что ФС реально работает.
        {
            File t = LittleFS.open("/.selftest", "w");
            if (!t) {
                slog("[LITTLEFS] self-test: open(w) FAILED\n");
            } else if (t.write((const uint8_t*)"meshcore", 8) != 8) {
                slog("[LITTLEFS] self-test: write FAILED\n");
                t.close();
            } else {
                t.close();
                File t2 = LittleFS.open("/.selftest", "r");
                if (!t2) {
                    slog("[LITTLEFS] self-test: open(r) FAILED\n");
                } else {
                    char buf[16] = {0};
                    int n = t2.read((uint8_t*)buf, sizeof(buf) - 1);
                    t2.close();
                    slog("[LITTLEFS] self-test: read %d bytes = \"%s\"\n",
                         n, (n > 0) ? buf : "(fail)");
                }
                LittleFS.remove("/.selftest");
            }
        }
    } else {
        slog("[LITTLEFS] FAILED (begin) — mesh OTA недоступен\n");
    }
    if (LittleFS.exists("/ota.bin")) {
        File f = LittleFS.open("/ota.bin", "r");
        otaFwSize = f ? (uint32_t)f.size() : 0;
        otaFwReady = (otaFwSize > 0);
        if (f) f.close();
        slog("[OTA] /ota.bin: %u байт (mesh OTA ready=%d)\n",
             (unsigned)otaFwSize, (int)otaFwReady);
    }
    setupMQTT();            // только конфиг (префиксы/сервер/коллбэк)
    setupOtaServer();       // HTTP OTA на :3232 (обновление прошивки по WiFi)
    #endif

    radio.startReceive();
    isListening = true;
    lastDirectAdvertMs = lastFloodAdvertMs = millis();
    
    #if HAS_OLED
    display.clearDisplay();
    // Стартовая заставка: только имя устройства по центру экрана (128x64).
    display.setTextSize(2);                     // 12x16 симв.
    const char* splash = "MeshCore";
    display.setCursor((128 - (int)strlen(splash) * 12) / 2, (64 - 16) / 2);
    display.print(splash);
    display.setTextSize(1);
    display.display();
    #endif
    
    Serial.printf("Listening on %s...\n", channelListStr().c_str());
    lastScreenActivityMs = millis();   // старт таймера автовыключения экрана
}

// Отправляет сообщение в сенсорный канал (3 flood-ретрая).
// Используется только SENSOR_NODE-сборкой. chIdx для sendFrame здесь
// не нужен: flood-кадры идут целиком, без return-path.
void sensorSendMsg(const char* msg) {
    if (sensorChannelIdx < 0) {
        Serial.printf("[SNS] sensor channel not configured, cannot send \"%s\"\n", msg);
        return;
    }
    uint8_t enc[256];
    int enclen = buildGroupEnc(sensorChannelIdx, msg, enc);
    if (enclen <= 0) return;
    uint8_t frame[300];
    int f = buildGroupFrameFlood(sensorChannelIdx, msg, frame, sizeof(frame), enc, enclen);
    if (f > 0) {
        floodSend3(-1, frame, f);
        Serial.printf("[SNS] sent \"%s\" to sensor channel\n", msg);
    }
}

#ifdef SENSOR_NODE
// ===== MESH OTA: сенсор (принимающая сторона) =====
// Одиночная отправка ответа в сенсорный канал (для ack/nack).
// Одиночная (или флуд) отправка OTA-ответа от сенсора в сенсорный канал.
// ack/nack чанков — ОДИН кадр с задержкой staggerMs: three-in-a-flood блокирует
// чтение RX-FIFO (~72 мс), и следующий чанк от бота затирается → таймаут бота.
// ackstart — flurry flood (штатный конфиг, бот не спешит до settle). ackend/reboot — single.
void otaSensorSend(const String& msg, bool flood = false, unsigned int staggerMs = 0) {
    if (sensorChannelIdx < 0) return;
    uint8_t enc[256];
    int enclen = buildGroupEnc(sensorChannelIdx, msg, enc);
    if (enclen <= 0) return;
    uint8_t frame[300];
    int f = buildGroupFrameFlood(sensorChannelIdx, msg, frame, sizeof(frame), enc, enclen);
    if (f <= 0) return;
    if (flood) {
        floodSend3(-1, frame, f, 20);
    } else {
        if (staggerMs) delay(staggerMs);
        txFrame((uint8_t*)frame, f);
    }
}

void otaSensorDraw() {
    #if HAS_OLED
    static uint32_t lastDraw = 0;
    if (millis() - lastDraw < OTA_DRAW_MS) return;
    lastDraw = millis();
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("OTA update");
    uint32_t pct = otaTotal ? (otaGot * 100 / otaTotal) : 0;
    if (pct > 100) pct = 100;
    display.printf("%u%%\n", (unsigned)pct);
    int bw = otaTotal ? (int)((long)otaGot * 128 / otaTotal) : 0;
    if (bw > 128) bw = 128;
    display.drawRect(0, 28, 128, 10, SSD1306_WHITE);
    display.fillRect(0, 28, bw, 10, SSD1306_WHITE);
    display.setCursor(0, 44);
    display.printf("Pkts: %d", packetCount);
    display.display();
    #endif
}

void otaSensorAbort(const char* why) {
    radioSetNormalConfig();
    otaFastMode = false;
    Serial.printf("[OTA] abort (%s) -> откат к прежней прошивке\n", why);
    Update.abort();
    otaActive = false;
    otaGotStart = false;
    otaCrcAcc = 0xFFFFFFFF;
    otaSeqExp = 0;
    #if HAS_OLED
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("OTA ABORT");
    display.println(why);
    display.display();
    #endif
}

// Неблокирующий тикер: если OTA зависла (нет пакетов OTA_SENSOR_STALL_MS) — откат.
void otaSensorTick() {
    if (!otaActive) return;
    if (millis() - otaLastActivity > OTA_SENSOR_STALL_MS) {
        otaSensorAbort("stall timeout");
    }
}

// Обработка OTA-команд от бота (вызывается из parseMeshCorePacket, сенсорный канал).
void otaSensorHandle() {
    String m = lastMessage;

    if (m == "ota:abort") { otaSensorAbort("from bot"); return; }

    if (m.startsWith("ota:start:")) {
        // ota:start:<target>:<total>:<crc32hex>
        String rest = m.substring(10);
        int p = rest.indexOf(':');
        if (p <= 0) return;
        String target = rest.substring(0, p);
        if (target != DEVICE_NAME) return;
        rest = rest.substring(p + 1);
        int p2 = rest.indexOf(':');
        if (p2 <= 0) return;
        uint32_t total = strtoul(rest.substring(0, p2).c_str(), NULL, 10);
        uint32_t crc = (uint32_t)strtoul(rest.substring(p2 + 1).c_str(), NULL, 16);
        if (total == 0 || total > (uint32_t)(3 * 1024 * 1024)) return;
        // откат предыдущей сессии, если вдруг была
        if (otaActive && otaGotStart) Update.abort();
        otaTotal = total; otaCrcExp = crc;
        otaGot = 0; otaCrcAcc = 0xFFFFFFFF; otaSeqExp = 0;
        otaActive = true; otaGotStart = true;
        otaLastActivity = millis();
        if (!Update.begin(total)) {
            Update.printError(Serial);
            otaSensorAbort("begin fail");
            return;
        }
        Serial.printf("[OTA] start %s: %u байт crc=%08X\n", DEVICE_NAME, total, crc);
        otaSensorDraw();
        otaSensorSend("ota:ackstart", true);
        // ackstart уходит на штатном конфиге (сенсор ещё не переключался);
        // бот после получения ackstart ждёт OTA_FAST_SETTLE_MS, затем шлёт чанки на быстром.
        // Мы переключаемся сразу после окончания floodSend3 ackstart ( blocking ~1 с).
        radioSetFastConfig();
        otaFastMode = true;
        Serial.printf("[OTA] fast config: %.1f MHz SF%d BW%.0f\n",
                      OTA_FAST_FREQ, OTA_FAST_SF, OTA_FAST_BW);
        return;
    }

    if (!otaActive) return;

    if (m.startsWith("ota:data:")) {
        // ota:data:<seq>:<crc16hex>:<hexdata>
        String rest = m.substring(9);
        int p = rest.indexOf(':');
        if (p <= 0) return;
        uint32_t seq = strtoul(rest.substring(0, p).c_str(), NULL, 10);
        rest = rest.substring(p + 1);
        int p2 = rest.indexOf(':');
        if (p2 <= 0) return;
        uint16_t rxcrc = hexToU16(rest.substring(0, p2).c_str());
        String data = rest.substring(p2 + 1);
        Serial.printf("[OTA] data seq=%u exp=%u len=%u crc16=0x%04X\n",
                      (unsigned)seq, (unsigned)otaSeqExp, (unsigned)data.length(), rxcrc);
        if (data.length() == 0 || (data.length() % 2) != 0 || data.length() > OTA_CHUNK_HEX) {
            Serial.printf("[OTA] REJECT bad len %u (max %d)\n", (unsigned)data.length(), OTA_CHUNK_HEX);
            otaSensorSend("ota:nack:" + String(otaSeqExp), false, OTA_ACK_STAGGER_MS);
            return;
        }
        if (seq != otaSeqExp) {
            Serial.printf("[OTA] REJECT seq %u != exp %u\n", (unsigned)seq, (unsigned)otaSeqExp);
            otaSensorSend("ota:nack:" + String(otaSeqExp), false, OTA_ACK_STAGGER_MS);
            return;
        }
        uint8_t chunk[OTA_CHUNK_BYTES + 2];
        int n = hexToBytes(data.c_str(), chunk, sizeof(chunk));
        if (n <= 0 || crc16buf(chunk, n) != rxcrc) {
            Serial.printf("[OTA] REJECT crc16 got=0x%04X exp=0x%04X n=%d\n",
                          crc16buf(chunk, n), rxcrc, n);
            otaSensorSend("ota:nack:" + String(seq), false, OTA_ACK_STAGGER_MS);
            return;
        }
        if (Update.write(chunk, n) != n) {
            Update.printError(Serial);
            Serial.printf("[OTA] REJECT write failed\n");
            otaSensorSend("ota:nack:" + String(seq), false, OTA_ACK_STAGGER_MS);
            return;
        }
        otaCrcAcc = crc32_upd(otaCrcAcc, chunk, n);
        otaGot += n;
        otaSeqExp = seq + 1;
        otaLastActivity = millis();
        otaSensorDraw();
        otaSensorSend("ota:ack:" + String(seq), false, OTA_ACK_STAGGER_MS);
        return;
    }

    if (m == "ota:end") {
        if (otaGot != otaTotal) {
            otaSensorSend("ota:nack:" + String(otaSeqExp), false, OTA_ACK_STAGGER_MS);
            return;
        }
        uint32_t actCrc = ~otaCrcAcc;
        if (actCrc != otaCrcExp || !otaGotStart) {
            Serial.printf("[OTA] CRC MISMATCH exp=%08X got=%08X\n", otaCrcExp, actCrc);
            otaSensorAbort("crc mismatch");
            otaSensorSend("ota:nackcrc");
            return;
        }
        if (!Update.end(true)) {
            Update.printError(Serial);
            otaSensorAbort("end fail");
            otaSensorSend("ota:nackcrc");
            return;
        }
        otaActive = false; otaGotStart = false;
        Serial.println("[OTA] DONE, rebooting into new firmware...");
        otaSensorSend("ota:ackend");
        delay(200);
        otaSensorSend("ota:reboot");
        delay(400);
        otaFastMode = false;
        ESP.restart();
        return;
    }
}
#endif

void loop() {
    #ifdef MQTT_ENABLED
    otaServer.handleClient();   // HTTP OTA: принимаем реквесты не блокируя радио
    otaBotTick();               // mesh OTA: таймауты повтора чанков

    // ===== MQTT RECONNECT (неблокирующий, раз в 5 с) =====
    if (millis() - lastMqttReconnectMs > MQTT_RECONNECT_INTERVAL_MS) {
        lastMqttReconnectMs = millis();
        tickRetryConnections();
    }
    if (mqttConnected) mqtt.loop();
    #endif

    // ===== ADVERT (периодический) =====
    if (isListening && !otaFastMode) {
        if (!advertBootSent && millis() > 6000) {   // стартовый beacon
            advertBootSent = true;
            sendAdvert(ADV_ROUTE_DIRECT);
        }
        if (millis() - lastDirectAdvertMs >= ADVERT_PERIOD_MS) {
            lastDirectAdvertMs = millis();
            sendAdvert(ADV_ROUTE_DIRECT);
        }
        if (millis() - lastFloodAdvertMs >= ADVERT_FLOOD_PERIOD_MS) {
            lastFloodAdvertMs = millis();
            sendAdvert(ADV_ROUTE_FLOOD);
        }
    }

    // ===== ПРИЁМ =====
    if (isListening) {
        // Периодический сброс AGC, если не идёт приём пакета прямо сейчас.
        // Не сбрасываем, пока стоит RX_DONE (иначе потеряем пакет).
        bool rxPending = (radio.getIrqFlags() & RADIOLIB_SX126X_IRQ_RX_DONE) != 0;
        if (!rxPending && (millis() - lastReArmMs > RADIO_REARM_INTERVAL_MS)) {
            rearmRadioAGC();
            rxPending = (radio.getIrqFlags() & RADIOLIB_SX126X_IRQ_RX_DONE) != 0;
        }

        // читать только если радио действительно получило пакет (RX_DONE)
        if (rxPending) {
            uint8_t buffer[256];
            int state = radio.readData(buffer, sizeof(buffer));
            if (state == RADIOLIB_ERR_NONE) {
                int pktLen = radio.getPacketLength();
                float rssi = radio.getRSSI();
                float snr = radio.getSNR();
                if (pktLen > 0) {
                    Serial.printf("\n[RX] len=%d RSSI=%.1f SNR=%.1f ", pktLen, rssi, snr);
                    // hex-дамп опускаем в fast-режиме: каждый пакет — это ~8 мс на UART
                    if (!otaFastMode)
                        for (int i = 0; i < min(pktLen, 24); i++) Serial.printf("%02X", buffer[i]);
                    Serial.println();
                }
                if (checkAndMarkSeen(buffer, pktLen)) {
                    duplicateCount++;
                    Serial.printf("[DUP] skipped (total dups=%lu)\n", duplicateCount);
                } else {
                    bool parsed = parseMeshCorePacket(buffer, pktLen);

                    // hex-экран только для GRP_TXT, который не расшифровался
                    // (рекламные/служебные пакеты экран не трогаем)
                    if (pktLen > 0 && !parsed && !otaFastMode && ((buffer[0] >> 2) & 0x0F) == 0x05) {
                        if (!screenOff) {
                            display.setTextSize(1);
                            display.clearDisplay();
                            display.setCursor(0, 0);
                            display.printf("RX %dB RSSI:%.0f\n", pktLen, rssi);
                            display.printf("SNR:%.0f pkts:%d\n", snr, packetCount);
                            display.printf("hex:");
                            for (int i = 0; i < min(pktLen, 21); i++) display.printf("%02X", buffer[i]);
                            display.display();
                        }
                        lastRxDisplay = millis();
                    }

// не перезатираем экран 5 сек после сообщения
                    if (parsed) {
                        lastRxDisplay = millis();
                        lastScreenActivityMs = millis();   // активность — отодвигает авто-гашение
                        #ifdef MQTT_ENABLED
                        publishMessage();
                        #endif
                    }
                    lastReArmMs = millis();  // был приём — сброс AGC откладываем
                    radio.startReceive();
                }
            } else {
                // Захват сорвался (CRC и т.п.) — флаг RX_DONE мог остаться,
                // что приведёт к бесконечному циклу. Сбрасываем флаги и ре-армим.
                Serial.printf("[RX] readData error %d, re-arming\n", state);
                radio.clearIrqStatus();
                rearmRadioAGC();
            }
        }
    }

    // Показать статус на экране (обновляем раз в 500мс)
    if (isListening && !screenOff && (millis() - lastDisplayUpdate > 500)) {
        lastDisplayUpdate = millis();
        if (!otaFastMode && (millis() - lastRxDisplay > 5000)) {
            drawIdleStatus();
        }
    }

    #ifdef MQTT_ENABLED
    // ===== MQTT STATUS (раз в 60 сек) =====
    if (mqttConnected && millis() - lastStatusPublishMs > MQTT_STATUS_INTERVAL_MS) {
        lastStatusPublishMs = millis();
        publishStatus();
    }
    // ===== ДОСТУПНОСТЬ ДАТЧИКОВ: если от датчика давно ничего нет — offline =====
    if (mqttConnected && millis() - lastSensorAvailCheckMs > 10000) {
        lastSensorAvailCheckMs = millis();
        for (int i = 0; i < sensorDeviceDiscCount; i++) {
            if (sensorOnlineNow[i] && millis() - sensorLastActive[i] > SENSOR_OFFLINE_MS) {
                sensorOnlineNow[i] = false;
                char slug[48];
                mqttSlug(sensorDeviceDisc[i].c_str(), slug, sizeof(slug));
                char tAvail[128];
                snprintf(tAvail, sizeof(tAvail), "%s/sensor/%s/available", mqttPrefix, slug);
                mqtt.publish(tAvail, "offline", true);
                Serial.printf("[SNS] %s OFFLINE (no data for %lus)\n",
                              sensorDeviceDisc[i].c_str(), (unsigned long)(SENSOR_OFFLINE_MS / 1000));
            }
        }
    }
    // ===== Ре-синк NTP раз в час (configTime снова делает stop+init) =====
    if (ntpStarted && wifiConnected && (int32_t)(millis() - lastNtpSyncMs) >= (int32_t)NTP_RESYNC_INTERVAL_MS) {
        lastNtpSyncMs = millis();
        configTime(0, 0, "pool.ntp.org");
    }
    // ===== Первая успешная синхронизация SNTP: лог + сразу рассылка времени сенсорам =====
    // Отслеживаем статус lwIP SNTP (SNTP_SYNC_STATUS_COMPLETED), а не сдвиг часов:
    // при свежем билде реальное время может лишь на минуты отличаться от build-time.
    if (ntpStarted && !ntpSyncedLogged &&
        sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        ntpSyncedLogged = true;
        time_t local = time(NULL) + (time_t)TZ_OFFSET_HOURS * 3600;
        struct tm tm_now;
        gmtime_r(&local, &tm_now);
        char tbuf[32];
        strftime(tbuf, sizeof(tbuf), "%H:%M:%S %d.%m.%Y", &tm_now);
        Serial.printf("[RTC] NTP time synced: %s\n", tbuf);
        if (sensorChannelIdx >= 0) {
            lastSensorTimeSyncMs = millis();
            sendSensorTimeSync();
        }
    }
    // ===== Рассылка актуального времени сенсорам в сенсорный канал =====
    // Работает только при реально синхронизированном времени (ntpSyncedLogged):
    // build-time устаревает, и датчики должны получать истинный epoch.
    if (ntpSyncedLogged && sensorChannelIdx >= 0 &&
        (int32_t)(millis() - lastSensorTimeSyncMs) >= (int32_t)SENSOR_TIME_SYNC_INTERVAL_MS) {
        lastSensorTimeSyncMs = millis();
        sendSensorTimeSync();
    }
    // ===== Сброс lastmsg после паузы (чтобы повторный одинаковый текст триггерил HA) =====
    clearLastMsg();
    // ===== Сброс text-топика после триггера "button" (повторное нажатие = новый state_changed) =====
    clearSensorBtnText();
    #endif

    // Sensor node: button = trigger ("button"), hello = heartbeat раз в N минут
#ifdef SENSOR_NODE
    otaSensorTick();   // mesh OTA: сторожевое время — при зависании откат к прежней прошивке
    static unsigned long lastBtnPress = 0;
    static unsigned long lastHeartbeat = 0;
    if (millis() - lastBtnPress > 5000) {
        if (digitalRead(BUTTON_PIN) == LOW) {
            lastBtnPress = millis();
            // ждём отпускания кнопки (таймаут защиты от залипания)
            unsigned long heldAt = millis();
            while (digitalRead(BUTTON_PIN) == LOW && millis() - heldAt < 5000) delay(2);

            // окно двойного нажатия: если в течение SNS_BTN_DBL_WINDOW_MS
            // кнопку нажмут снова — это button2
            bool isDouble = false;
            unsigned long waitUntil = millis() + SNS_BTN_DBL_WINDOW_MS;
            while (millis() < waitUntil) {
                if (digitalRead(BUTTON_PIN) == LOW) {
                    isDouble = true;
                    unsigned long heldAt2 = millis();
                    while (digitalRead(BUTTON_PIN) == LOW && millis() - heldAt2 < 5000) delay(2);
                    break;
                }
                delay(5);
            }
            sensorSendMsg(isDouble ? SENSOR_MSG_BUTTON2 : SENSOR_MSG_BUTTON);
        }
    }
    // Регулярный heartbeat: держит сенсор "online" в HA даже без кнопок
    if (millis() - lastHeartbeat >= SENSOR_HEARTBEAT_MS) {
        lastHeartbeat = millis();
        sensorSendMsg(SENSOR_MSG_HELLO);
    }
#endif

    delay(10);
}