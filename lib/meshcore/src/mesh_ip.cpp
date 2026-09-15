// ===== IP over MeshCore — транспорт =====
// Надёжная доставка фрагментированных IP-датаграмм поверх сенсорного канала.
// Формат:  данные  "ip:d" + 3 hex (seq) + 1 hex (fi<<4 | nf) + base64(raw)
//          подтв.  "ip:a" + 3 hex (base) + 2 hex (bitmap8)

#include "config.h"
#if FEATURE_MESH_IP

#include "mesh_ip.h"
#include "globals.h"
#include "mesh.h"
#include "radio.h"

#include <Arduino.h>
#include <cstring>

// ===================== Хелперы =====================

static const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static uint8_t hexval(char c) {
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    return 0xFF;
}
static uint8_t  hex1(const char* p) { return hexval(p[0]); }
static uint16_t hex3(const char* p) {
    return (uint16_t)((hexval(p[0]) << 8) | (hexval(p[1]) << 4) | hexval(p[2]));
}
static uint16_t hex4(const char* p) {
    return (uint16_t)((hexval(p[0]) << 12) | (hexval(p[1]) << 8) |
                      (hexval(p[2]) << 4) | hexval(p[3]));
}

static int b64enc(const uint8_t* in, int inLen, char* out, int outMax) {
    int i = 0, o = 0;
    while (i + 2 < inLen && o + 4 <= outMax) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = kB64[(v >> 18) & 0x3F];
        out[o++] = kB64[(v >> 12) & 0x3F];
        out[o++] = kB64[(v >> 6) & 0x3F];
        out[o++] = kB64[v & 0x3F];
        i += 3;
    }
    if (i < inLen && o + 4 <= outMax) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < inLen) v |= (uint32_t)in[i + 1] << 8;
        out[o++] = kB64[(v >> 18) & 0x3F];
        out[o++] = kB64[(v >> 12) & 0x3F];
        if (i + 1 < inLen) out[o++] = kB64[(v >> 6) & 0x3F];
        else               out[o++] = '=';
        out[o++] = '=';
    }
    if (o < outMax) out[o] = 0;
    return o;
}

// Таблица обратного декодирования (ленивая инициализация)
static uint8_t kB64D[256];
static void b64Init() {
    static bool inited = false;
    if (inited) return;
    memset(kB64D, 0xFF, sizeof(kB64D));
    for (int i = 0; i < 64; i++) kB64D[(uint8_t)kB64[i]] = (uint8_t)i;
    inited = true;
}

static int b64dec(const char* in, uint8_t* out, int outMax) {
    b64Init();
    int i = 0, o = 0;
    uint32_t v = 0;
    int bits = 0;
    while (in[i] && o < outMax) {
        uint8_t c = kB64D[(uint8_t)in[i++]];
        if (c > 63) continue;    // пропуска '=' и мусор
        v = (v << 6) | c;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)((v >> bits) & 0xFF);
        }
    }
    return o;
}

// ===================== Чексумы =====================

