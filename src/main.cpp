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
#include <sys/time.h>
#include <time.h>
#include <Preferences.h>

#ifdef MQTT_ENABLED
#include <WiFi.h>
#include <PubSubClient.h>
#endif

// Per-board pins (Heltec V4.3 by default), radio params and display driver
// are mapped here from build_flags — see README / platformio.ini.
#include "board_config.h"

// Имя устройства (идёт в заголовке исходящих сообщений)
#ifndef DEVICE_NAME
#define DEVICE_NAME "Tr1glav_esp_bot"
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

// ===== ОТВЕТ НА /ping: обратный маршрут + ретрай =====
// Путь, которым к нам дошёл последний пакет (в порядке «от источника»),
// и место/текст ответа для повтора через ~250 мс.
#define MAX_REPLY_PATH 63
uint8_t replyPath[MAX_REPLY_PATH];
uint8_t replyHopCount = 0;
uint8_t replyHashSize = 1;
String pingReplyText = "";
uint8_t pingReplyFrame[300];
int pingReplyFrameLen = 0;
int pingReplyChannel = 0;
uint8_t pingReplyEnc[256];      // зашифрованный блок ответа — общий для
int pingReplyEncLen = 0;        // первого (direct) и повторного (flood) кадра
bool replyPendingRetransmit = false;
unsigned long replyRetransmitAt = 0;

// ===== ОТВЕТ В ЛИЧКУ (TXT_MSG) =====
// Кадр приватного ответа хранится целиком: повтор 250 мс — той же копией.
uint8_t dmSrcHash = 0;
uint8_t dmReplyFrame[300];
int dmReplyFrameLen = 0;
bool dmReplyPendingRetransmit = false;
unsigned long dmReplyRetransmitAt = 0;

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

// Выбранный канал для отправки из HA (index в channels[])
int mqttTxChannel = 1;  // по умолчанию #connections

// Обнуление lastmsg через некоторое время после публикации (для повторных триггеров HA)
#define LASTMSG_RESET_MS 4000
unsigned long lastmsgClearAt = 0;
bool lastmsgPendingClear = false;

// Буферы для кадров TX из HA
uint8_t mqttTxFrame[300];
int mqttTxFrameLen = 0;
bool mqttTxPending = false;

// Forward declarations
void setupMQTT();
void tickRetryConnections();
void publishDiscovery();
void publishMessage();
void publishSensorMessage();   // fwd-decl: вызывается из parseMeshCorePacket
void publishStatus();
void clearLastMsg();
void mqttCallback(char* topic, byte* payload, unsigned int length);
#endif

// Декларация вне #ifdef: вызов из parseMeshCorePacket компилируется во всех сборках
// (в non-MQTT-сборках сенсорный канал не создаётся и вызов недостижим).
void publishSensorMessage();

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
Preferences prefs;
#define PRIV_CHAN_NS    "meshbot"
#define PRIV_CHAN_TXCH  "txchan"

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

void saveTxChannel(int idx) {
#ifdef MQTT_ENABLED
    prefs.begin(PRIV_CHAN_NS, false);
    prefs.putInt(PRIV_CHAN_TXCH, idx);
    prefs.end();
    if (mqttConnected) {
        char cfgTopic[96];
        snprintf(cfgTopic, sizeof(cfgTopic), "%s/cfg/channel", mqttPrefix);
        mqtt.publish(cfgTopic, channels[idx].name, true);   // retained: соберём при рестарте
    }
#endif
    Serial.printf("[MQTT] TX channel %s saved to NVS + retained cfg\n", channels[idx].name);
}

