#include "config.h"
#include "globals.h"
#include "crypto.h"
#include "radio.h"
#include "mesh.h"
#include "ota.h"
#include "display.h"

// ===== Приём прошивки по радио: сторона узла =====
// Выделено из ota.cpp. Этот код не делит ни одной переменной с раздающей стороной и с
// веб-страницей, поэтому отделяется начисто и собирается только там, где нужен:
// распаковка потока на лету, запись в раздел прошивки, окно подтверждений и проверка
// образа перед применением.

#if FEATURE_MESH_OTA_RECEIVER

extern "C" {
#include "esp32s3/rom/miniz.h"
}

static uint32_t otaStreamLen = 0;        // байт в сжатом потоке
static tinfl_decompressor* otaInfl = NULL;
static uint8_t* otaDict = NULL;          // окно распаковки, оно же буфер вывода
static size_t otaDictOfs = 0;
static uint8_t otaWin[OTA_WINDOW][OTA_RAW_CHUNK_BYTES];   // слот = seq % OTA_WINDOW
static uint8_t otaWinLen[OTA_WINDOW];
static uint16_t otaWinMask = 0;          // бит i: чанк otaSeqExp+i уже в окне
static FwScan otaImgScan;                // маркер платы в распакованном образе

static void otaZFree() {
    free(otaInfl);
    otaInfl = NULL;
    free(otaDict);
    otaDict = NULL;
}

static bool otaWriteImage(const uint8_t* data, size_t n) {
    // Распаковщик может выдать больше объявленного: в конце потока он дополняет
    // последний блок. Лишнее в раздел не пишем, иначе Update.write упрётся в границу.
    if (otaGot + n > otaTotal) n = otaTotal - otaGot;
    if (n == 0) return true;
    if (Update.write((uint8_t*)data, n) != n) {
        Update.printError(Serial);
        return false;
    }
    fwScanFeed(&otaImgScan, data, n);
    otaCrcAcc = crc32_upd(otaCrcAcc, data, n);
    otaGot += n;
    return true;
}

// last = это последний чанк сжатого потока. Флаг «вход ещё будет» на нём снимается:
// с ним распаковщик не берётся за символы, которым не хватает бит в буфере, и
// придерживает хвост образа до следующей порции — которой уже не будет. Так терялись
// последние байты (на образе 1106000 доходило 1103279), и приём падал с «size mismatch».
static bool otaFeed(const uint8_t* in, size_t n, bool last) {
    for (;;) {
        size_t inBytes = n;
        size_t outBytes = TINFL_LZ_DICT_SIZE - otaDictOfs;
        tinfl_status st = tinfl_decompress(otaInfl, in, &inBytes, otaDict, otaDict + otaDictOfs, &outBytes,
                                           TINFL_FLAG_PARSE_ZLIB_HEADER | (last ? 0 : TINFL_FLAG_HAS_MORE_INPUT));
        in += inBytes;
        n -= inBytes;
        if (outBytes > 0 && !otaWriteImage(otaDict + otaDictOfs, outBytes)) return false;
        otaDictOfs = (otaDictOfs + outBytes) & (TINFL_LZ_DICT_SIZE - 1);
        if (st < 0) {
            Serial.printf("[OTA] inflate error %d\n", (int)st);
            return false;
        }
        if (st == TINFL_STATUS_DONE || (n == 0 && st != TINFL_STATUS_HAS_MORE_OUTPUT)) return true;
    }
}

static bool otaFlushWindow() {
    while (otaWinMask & 1) {
        uint8_t slot = otaSeqExp % OTA_WINDOW;
        uint32_t off = otaSeqExp * OTA_RAW_CHUNK_BYTES;
        bool last = (off + otaWinLen[slot] >= otaStreamLen);
        if (!otaFeed(otaWin[slot], otaWinLen[slot], last)) return false;
        otaWinMask >>= 1;
        otaSeqExp++;
    }
    return true;
}

static void otaSendWack() {
    uint8_t mask[2] = { (uint8_t)otaWinMask, (uint8_t)(otaWinMask >> 8) };
    uint8_t f[16];
    rawTxFrame(f, rawBuildFrame(f, RAW_TYPE_WACK, otaSeqExp, mask, 2));
}