uint16_t meshIpChecksum(const uint8_t* data, uint16_t len) {
    uint32_t sum = 0;
    while (len > 1) { sum += ((uint32_t)data[0] << 8) | data[1]; data += 2; len -= 2; }
    if (len) sum += (uint32_t)data[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

uint16_t meshIpTcpUdpChecksum(const uint8_t* src_ip, const uint8_t* dst_ip,
                               uint8_t proto, const uint8_t* data, uint16_t len) {
    uint32_t sum = 0;
    // pseudo-header
    sum += ((uint32_t)src_ip[0] << 8) | src_ip[1];
    sum += ((uint32_t)src_ip[2] << 8) | src_ip[3];
    sum += ((uint32_t)dst_ip[0] << 8) | dst_ip[1];
    sum += ((uint32_t)dst_ip[2] << 8) | dst_ip[3];
    sum += proto;
    sum += len;
    // payload
    while (len > 1) { sum += ((uint32_t)data[0] << 8) | data[1]; data += 2; len -= 2; }
    if (len) sum += (uint32_t)data[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

// ===================== Состояние линка =====================

static String       s_linkPeer;       // имя пира (первый полученный фрагмент)
static uint32_t     s_lastRxMs = 0;   // момент последнего кадра от пира
static bool         s_linkUp   = false;
static meshIpRecvCb s_recvCb   = nullptr;

// --- Исходящая очередь (кольцо IP-датаграмм) ---
static uint8_t  s_txQueue[MESH_IP_QUEUE_MAX][MESH_IP_PKT_MAX];
static uint16_t s_txQLen[MESH_IP_QUEUE_MAX];
static uint8_t  s_txHead  = 0;
static uint8_t  s_txCount = 0;   // сколько в очереди

// --- Исходящий фрагмент-движок (один пакет в полёте) ---
static bool     s_txInFlight     = false;
static uint16_t s_txBaseSeq      = 0;
static uint8_t  s_txFragCount    = 0;
static uint16_t s_txWaitBitmap   = 0;   // какие фрагменты уже ACK'нуты
static uint16_t s_txExpectBitmap = 0;
static uint8_t  s_txRetries      = 0;
static uint32_t s_txLastMs       = 0;

// --- Исходящая нумерация (независимая от пира) ---
static uint16_t s_nextSeq = 0;

// --- Входящее окно (восстановление из фрагментов пира) ---
static uint16_t s_rxBase    = 0;     // база текущего окна
static uint16_t s_rxBitmap  = 0;     // бит i = 1 → фрагмент с seq=rxBase+i получен
static uint8_t  s_rxNf      = 0;     // nf из fi==0 (0 = ещё не знаем)
static uint8_t  s_rxHold[MESH_IP_WINDOW][MESH_IP_FRAG_MAX];
static uint8_t  s_rxHoldLen[MESH_IP_WINDOW];

// Собранный пакет, готовый к передаче в recvCb
static bool     s_rxMsgReady = false;
static uint16_t s_rxMsgLen   = 0;
static uint8_t  s_rxMsgBuf[MESH_IP_PKT_MAX];

// Статический общий буфер для base64
static char s_b64buf[MESH_IP_FRAG_MAX * 4 / 3 + 8];

// ===================== Публичные =====================

uint16_t meshIpFragCap() {
    int nameLen = (int)cfg.name.length();
    if (nameLen < 0) nameLen = 0;
    // buildGroupEnc: plaintext = 4 (ts) + 1 (type) + nameLen + 2 (": ") + tunnelMsg
    // tunnelMsg <= 240 - 5 - (nameLen + 2)
    int tunnelMax = GROUP_TEXT_MAX_PLAIN - 5 - nameLen - 2;
    // tunnelMsg = "ip:"(3) + seq3(3) + fn1(1) + base64
    int b64Max = tunnelMax - 3 - 3 - 1;
    if (b64Max < 0) b64Max = 0;
    int rawMax = (b64Max * 3) / 4;
    if (rawMax > MESH_IP_FRAG_MAX) rawMax = MESH_IP_FRAG_MAX;
    if (rawMax < 1) rawMax = 1;
    return (uint16_t)rawMax;
}

bool meshIpLinkUp() { return s_linkUp; }

void meshIpReset() {
    s_linkUp = false;
    s_linkPeer = "";
    s_txInFlight = false;
    s_txHead = s_txCount = 0;
    s_txWaitBitmap = 0;
    s_txRetries = 0;
    s_rxBitmap = 0;
    s_rxNf = 0;
    memset(s_rxHoldLen, 0, sizeof(s_rxHoldLen));
    s_rxMsgReady = false;
}

void meshIpInit() {
    meshIpReset();
    s_nextSeq = (uint16_t)(millis() & 0xFFF);  // начальный seq — псевдослучайный
    s_recvCb = nullptr;

    #if defined(COMPANION_NODE)
    meshIpApInit();
    #endif
    #if defined(MQTT_ENABLED)
    meshIpNatInit();
    #endif
}

void meshIpSetRecvCb(meshIpRecvCb cb) { s_recvCb = cb; }

// ===================== Внутренняя очередь =====================

// Потокобезопасная (portENTER_CRITICAL) обёртка
static portMUX_TYPE s_ipMux = portMUX_INITIALIZER_UNLOCKED;

static void meshIpInjectRaw(const uint8_t* pkt, uint16_t len) {
    if (s_txCount >= MESH_IP_QUEUE_MAX) return;
    if (len > MESH_IP_PKT_MAX) len = MESH_IP_PKT_MAX;
    uint8_t tail = (s_txHead + s_txCount) % MESH_IP_QUEUE_MAX;
    memcpy(s_txQueue[tail], pkt, len);
    s_txQLen[tail] = len;
    s_txCount++;
}

void meshIpInject(const uint8_t* pkt, uint16_t len) {
    portENTER_CRITICAL(&s_ipMux);
    meshIpInjectRaw(pkt, len);
    portEXIT_CRITICAL(&s_ipMux);
}

// ===================== Отправка фрагмента =====================

static void sendSensorFrame(const char* msg) {
    if (sensorChannelIdx < 0) return;
    String sMsg(msg);
    uint8_t frame[256];
    int f = buildGroupFrameFlood(sensorChannelIdx, sMsg, frame, sizeof(frame));
    if (f > 0) txFrame(frame, f);
}

static void sendFragment(const uint8_t* raw, uint8_t len, uint16_t seq, uint8_t fi, uint8_t nf) {
    int b64len = b64enc(raw, len, s_b64buf, (int)sizeof(s_b64buf) - 1);
    s_b64buf[b64len] = 0;
    char frame[1400];
    snprintf(frame, sizeof(frame), "%s%03X%01X%s", MESH_IP_PFX,
             seq & 0xFFF, ((fi & 0xF) << 4) | (nf & 0xF), s_b64buf);
    sendSensorFrame(frame);
}

static void sendAck() {
    char frame[16];
    snprintf(frame, sizeof(frame), "%s%03X%04X", MESH_IP_PFX,
             s_rxBase & 0xFFF, s_rxBitmap);
    sendSensorFrame(frame);
}

// ===================== Обработка входящего ACK =====================

static void handleAck(uint16_t ackBase, uint16_t bitmap) {
    if (!s_txInFlight) return;
    if (ackBase != s_txBaseSeq) return;
    s_txWaitBitmap |= bitmap;
    s_txLastMs = millis();
    Serial.printf("[IP] ACK base=%03X bitmap=%04X wait=%04X expect=%04X\n",
                  ackBase, bitmap, s_txWaitBitmap, s_txExpectBitmap);
    if (s_txWaitBitmap == s_txExpectBitmap) {
        s_txInFlight = false;
        s_txHead = (s_txHead + 1) % MESH_IP_QUEUE_MAX;
        s_txCount--;
        Serial.printf("[IP] TX complete, queued=%d\n", s_txCount);
    }
}

// ===================== Обработка входящего фрагмента =====================

static void handleDataFragment(uint16_t seq, uint8_t fi, uint8_t nf,
                               const uint8_t* raw, uint8_t rawLen) {
    if (nf == 0 || nf > MESH_IP_FRAGS_PER_MSG || fi >= nf) return;

    s_lastRxMs = millis();
    s_linkUp = true;

    // Пир ушёл в переполнение окна (сброс на своей стороне) → начинаем заново
    uint16_t delta = (uint16_t)((seq - s_rxBase) & 0xFFF);
    if (delta >= MESH_IP_WINDOW) {
        Serial.printf("[IP] RX window reset: seq=%03X base=%03X delta=%u\n", seq, s_rxBase, delta);
        s_rxBase = seq;
        s_rxBitmap = 0;
        s_rxNf = 0;
        memset(s_rxHoldLen, 0, sizeof(s_rxHoldLen));
        delta = 0;
    }
    uint8_t slot = (uint8_t)(delta & (MESH_IP_WINDOW - 1));

    if (rawLen > MESH_IP_FRAG_MAX) rawLen = MESH_IP_FRAG_MAX;
    memcpy(s_rxHold[slot], raw, rawLen);
    s_rxHoldLen[slot] = rawLen;
    s_rxBitmap |= (uint16_t)(1 << slot);

    if (s_rxNf == 0 || fi == 0) s_rxNf = nf;
    if (s_rxNf > MESH_IP_FRAGS_PER_MSG) s_rxNf = MESH_IP_FRAGS_PER_MSG;

    uint16_t headSeq = seq - fi;
    Serial.printf("[IP] RX frag seq=%03X fi=%d/%d raw=%d bitmap=%04X\n",
                  seq, fi, nf, rawLen, s_rxBitmap);

    if (s_rxNf != 0) {
        bool complete = true;
        for (uint8_t j = 0; j < s_rxNf; j++) {
            uint16_t d2 = (uint16_t)(((headSeq + j) - s_rxBase) & 0xFFF);
            if (d2 >= MESH_IP_WINDOW ||
                !(s_rxBitmap & (uint16_t)(1 << (d2 & (MESH_IP_WINDOW - 1))))) {
                complete = false; break;
            }
        }
        if (complete) {
            int total = 0;
            for (uint8_t j = 0; j < s_rxNf; j++) {
                uint16_t d2 = (uint16_t)(((headSeq + j) - s_rxBase) & 0xFFF);
                uint8_t k = (uint8_t)(d2 & (MESH_IP_WINDOW - 1));
                if (total + s_rxHoldLen[k] > MESH_IP_PKT_MAX) break;
                memcpy(s_rxMsgBuf + total, s_rxHold[k], s_rxHoldLen[k]);
                total += s_rxHoldLen[k];
            }
            s_rxMsgLen = (uint16_t)total;
            s_rxMsgReady = true;
            Serial.printf("[IP] RX ASSEMBLED %dB, delivering via recvCb\n", total);
            s_rxBase = (uint16_t)((headSeq + s_rxNf) & 0xFFF);
            s_rxBitmap = 0;
            s_rxNf = 0;
            memset(s_rxHoldLen, 0, sizeof(s_rxHoldLen));
        }
    }
}

// ===================== Приём текста из сенсорного канала =====================

bool meshIpOnChannelText(const String& name, const String& text) {
    if (text.length() <= 3 || !text.startsWith(MESH_IP_PFX)) return false;
    char cmd = text.charAt(3);
    if (cmd != 'd' && cmd != 'D' && cmd != 'a' && cmd != 'A') return false;

    // Игнорируем собственные фрагменты (echo自己的 flood)
    if (name == cfg.name) return true;

    s_linkPeer = name;
    s_lastRxMs = millis();
    s_linkUp = true;

    if (cmd == 'd' || cmd == 'D') {
        // Данные: "ip:d" + 3seq + 1fn + base64
        if (text.length() < 8) return false;
        const char* p = text.c_str() + 4;
        uint16_t seq = hex3(p); p += 3;
        uint8_t fn   = hex1(p); p += 1;
        uint8_t fi   = (fn >> 4) & 0x0F;
        uint8_t nf   = fn & 0x0F;
        uint8_t rawBuf[MESH_IP_FRAG_MAX];
        int rawLen = b64dec(p, rawBuf, sizeof(rawBuf));
        if (rawLen <= 0) return false;
        handleDataFragment(seq, fi, nf, rawBuf, (uint8_t)rawLen);
        sendAck();
        return true;
    }
    // ACK
    if (text.length() < 11) return false;
    const char* p = text.c_str() + 4;
    uint16_t ackBase = hex3(p); p += 3;
    uint16_t bitmap  = hex4(p);
    handleAck(ackBase, bitmap);
    return true;
}

// ===================== Сторожевой таймер =====================

void meshIpTick() {
    // Доставка собранного пакета в recvCb
    if (s_rxMsgReady && s_recvCb) {
        s_recvCb(s_rxMsgBuf, s_rxMsgLen);
        s_rxMsgReady = false;
    }

    // Во время OTAfastMode не отправляем (радио занято)
    if (otaFastMode) return;
    if (!s_linkUp) return;

    // Таймаут пира
    if (millis() - s_lastRxMs > MESH_IP_IDLE_MS) {
        s_linkUp = false;
        meshIpReset();
        return;
    }

    // Исходящий фрагмент-движок
    if (s_txInFlight) {
        // Таймаут → ретрансмиссия
        if (millis() - s_txLastMs >= MESH_IP_RTT_MS) {
            s_txRetries++;
            if (s_txRetries > MESH_IP_RETRY_MAX) {
                s_txInFlight = false;
                s_txHead = (s_txHead + 1) % MESH_IP_QUEUE_MAX;
                s_txCount--;
                Serial.printf("[IP] TX TIMEOUT (retries=%d), packet dropped\n", s_txRetries);
                return;
            }
            Serial.printf("[IP] TX RETRY %d/%d\n", s_txRetries, MESH_IP_RETRY_MAX);
            if (s_txCount > 0) {
                const uint8_t* pkt = s_txQueue[s_txHead];
                uint16_t pktLen = s_txQLen[s_txHead];
                uint16_t rawMax = meshIpFragCap();
                for (uint8_t i = 0; i < s_txFragCount; ++i) {
                    if (!(s_txWaitBitmap & (1 << i))) {
                        uint16_t off = (uint16_t)(i * rawMax);
                        uint8_t  flen = (uint8_t)((off + rawMax <= pktLen)
                                                  ? rawMax : (pktLen - off));
                        sendFragment(pkt + off, flen, s_txBaseSeq + i, i, s_txFragCount);
                    }
                }
                s_txLastMs = millis();
            }
        }
    } else if (s_txCount > 0) {
        const uint8_t* pkt = s_txQueue[s_txHead];
        uint16_t pktLen = s_txQLen[s_txHead];
        uint16_t rawMax = meshIpFragCap();
        s_txFragCount = (pktLen == 0) ? 1 : (uint8_t)((pktLen + rawMax - 1) / rawMax);
        if (s_txFragCount > MESH_IP_FRAGS_PER_MSG) s_txFragCount = MESH_IP_FRAGS_PER_MSG;
        s_txBaseSeq = s_nextSeq;
        s_nextSeq = (uint16_t)((s_nextSeq + s_txFragCount) & 0xFFF);
        s_txExpectBitmap = (uint16_t)((1U << s_txFragCount) - 1);
        s_txWaitBitmap = 0;
        s_txRetries = 0;
        s_txInFlight = true;

        Serial.printf("[IP] TX START seq=%03X frags=%d pktLen=%d rawMax=%d expect=%04X\n",
                      s_txBaseSeq, s_txFragCount, pktLen, rawMax, s_txExpectBitmap);
        for (uint8_t i = 0; i < s_txFragCount; ++i) {
            uint16_t off = (uint16_t)(i * rawMax);
            uint8_t  flen = (uint8_t)((off + rawMax <= pktLen) ? rawMax : (pktLen - off));
            sendFragment(pkt + off, flen, s_txBaseSeq + i, i, s_txFragCount);
        }
        s_txLastMs = millis();
    }
}

#endif // FEATURE_MESH_IP
