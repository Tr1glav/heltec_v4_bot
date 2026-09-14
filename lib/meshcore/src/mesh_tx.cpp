#include "config.h"
#include "globals.h"
#include "crypto.h"
#include "radio.h"
#include "mesh.h"
#include "display.h"
#include "ota.h"
#include "companion.h"

// ===== Передача =====
// Выделено из mesh.cpp: объявление себя в сети, сборка группового и личного кадра,
// повторы флуда и сообщения узла (heartbeat, проверка связи, синхронизация времени).

void sendAdvert(uint8_t route_type) {
    uint8_t app[32];
    int applen = 0;
    app[applen++] = 0x80 | 0x01;  // ADV_TYPE_CHAT + имя
    const char* name = cfg.name.c_str();
    int nlen = (int)cfg.name.length();
    if (nlen > 31) nlen = 31;
    memcpy(app + applen, name, nlen);
    applen += nlen;

    uint8_t frame[190];
    int f = 0;
    frame[f++] = (uint8_t)((0x04 << 2) | (route_type & 0x03));  // ADVERT | route
    frame[f++] = PATH_LEN_INIT;  // path_len: размер хэша и 0 хопов

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

int buildGroupEnc(int chIdx, const String& msg, uint8_t* enc) {
    if (chIdx < 0 || chIdx >= numChannels) return 0;
    uint8_t plaintext[GROUP_TEXT_MAX_PLAIN];
    // ts — Unix-время в СЕКУНДАХ (как у adverts и личных сообщений): epoch-ms не
    // помещается в uint32. Пока часы не выставлены — millis.
    uint32_t now = (uint32_t)time(NULL);
    uint32_t ts = (now > 1000000000) ? now : (uint32_t)millis();
    memcpy(plaintext, &ts, 4);                      // timestamp (LE)
    plaintext[4] = 0;                               // TXT_TYPE_PLAIN
    size_t plen = 5;
    // "имя: " собирается в рантайме: имя приходит из настроек, а не из макроса
    char prefix[48];
    int pn = snprintf(prefix, sizeof(prefix), "%s: ", cfg.name.c_str());
    if (pn < 0) pn = 0;
    if (pn > (int)sizeof(prefix) - 1) pn = (int)sizeof(prefix) - 1;
    size_t n = min((size_t)pn, sizeof(plaintext) - plen);
    memcpy(plaintext + plen, prefix, n); plen += n;
    n = min((size_t)msg.length(), sizeof(plaintext) - plen);
    memcpy(plaintext + plen, msg.c_str(), n); plen += n;

    return encryptGroupText(channels[chIdx].secret, enc, plaintext, plen);
}

int buildGroupFrameFlood(int chIdx, const String& msg, uint8_t* frame, int maxlen) {
    uint8_t enc[GROUP_TEXT_MAX_PLAIN + 2];
    int enclen = buildGroupEnc(chIdx, msg, enc);
    if (enclen <= 0 || 3 + enclen > maxlen) return 0;
    frame[0] = 0x15;                    // GRP_TXT | ROUTE_TYPE_FLOOD
    frame[1] = PATH_LEN_INIT;           // размер хэша, 0 хопов (путь достроят ретрансляторы)
    frame[2] = channels[chIdx].hash;
    memcpy(frame + 3, enc, enclen);
    return 3 + enclen;
}

int buildPrivateTextFrame(uint8_t dest_hash, const uint8_t* dest_pub,
                          const String& msg, uint8_t* frame, int maxlen) {
    uint8_t secret[32];
    ed25519_key_exchange(secret, dest_pub, bot_prv64);

    uint8_t data[DM_TEXT_MAX + 8];
    int dlen = 0;
    uint32_t ts = (uint32_t)time(NULL);   // Unix-секунды (epoch-ms не лезет в uint32)
    memcpy(data, &ts, 4); dlen += 4;
    data[dlen++] = 0;                        // attempt = 0
    size_t ml = min((size_t)DM_TEXT_MAX, (size_t)msg.length());
    memcpy(data + dlen, msg.c_str(), ml); dlen += ml;
    data[dlen++] = 0;                        // null terminator

    uint8_t enc[DM_TEXT_MAX + 24];           // MAC 2 + шифр, дополненный до кратного 16
    int enclen = encryptGroupText(secret, enc, data, dlen);   // [MAC 2B][cipher]
    if (enclen <= 0 || 4 + enclen > maxlen) return 0;

    int f = 0;
    frame[f++] = 0x09;                       // TXT_MSG | ROUTE_TYPE_FLOOD
    frame[f++] = PATH_LEN_INIT;              // размер хэша, 0 хопов
    frame[f++] = dest_hash;
    frame[f++] = ownShortHash;
    memcpy(frame + f, enc, enclen); f += enclen;
    return f;
}

int sendFrame(int chIdx, const uint8_t* frame, int f) {
    if (chIdx < 0 || chIdx >= numChannels) return RADIOLIB_ERR_UNKNOWN;
    // hex-лог кадра стоит ~40 мс на UART для 245-байтного кадра — в fast-режиме молчим
    if (!otaFastMode) {
        for (int i = 0; i < f; i++) Serial.printf("%02X", frame[i]);
        Serial.println();
    }
    return txFrame((uint8_t*)frame, f);
}

void floodSend(int chIdx, const uint8_t* frame, int f, unsigned int gapMs, int repeats) {
    for (int i = 0; i < repeats; i++) {
        if (chIdx >= 0) sendFrame(chIdx, frame, f);
        else            txFrame((uint8_t*)frame, f);
        if (i < repeats - 1) delay(gapMs);
    }
}

void sensorSendMsg(const char* msg, unsigned int gapMs, int repeats) {
    if (sensorChannelIdx < 0) {
        Serial.printf("[SNS] sensor channel not configured, cannot send \"%s\"\n", msg);
        return;
    }
    uint8_t frame[256];
    int f = buildGroupFrameFlood(sensorChannelIdx, msg, frame, sizeof(frame));
    if (f <= 0) return;
    floodSend(-1, frame, f, gapMs, repeats);
    Serial.printf("[SNS] sent \"%s\" to sensor channel\n", msg);
    #ifdef COMPANION_NODE
    // Собственные передачи в приложение иначе не попадают: в очередь кладётся только
    // принятое из эфира, а свой же флуд отбрасывается как эхо. Кладём прямо здесь, с тем
    // же префиксом имени, с каким сообщение ушло в эфир.
    companionOnChannelText(sensorChannelIdx, cfg.name + ": " + msg, 0.0f, PATH_LEN_INIT, false);
    #endif
    #ifdef SENSOR_NODE
    sensorLastSent = msg;
    sensorLastSentMs = millis();
    #endif
}

// "time:<epoch>:<версия бота>" — сенсор выставляет часы и сверяет свою версию с ботом
#ifdef SENSOR_NODE
// hello:<версия>:<заряд %>:<напряжение>:<код платы> — бот публикует эти поля в MQTT.
// Без измерения батареи вместо значений идёт "-", чтобы позиция кода платы не съезжала.
void sensorSendHello() {
    // Пятым полем идёт окружение сборки: код платы больше не определяет прошивку —
    // сенсор и компаньон живут на одной h43, а образы у них разные.
    char msg[96];
    #if HAS_BATTERY
    if (batteryPresent()) {
        char bv[12];
        snprintf(msg, sizeof(msg), "%s:%s:%d:%s:%s:%s", SENSOR_MSG_HELLO, FW_VERSION,
                 batteryPercent(), fmtFix(batteryVoltage(), 2, bv, sizeof(bv)),
                 BOARD_CODE, FW_ENV);
    } else {
        snprintf(msg, sizeof(msg), "%s:%s:-:-:%s:%s", SENSOR_MSG_HELLO, FW_VERSION,
                 BOARD_CODE, FW_ENV);
    }
    #else
    snprintf(msg, sizeof(msg), "%s:%s:-:-:%s:%s", SENSOR_MSG_HELLO, FW_VERSION,
             BOARD_CODE, FW_ENV);
    #endif
    sensorSendMsg(msg);
}
#endif

#ifdef SENSOR_NODE
// Эхо-запрос: одиночная посылка, чтобы измерять время одного обмена, а не повторов
void sensorPingSend() {
    if (sensorChannelIdx < 0) return;
    pingId = (uint16_t)millis();
    if (pingId == 0) pingId = 1;
    pingFailed = false;
    pingShowUntil = 0;
    char msg[24];
    snprintf(msg, sizeof(msg), "%s%u", SENSOR_MSG_PING, (unsigned)pingId);
    pingSentMs = millis();
    sensorSendMsg(msg, FLOOD_RETRY_MS, 1);
}

void sensorPingTick() {
    if (pingSentMs == 0) return;
    if (millis() - pingSentMs < PING_TIMEOUT_MS) return;
    pingSentMs = 0;
    pingFailed = true;
    pingShowUntil = millis() + PING_SHOW_MS;
    Serial.println("[PING] ответа нет");
}
#endif

void sendSensorTimeSync() {
    char msg[48];
    snprintf(msg, sizeof(msg), "time:%llu:%s", (unsigned long long)time(NULL), FW_VERSION);
    sensorSendMsg(msg);
}

String channelListStr() {
    String s = "";
    for (int i = 0; i < numChannels; i++) {
        if (i > 0) s += " + ";
        s += channels[i].name;
    }
    return s;
}