static void otaRawFail(const char* why) {
    // Причину кладём прямо в кадр. Раньше он уходил пустым, координатор писал в журнал
    // обобщённое «sensor fail», а настоящая причина (crc mismatch, end fail, write fail)
    // оставалась в консоли узла — к которому в рабочей сети обычно не подключиться.
    uint8_t f[48];
    size_t n = strnlen(why, 20);
    rawTxFrame(f, rawBuildFrame(f, RAW_TYPE_FAIL, 0, (const uint8_t*)why, n));
    otaSensorAbort(why);
}

// Приём raw-фреймов на сенсоре (бот -> сенсор) во время чистой LoRa OTA
void otaHandleRawSensor(const uint8_t* buf, int len) {
    if (!otaActive) return;
    if (len < 9 || buf[0] != RAW_MAGIC0 || buf[1] != RAW_MAGIC1) return;
    int paylen = len - 2;
    uint16_t c = (uint16_t)(buf[len - 1] << 8) | buf[len - 2];
    if (crc16buf(buf, paylen) != c) return;
    uint8_t type = buf[2];
    uint32_t seq = (uint32_t)buf[3] | ((uint32_t)buf[4] << 8) |
                   ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 24);

    if (type == RAW_TYPE_ABORT) {
        otaSensorAbort("from bot");
        return;
    }

    if (type == RAW_TYPE_DATA || type == RAW_TYPE_DATA_LAST) {
        if (!otaGotStart) return;
        otaLastActivity = millis();
        // Фрейм: [magic 2B][type 1B][seq 4B][MAC 2B][ciphertext][crc16 2B]
        uint32_t i = seq - otaSeqExp;
        uint32_t off = seq * OTA_RAW_CHUNK_BYTES;
        int ct_len = len - 11;
        if (seq >= otaSeqExp && i < OTA_WINDOW && !(otaWinMask & (1u << i)) && off < otaStreamLen &&
            ct_len > 0 && ct_len % 16 == 0 && sensorChannelIdx >= 0) {
            uint8_t slot = seq % OTA_WINDOW;
            // шифр дополнен до 16 байт — настоящую длину чанка даёт длина потока
            uint32_t need = min((uint32_t)OTA_RAW_CHUNK_BYTES, otaStreamLen - off);
            int n = decryptRaw(channels[sensorChannelIdx].secret, &buf[7], &buf[9], ct_len,
                               otaWin[slot], OTA_RAW_CHUNK_BYTES);
            if (n >= (int)need) {
                otaWinLen[slot] = need;
                otaWinMask |= (1u << i);
            } else {
                Serial.printf("[OTA] raw decrypt FAIL seq=%u\n", seq);
            }
        }
        if (type == RAW_TYPE_DATA_LAST) {
            if (!otaFlushWindow()) { otaRawFail("write fail"); return; }
            otaSensorDraw();
            otaSendWack();
        }
        return;
    }

    if (type == RAW_TYPE_POLL) {
        if (!otaGotStart) return;
        otaLastActivity = millis();
        if (!otaFlushWindow()) { otaRawFail("write fail"); return; }
        otaSendWack();
        return;
    }

    if (type == RAW_TYPE_DONE) {
        if (otaGot != otaTotal || !otaGotStart) {
            Serial.printf("[OTA] raw DONE size mismatch got=%u total=%u\n", otaGot, otaTotal);
            otaRawFail("size mismatch");
            return;
        }
        uint32_t actCrc = ~otaCrcAcc;
        if (actCrc != otaCrcExp) {
            Serial.printf("[OTA] CRC MISMATCH exp=%08X got=%08X\n", otaCrcExp, actCrc);
            otaRawFail("crc mismatch");
            return;
        }
        // прошивка другой платы не стартует, и сенсор придётся снимать и шить по USB
        if (fwScanVerdict(&otaImgScan) < 0) {
            Serial.printf("[OTA] образ платы %s, а это " BOARD_CODE " — отказ\n", otaImgScan.other);
            otaRawFail("board mismatch");
            return;
        }
        if (!Update.end(true)) {
            Update.printError(Serial);
            otaRawFail("end fail");
            return;
        }
        otaActive = false;
        otaGotStart = false;
        Serial.println("[OTA] raw DONE, rebooting...");
        uint8_t f[16];
        rawTxFrame(f, rawBuildFrame(f, RAW_TYPE_DONE_ACK, 0, NULL, 0));
        delay(300);
        ESP.restart();
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
    // Качество связи: RSSI/SNR последнего принятого чанка
    display.setCursor(0, 54);
    char pr[12], ps[12];
    display.printf("RSSI:%s SNR:%s", fmtFix(lastRSSI, 0, pr, sizeof(pr)),
                   fmtFix(lastSNR, 0, ps, sizeof(ps)));
    display.display();
    #endif
}