#ifdef MQTT_ENABLED
void loadTxChannel() {
    prefs.begin(PRIV_CHAN_NS, false);
    int idx = prefs.getInt(PRIV_CHAN_TXCH, -1);
    prefs.end();
    if (idx < 0) {
        // дефолт из build-флагов, если такого канала ещё нет в NVS
        idx = findChannelByName(TX_CHANNEL);
        if (idx < 0) idx = 1;   // #connections
        if (idx < numChannels) {
            Serial.printf("[MQTT] no saved TX channel, using default '%s'\n", channels[idx].name);
        }
    }
    if (idx < numChannels) {
        mqttTxChannel = idx;
        Serial.printf("[MQTT] TX channel restored from NVS: %s\n", channels[idx].name);
    } else {
        Serial.printf("[MQTT] saved TX channel idx %d out of range, using default\n", idx);
    }
}
#endif // MQTT_ENABLED (loadTxChannel использует mqttTxChannel)

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
        #ifdef MQTT_ENABLED
        publishSensorMessage();
        #endif
        return true;
    }

    // === ОТВЕТ НА СООБЩЕНИЕ ===
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
                    dmReplyFrameLen = dl;
                    Serial.printf("\n[TX DM] to <%02X>: %s (%dB)\n", dmSrcHash, pingReplyText.c_str(), dl);
                    for (int i = 0; i < dl; i++) Serial.printf("%02X", dmReplyFrame[i]);
                    Serial.println();
                    if (txFrame(dmReplyFrame, dl) == RADIOLIB_ERR_NONE) {
                        dmReplyPendingRetransmit = true;
                        dmReplyRetransmitAt = millis() + 250;   // повтор лички
                    }
                }
            } else {
                Serial.printf("[DM] pubkey <%02X> неизвестен (нет advert) — ответ не отправлен\n", dmSrcHash);
            }
            return true;
        }

        // Шифруем ответ ОДИН раз и храним блок: первый кадр уходит DIRECT по
        // обратному маршруту (если путь был), повтор через ~250 мс — флудом.
        // Оба кадра несут тот же зашифрованный блок (один timestamp) =>
        // приёмник дедуплицирует их как повторы одного сообщения.
        int enclen = buildGroupEnc(chIdx, pingReplyText, pingReplyEnc);
        if (enclen <= 0) return true;
        pingReplyEncLen = enclen;

        int f = 0;
        bool viaReturnPath = (replyHopCount > 0);
        if (viaReturnPath) {
            f = buildGroupFrameReturnPath(chIdx, pingReplyText, replyPath,
                                          replyHopCount, replyHashSize,
                                          pingReplyFrame, sizeof(pingReplyFrame),
                                          pingReplyEnc, pingReplyEncLen);
        } else {
            f = buildGroupFrameFlood(chIdx, pingReplyText, pingReplyFrame, sizeof(pingReplyFrame),
                                     pingReplyEnc, pingReplyEncLen);
        }
        if (f > 0) {
            pingReplyFrameLen = f;
            pingReplyChannel = chIdx;
            Serial.printf("\n[TX] %s: %s (%dB, %s)\n", channels[chIdx].name,
                          (DEVICE_NAME ": " + pingReplyText).c_str(), f,
                          viaReturnPath ? "direct" : "flood");
            if (sendFrame(chIdx, pingReplyFrame, pingReplyFrameLen) == RADIOLIB_ERR_NONE) {
                replyPendingRetransmit = true;
                replyRetransmitAt = millis() + 250;   // повтор на случай desense ближнего узла
            }
        }
    }
    
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
    uint32_t ts = (uint32_t)millis();
    memcpy(plaintext, &ts, 4); plen += 4;           // timestamp (LE)
    plaintext[plen++] = 0;                          // TXT_TYPE_PLAIN
    const char* prefix = DEVICE_NAME ": ";
    memcpy(plaintext + plen, prefix, strlen(prefix)); plen += strlen(prefix);
    size_t mlen = min((size_t)200, msg.length());
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
    uint32_t ts = (uint32_t)time(NULL) * 1000 + (millis() % 1000);
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
    for (int i = 0; i < f; i++) Serial.printf("%02X", frame[i]);
    Serial.println();
    return txFrame((uint8_t*)frame, f);
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

    // --- meshcore/bot/.../cmd/channel ---
    if (strstr(topic, "/cmd/channel")) {
        int tci = findChannelByName(msg);
        if (tci >= 0) {
            mqttTxChannel = tci;
            Serial.printf("[MQTT] TX channel → %s\n", channels[tci].name);
            saveTxChannel(tci);
        } else {
            Serial.printf("[MQTT] unknown channel '%s'\n", msg);
        }
        // публикуем обратно текущее значение
        char stateTopic[96];
        snprintf(stateTopic, sizeof(stateTopic), "%s/state", mqttPrefix);
        mqtt.publish(stateTopic, channels[mqttTxChannel].name, true);
        return;
    }

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
            sendFrame(mqttTxChannel, frame, fl);
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

    // --- meshcore/bot/.../cfg/* (retained-конфиг, восстанавливается при старте) ---
    // Применяем БЕЗ обратной публикации и без discovery, чтобы исключить циклы.
    if (strstr(topic, "/cfg/channel")) {
        int tci = findChannelByName(msg);
        if (tci >= 0) {
            mqttTxChannel = tci;
            Serial.printf("[MQTT] TX channel restored from cfg: %s\n", channels[tci].name);
            prefs.begin(PRIV_CHAN_NS, false);
            prefs.putInt(PRIV_CHAN_TXCH, tci);
            prefs.end();
        } else {
            Serial.printf("[MQTT] cfg/channel: unknown channel '%s'\n", msg);
        }
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

    // --- Select: канал (options включают приватный и сенсорный каналы) ---
    String chOpts = "\"#public\",\"#connections\"";
    if (privateChannelIdx >= 0) chOpts += String(",\"") + privateChannelName + "\"";
    if (sensorChannelIdx >= 0) chOpts += String(",\"") + sensorChannelName + "\"";
    char selTopic[128], selPayload[600];
    snprintf(selTopic, sizeof(selTopic), "homeassistant/select/meshcore_%s/channel/config", DEVICE_NAME);
    snprintf(selPayload, sizeof(selPayload),
        "{\"name\":\"%s Channel\","
        "\"command_topic\":\"%s/cmd/channel\","
        "\"state_topic\":\"%s/state\","
        "\"value_template\":\"{{ value_json.channel }}\","
        "\"options\":[%s],"
        "\"retain\":true,"
        "\"unique_id\":\"meshcore_%s_ch\","
        "\"icon\":\"mdi:radio-tower\","
        "\"device\":{%s}}",
        DEVICE_NAME, mqttPrefix, mqttPrefix, chOpts.c_str(), DEVICE_NAME, devBlock);
    mqtt.publish(selTopic, selPayload, true);

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
    char topic[128], payload[320];
    snprintf(topic, sizeof(topic), "homeassistant/text/meshcore_sensor_%s/state/config", slug);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s data\",\"state_topic\":\"%s/sensor/%s/text\","
        "\"icon\":\"mdi:sprout\",\"unique_id\":\"meshcore_sensor_%s_text\","
        "\"entity_category\":\"diagnostic\","
        "\"device\":{%s}}",
        sender.c_str(), mqttPrefix, slug, slug, devBlock);
    mqtt.publish(topic, payload, true);
    Serial.printf("[MQTT] sensor device discovery: %s\n", slug);

    char retTopic[128], retPayload[160];
    snprintf(retTopic, sizeof(retTopic), "homeassistant/sensor/meshcore_sensor_%s/rssi/config", slug);
    snprintf(retPayload, sizeof(retPayload),
        "{\"name\":\"%s RSSI\",\"state_topic\":\"%s/sensor/%s/rssi\","
        "\"unit_of_measurement\":\"dBm\",\"device_class\":\"signal_strength\","
        "\"unique_id\":\"meshcore_sensor_%s_rssi\","
        "\"device\":{%s}}",
        sender.c_str(), mqttPrefix, slug, slug, devBlock);
    mqtt.publish(retTopic, retPayload, true);
    Serial.printf("[MQTT] sensor rssi discovery: %s\n", slug);
}