void otaSensorAbort(const char* why) {
    bool wasFast = otaFastMode;
    if (otaFastMode) {
        Serial.printf("[OTA] быстрый канал: принято кадров %u, ошибок приёма %u\n",
                      (unsigned)fastRxFrames, (unsigned)fastRxErrors);
    }
    radioSetNormalConfig();
    otaFastMode = false;
    Serial.printf("[OTA] abort (%s), остаёмся на текущей прошивке\n", why);
    // Пока мы не уходили на быстрый канал, бот слушает штатный и ждёт ответа. Без
    // этого сообщения он увидит лишь таймаут и не покажет настоящую причину.
    if (!wasFast && otaGotStart && sensorChannelIdx >= 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "ota:fail:%s", why);
        sensorSendMsg(msg);
    }
    Update.abort();
    otaZFree();
    otaWinMask = 0;
    otaActive = false;
    otaGotStart = false;
    otaCrcAcc = 0xFFFFFFFF;
    otaSeqExp = 0;
    #if HAS_OLED
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("OTA ABORT");
    display.println(why);
    display.printf("rx:%u err:%u\n", (unsigned)fastRxFrames, (unsigned)fastRxErrors);
    display.display();
    #endif
}

void otaSensorTick() {
    if (!otaActive) return;
    // бот не услышал ackstart и остался на штатном конфиге — возвращаемся, чтобы принять повтор ota:start
    if (otaFastMode && otaGot == 0 && millis() - otaLastActivity > OTA_SENSOR_FIRST_CHUNK_MS) {
        otaSensorAbort("no first chunk");
        return;
    }
    if (millis() - otaLastActivity > OTA_SENSOR_STALL_MS) {
        otaSensorAbort("stall timeout");
    }
}

void otaSensorHandle() {
    String m = lastMessage;

    if (m == "ota:abort") { otaSensorAbort("from bot"); return; }

    if (m.startsWith("ota:start:")) {
        // ota:start:<target>:<размер образа>:<crc32hex>:z<размер сжатого потока>
        String rest = m.substring(10);
        int p = rest.indexOf(':');
        if (p <= 0) return;
        String target = rest.substring(0, p);
        if (target != cfg.name) return;
        rest = rest.substring(p + 1);
        int p2 = rest.indexOf(':');
        if (p2 <= 0) return;
        uint32_t total = strtoul(rest.substring(0, p2).c_str(), NULL, 10);
        String tail = rest.substring(p2 + 1);
        uint32_t crc = (uint32_t)strtoul(tail.c_str(), NULL, 16);
        int zp = tail.indexOf(":z");
        uint32_t zsize = (zp > 0) ? strtoul(tail.c_str() + zp + 2, NULL, 10) : 0;
        if (total == 0 || total > OTA_MAX_FW_BYTES || zsize == 0) return;
        // прерываем предыдущую сессию, если вдруг была
        if (otaActive && otaGotStart) Update.abort();
        otaTotal = total; otaCrcExp = crc;
        otaGot = 0; otaCrcAcc = 0xFFFFFFFF; otaSeqExp = 0;
        otaActive = true; otaGotStart = true;
        otaLastActivity = millis();
        otaZFree();
        otaStreamLen = zsize;
        otaWinMask = 0;
        fwScanReset(&otaImgScan);
        otaDictOfs = 0;
        otaInfl = (tinfl_decompressor*)malloc(sizeof(tinfl_decompressor));
        otaDict = (uint8_t*)malloc(TINFL_LZ_DICT_SIZE);
        if (!otaInfl || !otaDict) { otaSensorAbort("no RAM"); return; }
        tinfl_init(otaInfl);
        if (!Update.begin(total)) {
            Update.printError(Serial);
            otaSensorAbort("begin fail");
            return;
        }
        Serial.printf("[OTA] start %s: %u байт crc=%08X\n", cfg.name.c_str(), total, crc);
        otaSensorDraw();
        // ackstart уходит на штатном конфиге; бот после него ждёт OTA_FAST_SETTLE_MS.
        sensorSendMsg(OTA_ACKSTART, 20);
        radioSetFastConfig();
        otaFastMode = true;
        Serial.printf("[OTA] fast config: FSK %d кбит/с\n", (int)OTA_FSK_BR);
        return;
    }
}


#endif // FEATURE_MESH_OTA_RECEIVER