// Публикует сообщение из сенсорного канала: устройство HA на каждого отправителя.
void publishSensorMessage() {
    #ifdef MQTT_ENABLED
    if (!mqttConnected) return;
    char slug[48];
    mqttSlug(lastSender.c_str(), slug, sizeof(slug));
    if (strlen(slug) == 0) snprintf(slug, sizeof(slug), "unknown");
    // discovery публикуется один раз на отправителя (кэш имён)
    bool known = false;
    for (int i = 0; i < sensorDeviceDiscCount; i++) {
        if (sensorDeviceDisc[i] == lastSender) { known = true; break; }
    }
    if (!known) {
        publishSensorDisc(lastSender, slug);
        if (sensorDeviceDiscCount < SENSOR_DEV_CACHE_MAX) {
            sensorDeviceDisc[sensorDeviceDiscCount++] = lastSender;
        }
    }
    char escText[128];
    jsonEscape(lastMessage.c_str(), escText, sizeof(escText));
    char tText[128], tRssi[128];
    snprintf(tText, sizeof(tText), "%s/sensor/%s/text", mqttPrefix, slug);
    snprintf(tRssi, sizeof(tRssi), "%s/sensor/%s/rssi", mqttPrefix, slug);
    mqtt.publish(tText, escText);
    char rssiStr[24];
    snprintf(rssiStr, sizeof(rssiStr), "%.1f", lastRSSI);
    mqtt.publish(tRssi, rssiStr);
    Serial.printf("[SNS] %s: %s (rssi %.1f)\n", lastSender.c_str(), lastMessage.c_str(), lastRSSI);
    #else
    Serial.printf("[SNS] %s: %s (MQTT disabled)\n", lastSender.c_str(), lastMessage.c_str());
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
        "\"temp\":%.1f,"
        "\"channel\":\"%s\",\"private\":\"%s\"}",
        isListening ? "true" : "false",
        upSec, packetCount, duplicateCount,
        tempC,
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
        snprintf(cmdTopic, sizeof(cmdTopic), "%s/cmd/channel", mqttPrefix);
        mqtt.subscribe(cmdTopic);

        // Retained-конфиг (для восстановления при рестарте без циклов)
        snprintf(cmdTopic, sizeof(cmdTopic), "%s/cfg/channel", mqttPrefix);
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
    WiFi.setSleep(false);   // без modem-sleep: меньше задержка отвёта
    setupMQTT();            // только конфиг (префиксы/сервер/коллбэк)
    #endif

    radio.startReceive();
    isListening = true;
    lastDirectAdvertMs = lastFloodAdvertMs = millis();
    
    #if HAS_OLED
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("MeshCore");
    display.println(channelListStr().substring(0, 16));
    display.println("Listening...");
    display.print(sysTimeStr);
    display.display();
    #endif
    
    Serial.printf("Listening on %s...\n", channelListStr().c_str());
    lastScreenActivityMs = millis();   // старт таймера автовыключения экрана
}

void loop() {
    #ifdef MQTT_ENABLED
    // ===== MQTT RECONNECT (неблокирующий, раз в 5 с) =====
    if (millis() - lastMqttReconnectMs > MQTT_RECONNECT_INTERVAL_MS) {
        lastMqttReconnectMs = millis();
        tickRetryConnections();
    }
    if (mqttConnected) mqtt.loop();
    #endif

    // ===== ADVERT (периодический) =====
    if (isListening) {
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

    // ===== РЕТРАЙ ОТВЕТА /ping (повтор через ~250 мс — уже ФЛУДОМ) =====
    if (isListening && replyPendingRetransmit && millis() >= replyRetransmitAt) {
        replyPendingRetransmit = false;
        if (pingReplyFrameLen > 0) {
            // Тот же зашифрованный блок (тот же timestamp) => копии одного сообщения.
            uint8_t flood[300];
            int fl = buildGroupFrameFlood(pingReplyChannel, pingReplyText, flood, sizeof(flood),
                                          pingReplyEnc, pingReplyEncLen);
            if (fl > 0) {
                Serial.printf("\n[PING] retransmit reply (flood): %s\n", pingReplyText.c_str());
                Serial.printf("[TX] %s: %s (%dB)\n", channels[pingReplyChannel].name,
                              (DEVICE_NAME ": " + pingReplyText).c_str(), fl);
                sendFrame(pingReplyChannel, flood, fl);
            }
        }
    }

    // ===== РЕТРАЙ ОТВЕТА В ЛИЧКУ (повтор тех же байт через ~250 мс) =====
    if (isListening && dmReplyPendingRetransmit && millis() >= dmReplyRetransmitAt) {
        dmReplyPendingRetransmit = false;
        if (dmReplyFrameLen > 0) {
            Serial.printf("\n[DM] retransmit reply to <%02X>: %s (%dB)\n",
                          dmSrcHash, pingReplyText.c_str(), dmReplyFrameLen);
            for (int i = 0; i < dmReplyFrameLen; i++) Serial.printf("%02X", dmReplyFrame[i]);
            Serial.println();
            txFrame(dmReplyFrame, dmReplyFrameLen);
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
                    if (pktLen > 0 && !parsed && ((buffer[0] >> 2) & 0x0F) == 0x05) {
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
        if (millis() - lastRxDisplay > 5000) {
            drawIdleStatus();
        }
    }

    #ifdef MQTT_ENABLED
    // ===== MQTT STATUS (раз в 60 сек) =====
    if (mqttConnected && millis() - lastStatusPublishMs > MQTT_STATUS_INTERVAL_MS) {
        lastStatusPublishMs = millis();
        publishStatus();
    }
    // ===== Ре-синк NTP раз в час (configTime снова делает stop+init) =====
    if (ntpStarted && wifiConnected && (int32_t)(millis() - lastNtpSyncMs) >= (int32_t)NTP_RESYNC_INTERVAL_MS) {
        lastNtpSyncMs = millis();
        configTime(0, 0, "pool.ntp.org");
    }
    // ===== Одноразовый лог, когда SNTP уточнил build-time =====
    if (ntpStarted && !ntpSyncedLogged && time(NULL) > (time_t)BUILD_UNIX_TIME + 3600) {
        ntpSyncedLogged = true;
        time_t local = time(NULL) + (time_t)TZ_OFFSET_HOURS * 3600;
        struct tm tm_now;
        gmtime_r(&local, &tm_now);
        char tbuf[32];
        strftime(tbuf, sizeof(tbuf), "%H:%M:%S %d.%m.%Y", &tm_now);
        Serial.printf("[RTC] NTP time synced: %s\n", tbuf);
    }
    // ===== Сброс lastmsg после паузы (чтобы повторный одинаковый текст триггерил HA) =====
    clearLastMsg();
    #endif

    // Sensor node: send boot message into sensor channel on button press
#ifdef SENSOR_NODE
    static unsigned long lastBtnPress = 0;
    if (millis() - lastBtnPress > 5000) {
        if (digitalRead(BUTTON_PIN) == LOW) {
            lastBtnPress = millis();
            // Send "hello" message into sensor channel
            uint8_t enc[256];
            int enclen = buildGroupEnc(sensorChannelIdx, "hello", enc);
            if (enclen > 0) {
                uint8_t frame[300];
                int f = buildGroupFrameFlood(sensorChannelIdx, "hello", frame, sizeof(frame), enc, enclen);
                if (f > 0) {
                    txFrame(frame, f);
                    Serial.println("[SNS] boot message sent to sensor channel");
                }
            }
            // wait for release (with timeout to avoid hanging)
            while (digitalRead(BUTTON_PIN) == LOW && millis() - lastBtnPress < 5000);
        }
    }
#endif

    delay(10);
}