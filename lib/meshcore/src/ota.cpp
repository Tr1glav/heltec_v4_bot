#include "config.h"
#include "globals.h"
#include "crypto.h"
#include "radio.h"
#include "mesh.h"
#include "ota.h"
#include "display.h"
#include <stdarg.h>
#include <stdio.h>
#include <esp_system.h>

// ==== ЧИСТЫЙ LoRa OTA: сырые фреймы на быстрой конфигурации (вне meshcore) ====

int rawBuildFrame(uint8_t* frm, uint8_t type, uint32_t seq, const uint8_t* data, int n) {
    int f = 0;
    frm[f++] = RAW_MAGIC0;
    frm[f++] = RAW_MAGIC1;
    frm[f++] = type;
    frm[f++] = (uint8_t)(seq);       frm[f++] = (uint8_t)(seq >> 8);
    frm[f++] = (uint8_t)(seq >> 16); frm[f++] = (uint8_t)(seq >> 24);
    if (data != NULL && n > 0) memcpy(frm + f, data, n), f += n;
    uint16_t c = crc16buf(frm, f);
    frm[f++] = (uint8_t)(c); frm[f++] = (uint8_t)(c >> 8);
    return f;
}

// listenAfter=false: не возвращаться в приём сразу после кадра. Внутри пачки ответа
// не ждём, а каждый возврат в RX стоит лишнего обмена по SPI.
int rawTxFrame(const uint8_t* frm, int f, bool listenAfter = true) {
    otaRawDidTx = true;
    #if HAS_FEM
    digitalWrite(FEM_TX_PIN, HIGH);
    #endif
    int st = radio.transmit((uint8_t*)frm, f);
    #if HAS_FEM
    digitalWrite(FEM_TX_PIN, LOW);
    #endif
    if (st != RADIOLIB_ERR_NONE) Serial.printf("[RAW-TX] failed %d\n", st);
    // Замер: 34.5 мс на кадр при 20 мс эфира. Лишнее — обмены по SPI, и один из них
    // это возврат в приём; внутри пачки он не нужен, следующий кадр уйдёт из standby.
    if (listenAfter && radio.startReceive() != RADIOLIB_ERR_NONE) rearmRadioAGC();
    return st;
}

// ==== Маркер платы в образе ====
// Прошивка каждой платы несёт строку FW_MARKER. И бот (HTTP /update), и сенсор
// (mesh OTA) сканируют принимаемый образ и отказываются писать чужой: неподходящая
// прошивка не запустится, а снимать её придётся USB-кабелем.
void fwScanReset(FwScan* s) {
    memset(s, 0, sizeof(*s));
}

// Поиск внутри одного непрерывного куска: после префикса читаем код платы до ':'
static void fwScanBuf(FwScan* s, const char* p, size_t n) {
    const size_t pl = sizeof(FW_MARK_PREFIX) - 1;
    for (size_t i = 0; i + pl < n; i++) {
        if (memcmp(p + i, FW_MARK_PREFIX, pl) != 0) continue;
        const char* code = p + i + pl;
        size_t avail = n - (i + pl);
        size_t c = 0;
        while (c < avail && c < sizeof(s->other) - 1 && code[c] > 0x20 && code[c] != ':') c++;
        // в образе есть и сам префикс-иголка из этого кода — он без кода платы, пропускаем
        if (c == 0 || c >= avail || code[c] != ':') continue;
        if (c == strlen(BOARD_CODE) && memcmp(code, BOARD_CODE, c) == 0) s->mine = true;
        else if (s->other[0] == 0) { memcpy(s->other, code, c); s->other[c] = 0; }
    }
}

void fwScanFeed(FwScan* s, const uint8_t* data, size_t n) {
    const size_t cap = sizeof(s->carry);
    if (n == 0) return;
    // Стык: хвост предыдущих кусков + начало текущего. Маркер короче cap, поэтому
    // любой разрыв попадает в это окно целиком.
    char joined[sizeof(s->carry) * 2];
    size_t head = min((size_t)n, cap);
    memcpy(joined, s->carry, s->carryLen);
    memcpy(joined + s->carryLen, data, head);
    size_t jl = s->carryLen + head;
    fwScanBuf(s, joined, jl);
    if (n > head) fwScanBuf(s, (const char*)data, n);   // длинный кусок смотрим целиком
    // Новый хвост — последние cap байт ПОТОКА, а не текущего куска: иначе при мелкой
    // нарезке (по байту) хвост не накапливается и маркер на стыке теряется.
    if (n >= cap) {
        memcpy(s->carry, data + n - cap, cap);
        s->carryLen = (uint8_t)cap;
    } else {
        size_t keep = min(jl, cap);
        memmove(s->carry, joined + jl - keep, keep);
        s->carryLen = (uint8_t)keep;
    }
}

int fwScanVerdict(const FwScan* s) {
    if (s->mine) return 1;
    if (s->other[0]) return -1;
    return 0;
}

#ifdef MQTT_ENABLED
static uint32_t otaImgSize = 0;       // размер прошивки после распаковки
static uint16_t otaWinAcked = 0;      // бит i — чанк otaSeq+i уже у сенсора
static unsigned long otaBurstMs = 0;  // когда ушёл последний кадр пачки
static unsigned long otaSessionMs = 0; // старт сессии — для скорости и длительности на странице
static unsigned long otaDoneMs = 0;    // когда сенсор подтвердил прошивку
static char otaLastErr[48] = "";       // причина последнего abort — показывается на странице
static String otaFwName;               // имя последнего загруженного файла — для страницы
static uint16_t otaPolls = 0;          // сколько раз пришлось переспрашивать маску за сессию
// Замер «куда уходит время»: чтение с ФС и шифрование против собственно передачи.
// Расчёт даёт 20 мс эфира на кадр 251 Б при 100 кбит/с, а по факту выходит втрое больше.
static uint32_t otaUsBuild = 0, otaUsTx = 0, otaChunksSent = 0;
static uint16_t otaRetrTotal = 0;      // сколько всего было повторов за сессию

// Для сенсора годится только .otaz (формат OTA_Z_MAGIC); otaFwSize — длина сжатого потока
void otaInspectStoredFw() {
    otaFwCrc = 0;
    otaFwSize = 0;
    File f = LittleFS.open("/ota.bin", "r");
    uint32_t sz = f ? (uint32_t)f.size() : 0;
    uint8_t hdr[OTA_Z_HDR];
    if (sz > OTA_Z_HDR && f.read(hdr, OTA_Z_HDR) == (size_t)OTA_Z_HDR && memcmp(hdr, OTA_Z_MAGIC, 4) == 0) {
        memcpy(&otaImgSize, hdr + 4, 4);
        memcpy(&otaFwCrc, hdr + 8, 4);
        otaFwSize = sz - OTA_Z_HDR;
        slog("[OTA] сжатая прошивка: %u -> %u байт\n", (unsigned)otaImgSize, (unsigned)otaFwSize);
    }
    if (f) f.close();
    otaFwReady = otaFwSize > 0;
    // имя файла переживает перезагрузку бота: сам .otaz остаётся, а имя пишется рядом
    if (otaFwReady && otaFwName.length() == 0) {
        File n = LittleFS.open("/ota.name", "r");
        if (n) {
            otaFwName = n.readStringUntil('\n');
            n.close();
        }
    }
}

bool otaSessionActive() {
    return otaPhase == OTA_PHASE_WAIT_START || otaPhase == OTA_PHASE_DATA || otaPhase == OTA_PHASE_WAIT_END;
}

void otaTxGroup(const String& msg) {
    if (sensorChannelIdx < 0) return;
    uint8_t frame[256];
    int f = buildGroupFrameFlood(sensorChannelIdx, msg, frame, sizeof(frame));
    if (f > 0) {
        sendFrame(sensorChannelIdx, frame, f);
        otaSince = millis();
    }
}

void otaBotAbort(const char* why) {
    strlcpy(otaLastErr, why, sizeof(otaLastErr));
    if (otaFastMode) {
        slog("[OTA] abort (%s): быстрый канал — принято кадров %u, ошибок приёма %u\n",
             why, (unsigned)fastRxFrames, (unsigned)fastRxErrors);
        uint8_t frame[16];
        int f = rawBuildFrame(frame, RAW_TYPE_ABORT, 0, NULL, 0);
        if (f > 0) rawTxFrame(frame, f);
        delay(100);
    } else if (otaTarget.length() > 0) {
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
    // Качество связи по последнему принятому пакету (ack/nack сенсора)
    display.setCursor(0, 54);
    char pr[12], ps[12];
    display.printf("RSSI:%s SNR:%s", fmtFix(lastRSSI, 0, pr, sizeof(pr)),
                   fmtFix(lastSNR, 0, ps, sizeof(ps)));
    display.display();
    #endif
}

void otaSendStart() {
    char msg[96];
    snprintf(msg, sizeof(msg), "ota:start:%s:%u:%08X:z%u", otaTarget.c_str(),
             (unsigned)otaImgSize, (unsigned)otaFwCrc, (unsigned)otaFwSize);
    slog("[OTA] -> %s: %s\n", otaTarget.c_str(), msg);
    otaTxGroup(msg);
}

// Кадр с чанком seq, шифр секретом канала сенсора: [MAC 2B][ciphertext]. 0 = ошибка, сессия прервана
static int otaBuildRawData(uint8_t* frame, uint8_t type, uint32_t seq) {
    uint32_t off = seq * OTA_RAW_CHUNK_BYTES;
    int n = min((int)OTA_RAW_CHUNK_BYTES, (int)(otaFwSize - off));
    uint8_t chunk[OTA_RAW_CHUNK_BYTES];
    otaFile.seek(OTA_Z_HDR + off);
    if (otaFile.read(chunk, n) != n) { otaBotAbort("read err"); return 0; }
    uint8_t enc[OTA_RAW_CHUNK_BYTES + 18];
    int enc_len = (sensorChannelIdx >= 0)
        ? encryptGroupText(channels[sensorChannelIdx].secret, enc, chunk, n)
        : 0;
    if (enc_len <= 0) { otaBotAbort("encrypt err"); return 0; }
    return rawBuildFrame(frame, type, seq, enc, enc_len);
}

static void otaLogProgress() {
    uint32_t pct = otaFwSize ? (uint64_t)otaSentBytes * 100 / otaFwSize : 0;
    Serial.printf("[OTA] seq=%u %u%% retr=%u\n", (unsigned)otaSeq, (unsigned)pct, otaRetries);
}

// Неподтверждённые чанки окна подряд; последний кадр просит у сенсора маску принятых
static void otaSendBurst() {
    uint32_t total = (otaFwSize + OTA_RAW_CHUNK_BYTES - 1) / OTA_RAW_CHUNK_BYTES;
    int last = -1;
    for (int i = 0; i < OTA_WINDOW && otaSeq + i < total; i++)
        if (!(otaWinAcked & (1u << i))) last = i;
    if (last < 0) return;
    if (otaSeq % 64 < OTA_WINDOW || otaRetries > 0) otaLogProgress();
    uint8_t frame[OTA_RAW_FRAME_MAX];
    bool first = true;
    for (int i = 0; i <= last; i++) {
        if (otaWinAcked & (1u << i)) continue;
        uint32_t t0 = micros();
        int f = otaBuildRawData(frame, i == last ? RAW_TYPE_DATA_LAST : RAW_TYPE_DATA, otaSeq + i);
        otaUsBuild += micros() - t0;
        if (f <= 0) return;
        if (!first) delay(OTA_BURST_GAP_MS);
        first = false;
        uint32_t t1 = micros();
        rawTxFrame(frame, f, i == last);   // в приём возвращаемся только после последнего
        otaUsTx += micros() - t1;
        otaChunksSent++;
    }
    otaBurstMs = otaSince = millis();
}

void otaSendEnd() {
    uint8_t frame[16];
    int f = rawBuildFrame(frame, RAW_TYPE_DONE, 0, NULL, 0);
    if (rawTxFrame(frame, f) == RADIOLIB_ERR_NONE) {
        slog("[OTA] -> raw DONE\n");
        otaSince = millis();
    }
}

void otaHandleAck() {
    // Сенсор сообщает причину отказа текстом: без этого на боте виден только таймаут
    if (lastMessage.startsWith("ota:fail:")) {
        if (otaSessionActive() && lastSender == otaTarget) {
            slog("[OTA] <- %s: отказ сенсора «%s»\n", lastSender.c_str(), lastMessage.c_str() + 9);
            otaBotAbort(lastMessage.c_str() + 9);
        }
        return;
    }
    if (!lastMessage.startsWith("ota:ackstart")) return;
    slog("[OTA] <- %s: %s (phase=%d)\n", lastSender.c_str(), lastMessage.c_str(), otaPhase);
    if (otaPhase != OTA_PHASE_WAIT_START || lastSender != otaTarget) return;
    if (lastMessage != OTA_ACKSTART) {
        otaBotAbort("старая прошивка сенсора, нужна USB");
        return;
    }
    if (!otaFile) { otaBotAbort("no file"); return; }
    otaPhase = OTA_PHASE_DATA;
    otaSeq = 0;
    otaSentBytes = 0;
    otaRetries = 0;
    otaWinAcked = 0;
    slog("[OTA] ackstart -> быстрый конфиг, пауза %dms\n", OTA_FAST_SETTLE_MS);
    radioSetFastConfig();
    otaFastMode = true;
    delay(OTA_FAST_SETTLE_MS);
    otaSendBurst();
    otaDrawProgress();
}

// Приём raw-фреймов на боте (сенсор -> бот) во время чистой LoRa OTA
void otaHandleRawBot(const uint8_t* buf, int len) {
    if (!otaFastMode) return;
    if (len < 9 || buf[0] != RAW_MAGIC0 || buf[1] != RAW_MAGIC1) return;
    int paylen = len - 2;
    uint16_t c = (uint16_t)(buf[len - 1] << 8) | buf[len - 2];
    if (crc16buf(buf, paylen) != c) return;
    uint8_t type = buf[2];
    uint32_t seq = (uint32_t)buf[3] | ((uint32_t)buf[4] << 8) |
                   ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 24);

    if (type == RAW_TYPE_WACK) {
        uint32_t totalChunks = (otaFwSize + OTA_RAW_CHUNK_BYTES - 1) / OTA_RAW_CHUNK_BYTES;
        if (otaPhase != OTA_PHASE_DATA || len < 11) return;
        if (seq < otaSeq || seq > totalChunks) return;
        uint16_t mask = (uint16_t)buf[7] | ((uint16_t)buf[8] << 8);
        bool progress = seq > otaSeq || (mask & ~otaWinAcked) != 0;
        // WACK без прогресса сразу после пачки — запоздалый ответ на прошлую, ждём ответ на эту
        if (!progress && millis() - otaBurstMs < OTA_ACK_TIMEOUT_MS) return;
        if (!progress) otaRetrTotal++;
        otaRetries = progress ? 0 : otaRetries + 1;
        if (otaRetries > OTA_MAX_RETRIES) { otaBotAbort("no progress"); return; }
        otaSeq = seq;
        otaWinAcked = mask;
        uint32_t off = seq * OTA_RAW_CHUNK_BYTES;
        otaSentBytes = min(otaFwSize, off);
        otaDrawProgress();
        if (otaSentBytes >= otaFwSize) {
            otaPhase = OTA_PHASE_WAIT_END;
            slog("[OTA] все байты подтверждены, ждём финал\n");
            otaSendEnd();
        } else {
            otaSendBurst();
        }
        return;
    }
    if (type == RAW_TYPE_DONE_ACK) {
        if (otaPhase != OTA_PHASE_WAIT_END) return;
        otaPhase = OTA_PHASE_DONE;
        otaDoneMs = millis();
        slog("[OTA] DONE: сенсор %s применил прошивку, CRC32 OK (кадров %u, ошибок приёма %u)\n",
             otaTarget.c_str(), (unsigned)fastRxFrames, (unsigned)fastRxErrors);
        if (otaChunksSent > 0) {
            unsigned long gapMs = (unsigned long)otaChunksSent * OTA_BURST_GAP_MS;
            slog("[OTA] тайминг: кадров %lu | чтение+шифр %lu мс (%lu мкс/кадр) | "
                 "передача %lu мс (%lu мкс/кадр) | паузы %lu мс\n",
                 (unsigned long)otaChunksSent,
                 (unsigned long)(otaUsBuild / 1000), (unsigned long)(otaUsBuild / otaChunksSent),
                 (unsigned long)(otaUsTx / 1000), (unsigned long)(otaUsTx / otaChunksSent),
                 gapMs);
        }
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
    if (type == RAW_TYPE_FAIL) {
        otaBotAbort("sensor fail");
        return;
    }
}

void otaBotTick() {
    if (otaPhase != OTA_PHASE_WAIT_START &&
        otaPhase != OTA_PHASE_DATA &&
        otaPhase != OTA_PHASE_WAIT_END) return;
    unsigned long timeout = (otaPhase == OTA_PHASE_WAIT_START) ? OTA_START_TIMEOUT_MS
                          : (otaPhase == OTA_PHASE_WAIT_END)   ? OTA_END_TIMEOUT_MS
                          : OTA_ACK_TIMEOUT_MS;
    if (millis() - otaSince < timeout) return;

    otaRetries++;
    otaRetrTotal++;
    if (otaRetries > OTA_MAX_RETRIES) {
        char why[24];
        snprintf(why, sizeof(why), "timeout p%d", otaPhase);
        otaBotAbort(why);
        return;
    }
    if (otaPhase == OTA_PHASE_WAIT_START) otaSendStart();
    else if (otaPhase == OTA_PHASE_DATA) {
        otaPolls++;
        slog("[OTA] poll seq=%u\n", (unsigned)otaSeq);
        uint8_t frame[16];
        int f = rawBuildFrame(frame, RAW_TYPE_POLL, otaSeq, NULL, 0);
        if (rawTxFrame(frame, f) == RADIOLIB_ERR_NONE) otaSince = millis();
    }
    else if (otaPhase == OTA_PHASE_WAIT_END) otaSendEnd();
    otaDrawProgress();
}

// Сколько байт лога записано с загрузки — позиция для живого вывода на странице (/logs/tail)
static uint32_t logTotal = 0;

void slog(const char* fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    size_t len = min((size_t)n, sizeof(tmp) - 1);   // vsnprintf возвращает длину без учёта обрезки
    Serial.write((const uint8_t*)tmp, len);
    logTail += tmp;
    logTotal += len;
    // logTail — точный хвост потока лога без вставок, иначе позиции /logs/tail разъедутся
    if (logTail.length() > LOG_TAIL_MAX) logTail.remove(0, logTail.length() - LOG_TAIL_MAX);
}

static const char* resetReasonStr() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "power on";
        case ESP_RST_SW:        return "software restart";
        case ESP_RST_PANIC:     return "PANIC (падение)";
        case ESP_RST_INT_WDT:   return "interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "task watchdog";
        case ESP_RST_WDT:       return "other watchdog";
        case ESP_RST_BROWNOUT:  return "BROWNOUT (просадка питания)";
        case ESP_RST_EXT:       return "external reset";
        case ESP_RST_DEEPSLEEP: return "deep sleep";
        default:                return "unknown";
    }
}

String buildDiagReport() {
    String r;
    r.reserve(3072);
    r += "===== MESHCORE BOT DIAG =====\r\n";
    r += "uptime: " + String((unsigned long)(millis() / 1000)) + " s\r\n";
    r += "firmware: v" FW_VERSION "\r\n";
    r += "reset reason: " + String(resetReasonStr()) + "\r\n";
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
    if (logTotal > logTail.length()) r += "…(начало обрезано)…\r\n";
    r += logTail;
    return r;
}

// Проверка LittleFS по запросу. Раньше шла внутри /logs и писала во flash при каждом
// открытии страницы — теперь только когда её явно попросили.
void otaHandleSelfTest() {
    String r = "LittleFS: ";
    File t = LittleFS.open("/.probe", "w");
    if (!t) {
        r += "open(w) FAILED — файловая система не работает";
    } else {
        int w = (int)t.write((const uint8_t*)"probe", 5);
        t.close();
        if (w != 5) {
            r += "write FAILED";
        } else {
            File t2 = LittleFS.open("/.probe", "r");
            if (!t2) {
                r += "open(r) FAILED";
            } else {
                char b[8] = {0};
                int rd = (int)t2.read((uint8_t*)b, 5);
                t2.close();
                r += "OK, прочитано \"" + String(b) + "\" (" + String(rd) + " Б)";
            }
            LittleFS.remove("/.probe");
        }
    }
    slog("[WEB] selftest: %s\n", r.c_str());
    otaServer.send(200, "text/plain; charset=utf-8", r + "\r\n");
}

// Настройки бота: тот же набор полей, что и в консоли. Секреты отдаются маской,
// поэтому страница не может их показать — только перезаписать.
void otaHandleConfigGet() {
    String json = "[";
    for (int i = 0; i < cfgFieldCount(); i++) {
        char name[24], val[96], item[200];
        jsonEscape(cfgFieldName(i), name, sizeof(name));
        jsonEscape(cfgFieldValue(i, false).c_str(), val, sizeof(val));
        snprintf(item, sizeof(item), "%s{\"f\":\"%s\",\"v\":\"%s\",\"s\":%s}",
                 i ? "," : "", name, val, cfgFieldSecret(i) ? "true" : "false");
        json += item;
    }
    json += "]";
    otaServer.send(200, "application/json", json);
}

void otaHandleConfigPost() {
    if (otaSessionActive()) {
        otaServer.send(409, "text/plain; charset=utf-8", "идёт прошивка сенсора");
        return;
    }
    int changed = 0;
    String unknown;
    for (int i = 0; i < otaServer.args(); i++) {
        String n = otaServer.argName(i);
        if (n == "reboot" || n == "plain") continue;
        if (cfgApply(n, otaServer.arg(i))) { changed++; slog("[WEB] настройка %s изменена\n", n.c_str()); }
        else unknown += " " + n;
    }
    if (unknown.length() > 0) {
        otaServer.send(400, "text/plain; charset=utf-8", "неизвестные поля:" + unknown);
        return;
    }
    if (changed == 0) {
        otaServer.send(200, "text/plain; charset=utf-8", "нечего менять");
        return;
    }
    cfgSave();
    bool reboot = otaServer.arg("reboot") == "1";
    otaServer.send(200, "text/plain; charset=utf-8",
                   String("OK, изменено полей: ") + changed + (reboot ? ", перезагрузка" : ""));
    if (reboot) {
        delay(300);
        ESP.restart();
    }
}

// Состояние бота для шапки страницы
void otaHandleInfo() {
    char fwname[64];
    jsonEscape(otaFwName.c_str(), fwname, sizeof(fwname));
    char json[384];
    snprintf(json, sizeof(json),
             "{\"up\":%lu,\"wifi\":%s,\"mqtt\":%s,\"heap\":%u,\"temp\":%.1f,"
             "\"bat\":%d,\"volt\":%.2f,\"ip\":\"%s\",\"pkts\":%d,"
             "\"env\":\"" FW_ENV "\",\"ver\":\"" FW_VERSION "\","
             "\"fwready\":%s,\"fwname\":\"%s\",\"fwsize\":%u,\"fwimg\":%u}",
             (unsigned long)(millis() / 1000),
             wifiConnected ? "true" : "false", mqttConnected ? "true" : "false",
             (unsigned)ESP.getFreeHeap(), cpuTempC(),
             batteryPercent(), batteryVoltage(),
             wifiConnected ? WiFi.localIP().toString().c_str() : "-", packetCount,
             otaFwReady ? "true" : "false", fwname,
             (unsigned)otaFwSize, (unsigned)otaImgSize);
    otaServer.send(200, "application/json", json);
}

// Живой вывод лога: текст, записанный после позиции from (счётчик logTotal)
void otaHandleLogTail() {
    uint32_t from = strtoul(otaServer.arg("from").c_str(), NULL, 10);
    uint32_t tailStart = logTotal - logTail.length();
    String out;
    if (from > logTotal) from = tailStart;   // бот перезагрузился — отдаём весь хвост
    if (from < tailStart) {
        out = "…(пропущено)…\n";
        from = tailStart;
    }
    out += logTail.substring(from - tailStart);
    otaServer.sendHeader("X-Log-Pos", String(logTotal));
    otaServer.send(200, "text/plain; charset=utf-8", out);
}

void otaHandleSensors() {
    String json = "[";
    for (int i = 0; i < sensorDeviceDiscCount; i++) {
        // Имя окружения показываем вместо кода платы: в нём уже есть и плата, и тип
        // прошивки, а по нему же автообновление выбирает файл релиза. Пустое значение —
        // прошивка узла старая, и файл подберётся по коду платы (он остаётся в hello).
        char name[48], ver[32], env[40], item[300];
        jsonEscape(sensorDeviceDisc[i].c_str(), name, sizeof(name));
        jsonEscape(sensorFwVersion[i].c_str(), ver, sizeof(ver));
        jsonEscape(sensorEnv[i].c_str(), env, sizeof(env));
        snprintf(item, sizeof(item),
                 "%s{\"name\":\"%s\",\"ver\":\"%s\",\"env\":\"%s\","
                 "\"online\":%s,\"seen_s\":%lu,\"bat\":%d,\"rssi\":%.0f}",
                 i ? "," : "", name, ver, env, sensorOnlineNow[i] ? "true" : "false",
                 (millis() - sensorLastActive[i]) / 1000, sensorBattery[i], sensorRssi[i]);
        json += item;
    }
    json += "]";
    otaServer.send(200, "application/json", json);
}

// Настройка сенсора по радио. Поля уходят по одному с большой паузой: на каждое
// сенсор отвечает подтверждением, а оно шлётся тройным флудом и занимает эфир около
// полутора секунд. Радио полудуплексное — пока сенсор передаёт, он не слышит ничего,
// поэтому следующая команда, посланная раньше, просто пропадёт.
#define CFG_MSG_GAP_MS 2000
void otaHandleSensorsConfig() {
    if (otaSessionActive()) { otaServer.send(409, "text/plain; charset=utf-8", "идёт прошивка сенсора"); return; }
    if (sensorChannelIdx < 0) { otaServer.send(503, "text/plain; charset=utf-8", "канал сенсоров не настроен"); return; }
    String target = otaServer.arg("target");
    if (target.length() == 0 || target.length() > 31) {
        otaServer.send(400, "text/plain; charset=utf-8", "не указан сенсор");
        return;
    }
    if (otaServer.arg("get") == "1") {
        slog("[CFG] -> %s: get\n", target.c_str());
        otaTxGroup("cfg:" + target + ":get");
        otaServer.send(200, "text/plain; charset=utf-8", "запрошены настройки, ответ в журнале");
        return;
    }
    int sent = 0;
    for (int i = 0; i < otaServer.args(); i++) {
        String n = otaServer.argName(i);
        if (n == "target" || n == "save" || n == "reboot" || n == "get" || n == "plain") continue;
        String msg = "cfg:" + target + ":" + n + "=" + otaServer.arg(i);
        slog("[CFG] -> %s: %s\n", target.c_str(), n.c_str());
        otaTxGroup(msg);
        sent++;
        delay(CFG_MSG_GAP_MS);
    }
    if (otaServer.arg("save") == "1") {
        slog("[CFG] -> %s: save\n", target.c_str());
        otaTxGroup("cfg:" + target + ":save");
        delay(CFG_MSG_GAP_MS);
    }
    if (otaServer.arg("reboot") == "1") {
        slog("[CFG] -> %s: reboot\n", target.c_str());
        otaTxGroup("cfg:" + target + ":reboot");
    }
    otaServer.send(200, "text/plain; charset=utf-8",
                   String("отправлено полей: ") + sent + ", ответы смотрите в журнале");
}

void otaHandleSensorsHello() {
    if (otaSessionActive()) { otaServer.send(409, "text/plain", "идёт прошивка сенсора"); return; }
    if (sensorChannelIdx < 0) { otaServer.send(503, "text/plain", "канал сенсоров не настроен"); return; }
    slog("[WEB] опрос сенсоров (%s)\n", SENSOR_MSG_HELLO_REQ);
    sensorSendMsg(SENSOR_MSG_HELLO_REQ);
    otaServer.send(200, "text/plain", "sent");
}

// CSS и JS лежат в web/ и попадают в прошивку уже сжатыми (scripts/gen_web.py собирает
// web_assets.h). Браузер распаковывает сам, а во флеше они занимают вчетверо меньше.
// HTML остаётся в коде: он маленький и требует подстановки имени и версии.
#include "web_assets.h"


static const char PAGE_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang='ru'><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>MeshBot OTA</title><link rel='stylesheet' href='/style.css?v=__VER__'></head><body>
<div class='wrap'>
<section class='card'>
<div class='hdr'><h2>MeshBot OTA</h2><span id='dev'>__NAME__</span></div>
<div id='info' class='info'>…</div>
<label>Куда прошиваем</label>
<div id='targets'></div>
<div class='row'><button id='ask' class='sec sm grow'>Опросить сенсоры</button></div>
<details class='cfg' id='scfgbox' hidden><summary>Настройки сенсора по радио</summary>
<div id='scfg'></div>
<div class='cfghint'>Заполняйте только те поля, которые меняете. Сенсор применяет их в памяти,
в NVS они попадут лишь при отметке «сохранить» — до этого всё чинится снятием питания.
Смена имени, ключа канала или параметров радио уводит сенсор из сети: он вернётся, только
если настройки совпадут с ботом. Ответы сенсора видны в журнале справа.</div>
<label class='chk'><input type='checkbox' id='scfgsave' checked> сохранить в NVS</label>
<label class='chk'><input type='checkbox' id='scfgreboot'> перезагрузить после сохранения</label>
<div class='row'><button id='scfgget' class='sec sm grow'>Запросить текущие</button>
<button id='scfgsend' class='sec sm grow'>Отправить</button></div>
</details>
<div id='drop'>&#128190; <span id='hint'></span><div id='fname'></div><div id='fver'></div></div>
<input id='file' type='file' hidden>
<div id='fw' class='fw' hidden></div>
<button id='go' disabled>Начать обновление</button>
<div id='prog' hidden>
<div class='pct'><span id='pctv'>0</span><small>%</small></div>
<div class='w'><div id='fill'></div></div>
<div class='t'><span id='pl'></span><span id='pr'></span></div>
<button id='ab' class='sec' hidden>Прервать</button>
</div>
<div id='st'></div>
<details class='cfg'><summary>Настройки бота</summary>
<div id='cfg'></div>
<div class='cfghint'>Поле с подсказкой «задано» — пароль или ключ: оставьте пустым, чтобы не менять.
Параметры радио должны совпадать у всех узлов сети. После сохранения бот перезагрузится.</div>
<button id='cfgsave' class='sec sm' style='margin-top:8px;width:100%'>Сохранить и перезагрузить</button>
</details>
<div class='ft'>MeshBot v__VER__ · <a href='/selftest' target='_blank'>проверить LittleFS</a></div>
</section>
<section class='card'>
<div class='tools'><input id='q' placeholder='фильтр по тексту'><button id='dl' class='sec sm'>Скачать</button><button id='clr' class='sec sm'>Очистить</button></div>
<div class='logwrap'><pre id='logs'>загрузка…</pre><button id='down' class='jump' hidden>&#8595; новые строки</button></div>
</section>
</div>
<script src='/app.js?v=__VER__'></script>
</body></html>)HTML";

// Статика отдаётся потоком из флеша, без копии в куче и без распаковки на боте:
// заголовок Content-Encoding говорит браузеру распаковать самому.
static void sendGz(const char* type, const uint8_t* data, size_t len) {
    otaServer.sendHeader("Content-Encoding", "gzip");
    otaServer.sendHeader("Cache-Control", "public, max-age=31536000, immutable");
    otaServer.setContentLength(len);
    otaServer.send(200, type, "");
    otaServer.sendContent_P((PGM_P)data, len);
}

void otaHandleCss() { sendGz("text/css; charset=utf-8", WEB_STYLE_CSS_GZ, WEB_STYLE_CSS_GZ_LEN); }
void otaHandleJs()  { sendGz("application/javascript; charset=utf-8", WEB_APP_JS_GZ, WEB_APP_JS_GZ_LEN); }

void otaHandleRoot() {
    String page = FPSTR(PAGE_HTML);
    page.replace("__NAME__", cfg.name);
    page.replace("__VER__", FW_VERSION);
    // сам HTML не кэшируем: он ссылается на css/js с версией в адресе,
    // и после обновления бота страница должна прийти заново
    otaServer.sendHeader("Cache-Control", "no-cache");
    otaServer.sendHeader("Connection", "close");
    otaServer.send(200, "text/html", page);
}

// После неудачной самопрошивки возвращаем радио и усилитель, иначе бот оглохнет до перезагрузки
static void otaSelfUpdateResume() {
    #if HAS_FEM
    digitalWrite(FEM_EN_PIN, HIGH);
    #endif
    radio.startReceive();
    isListening = true;
}

// Маркер платы в принимаемом образе и причина отказа для ответа странице
static FwScan otaSelfScan;
static char otaSelfErr[64] = "";

void otaHandleUpdate() {
    HTTPUpload& up = otaServer.upload();
    switch (up.status) {
    case UPLOAD_FILE_START:
        Serial.printf("\n[OTA] загрузка: %s\n", up.filename.c_str());
        fwScanReset(&otaSelfScan);
        otaSelfErr[0] = 0;
        #if HAS_OLED
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("OTA update...");
        display.display();
        #endif
        // на время записи радио и сеть выключены
        radio.sleep();
        isListening = false;
        if (mqttConnected) mqtt.disconnect();
        #if HAS_FEM
        digitalWrite(FEM_EN_PIN, LOW);
        #endif
        // totalSize на START ещё 0 (Arduino core) — размер ограничит сам раздел OTA
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
        break;
    case UPLOAD_FILE_WRITE:
        if (Update.isRunning()) {
            fwScanFeed(&otaSelfScan, up.buf, up.currentSize);
            if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
        }
        break;
    case UPLOAD_FILE_END:
    {
        // Образ чужой платы не запустится, а снимать его придётся USB-кабелем,
        // поэтому до применения прошивки проверяем маркер.
        int verdict = fwScanVerdict(&otaSelfScan);
        if (verdict < 0) {
            snprintf(otaSelfErr, sizeof(otaSelfErr), "образ платы %s, а это " BOARD_CODE,
                     otaSelfScan.other);
            slog("[OTA] отклонено: %s\n", otaSelfErr);
            Update.abort();
            #if HAS_OLED
            display.println("WRONG BOARD!");
            display.display();
            #endif
            otaSelfUpdateResume();
            break;
        }
        if (verdict == 0) slog("[OTA] в образе нет маркера платы — прошиваем как есть\n");
        if (Update.end(true)) {
            Serial.printf("[OTA] OK, %u bytes, reboot...\n", (unsigned)up.totalSize);
            #if HAS_OLED
            display.println("OK! Rebooting...");
            display.display();
            #endif
            otaServer.send(200, "text/plain", "OK rebooting");
            delay(300);
            ESP.restart();
        }
        Update.printError(Serial);
        #if HAS_OLED
        display.println("OTA FAILED!");
        display.display();
        #endif
        otaSelfUpdateResume();
        break;
    }
    case UPLOAD_FILE_ABORTED:
        Update.abort();
        otaSelfUpdateResume();
        Serial.println("[OTA] прервано");
        break;
    default:
        break;
    }
}

static bool otaSaveTooBig = false;

void otaHandleSaveFw() {
    HTTPUpload& up = otaServer.upload();
    switch (up.status) {
    case UPLOAD_FILE_START:
    {
        if (otaPhase != OTA_PHASE_IDLE && otaPhase != OTA_PHASE_DONE) otaBotAbort("новый файл");
        otaSaving = true;
        otaSaveOk = false;
        otaSaveTooBig = false;
        otaWriteCalls = 0;
        otaWriteBytes = 0;
        otaWriteSkipped = 0;
        // закрываем любые остатки прошлой сессии, иначе на /ota.bin висит чужой handle
        if (otaFile) { otaFile.close(); otaFile = File(); }
        otaFwName = up.filename;
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
        if (otaSaving && otaFile && otaWriteBytes + up.currentSize > OTA_MAX_FW_BYTES) {
            slog("[OTA-SAVE] файл больше лимита %lu байт, отменяем\n", (unsigned long)OTA_MAX_FW_BYTES);
            otaFile.close();
            otaFile = File();
            LittleFS.remove("/ota.bin");
            otaSaving = false;
            otaSaveTooBig = true;
            break;
        }
        if (otaSaving && otaFile) {
            size_t w = otaFile.write(up.buf, up.currentSize);
            otaWriteBytes += w;
            if (w != up.currentSize) slog("[OTA-SAVE] write short (%u/%u)\n",
                                          (unsigned)w, (unsigned)up.currentSize);
        } else {
            otaWriteSkipped++;
            if (!otaSaveTooBig) slog("[OTA-SAVE] WRITE skipped (saving=%d, file=%d)\n",
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
                // Переоткрываем для проверки (flush при close записал на диск)
                otaInspectStoredFw();
                otaSaveOk = true;
                File n = LittleFS.open("/ota.name", "w");
                if (n) {
                    n.print(otaFwName);
                    n.close();
                }
                slog("[OTA-SAVE] записано %lu байт, .otaz=%d\n", otaWriteBytes, (int)otaFwReady);
                if (otaFwReady && otaFwSize + OTA_Z_HDR != (uint32_t)otaWriteBytes) {
                    slog("[OTA-SAVE] ВНИМАНИЕ: size()=%u != writeBytes=%lu\n",
                         (unsigned)(otaFwSize + OTA_Z_HDR), otaWriteBytes);
                }
            }
        } else {
            otaSaving = false;
            otaFwReady = false;
            otaSaveOk = false;
            slog("[OTA-SAVE] FAIL — %s\n", otaSaveTooBig ? "файл больше лимита" : "file not open");
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

// Общий запуск сессии: используется и веб-обработчиком, и автообновлением
bool otaStartSession(const String& target) {
    if (otaPhase != OTA_PHASE_IDLE && otaPhase != OTA_PHASE_DONE) return false;
    if (!otaFwReady || target.length() == 0 || target.length() > 31) return false;
    otaFile = LittleFS.open("/ota.bin", "r");
    if (!otaFile) { slog("[OTA] /ota.bin не открылся\n"); return false; }
    otaTarget = target;
    otaLastErr[0] = 0;
    otaSessionMs = millis();
    otaPolls = 0;
    otaRetrTotal = 0;
    otaUsBuild = otaUsTx = otaChunksSent = 0;
    otaPhase = OTA_PHASE_WAIT_START;
    otaSeq = 0;
    otaSentBytes = 0;
    otaRetries = 0;
    slog("[OTA] старт -> '%s' (%u байт, crc=%08X)\n",
         otaTarget.c_str(), (unsigned)otaFwSize, (unsigned)otaFwCrc);
    otaSendStart();
    otaDrawProgress();
    return true;
}

void otaHandleStartOta() {
    String target = otaServer.arg("target");
    if (otaPhase != OTA_PHASE_IDLE && otaPhase != OTA_PHASE_DONE) {
        slog("[WEB] /ota/start busy (phase=%d)\n", otaPhase);
        otaServer.send(409, "text/plain", "busy");
        return;
    }
    if (!otaFwReady) {
        slog("[WEB] /ota/start: нет .otaz на боте\n");
        otaServer.send(400, "text/plain", "нет .otaz на боте");
        return;
    }
    if (target.length() == 0 || target.length() > 31) {
        slog("[WEB] /ota/start bad target: '%s'\n", target.c_str());
        otaServer.send(400, "text/plain", "bad target");
        return;
    }
    otaFile = LittleFS.open("/ota.bin", "r");
    if (!otaFile) { slog("[WEB] /ota/start: fs open fail\n"); otaServer.send(500, "text/plain", "fs open fail"); return; }
    otaTarget = target;
    otaLastErr[0] = 0;
    otaSessionMs = millis();
    otaPolls = 0;
    otaRetrTotal = 0;
    otaUsBuild = otaUsTx = otaChunksSent = 0;
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
    int idx = -1;
    for (int i = 0; i < sensorDeviceDiscCount; i++) {
        if (sensorDeviceDisc[i] == otaTarget) { idx = i; break; }
    }
    char tgt[48], err[64], ver[32];
    jsonEscape(otaTarget.c_str(), tgt, sizeof(tgt));
    jsonEscape(otaLastErr, err, sizeof(err));
    jsonEscape(idx >= 0 ? sensorFwVersion[idx].c_str() : "", ver, sizeof(ver));
    unsigned long endMs = (otaPhase == OTA_PHASE_DONE) ? otaDoneMs : millis();
    // back: сенсор прислал hello уже после подтверждения прошивки — значит, загрузился с неё
    bool back = otaPhase == OTA_PHASE_DONE && idx >= 0 && sensorLastActive[idx] > otaDoneMs;
    char json[384];
    snprintf(json, sizeof(json),
             "{\"phase\":%u,\"fw\":%s,\"sent\":%u,\"total\":%u,\"elapsed_ms\":%lu,\"retr\":%u,"
             "\"polls\":%u,\"retrs\":%u,"
             "\"err\":\"%s\",\"target\":\"%s\",\"ver\":\"%s\",\"back\":%s}",
             (unsigned)otaPhase, otaFwReady ? "true" : "false",
             (unsigned)otaSentBytes, (unsigned)otaFwSize,
             otaSessionMs ? endMs - otaSessionMs : 0UL, (unsigned)otaRetries,
             (unsigned)otaPolls, (unsigned)otaRetrTotal,
             err, tgt, ver, back ? "true" : "false");
    otaServer.send(200, "application/json", json);
}

void setupOtaServer() {
    otaServer.on("/", HTTP_GET, otaHandleRoot);
    otaServer.on("/update", HTTP_POST, []() {
        otaServer.sendHeader("Connection", "close");
        if (otaSelfErr[0]) otaServer.send(200, "text/plain", String("FAIL: ") + otaSelfErr);
        else otaServer.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
    }, otaHandleUpdate);
    otaServer.on("/savefw", HTTP_POST, []() {
        otaServer.sendHeader("Connection", "close");
        if (otaSaveOk && otaFwReady) {
            slog("[WEB] /savefw -> OK (%u байт)\n", (unsigned)otaFwSize);
            otaServer.send(200, "text/plain", "OK");
        } else {
            slog("[WEB] /savefw -> FAIL (saveOk=%d fwSize=%u)\n",
                 (int)otaSaveOk, (unsigned)otaFwSize);
            otaServer.send(200, "text/plain",
                           otaSaveTooBig ? "FAIL: файл больше 3 МБ" :
                           otaSaveOk ? "FAIL: для сенсора нужен .otaz" : "FAIL: файл не открылся (LittleFS)");
        }
    }, otaHandleSaveFw);
    otaServer.on("/ota/start", HTTP_POST, otaHandleStartOta);
    otaServer.on("/ota/abort", HTTP_POST, otaHandleAbort);
    otaServer.on("/ota/status", HTTP_GET, otaHandleStatus);
    otaServer.on("/sensors", HTTP_GET, otaHandleSensors);
    otaServer.on("/sensors/hello", HTTP_POST, otaHandleSensorsHello);
    otaServer.on("/sensors/config", HTTP_POST, otaHandleSensorsConfig);
    otaServer.on("/logs", HTTP_GET, []() {
        otaServer.sendHeader("X-Log-Pos", String(logTotal));
        otaServer.send(200, "text/plain; charset=utf-8", buildDiagReport());
    });
    otaServer.on("/logs/tail", HTTP_GET, otaHandleLogTail);
    otaServer.on("/info", HTTP_GET, otaHandleInfo);
    otaServer.on("/selftest", HTTP_GET, otaHandleSelfTest);
    otaServer.on("/config", HTTP_GET, otaHandleConfigGet);
    otaServer.on("/config", HTTP_POST, otaHandleConfigPost);
    otaServer.on("/style.css", HTTP_GET, otaHandleCss);
    otaServer.on("/app.js", HTTP_GET, otaHandleJs);
    otaServer.begin();
    Serial.println("OTA server: http://<ip>:3232/update | /ota/start");
}

#endif // MQTT_ENABLED

#ifdef SENSOR_NODE
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
    if (Update.write((uint8_t*)data, n) != n) {
        Update.printError(Serial);
        return false;
    }
    fwScanFeed(&otaImgScan, data, n);
    otaCrcAcc = crc32_upd(otaCrcAcc, data, n);
    otaGot += n;
    return true;
}

static bool otaFeed(const uint8_t* in, size_t n) {
    for (;;) {
        size_t inBytes = n;
        size_t outBytes = TINFL_LZ_DICT_SIZE - otaDictOfs;
        tinfl_status st = tinfl_decompress(otaInfl, in, &inBytes, otaDict, otaDict + otaDictOfs, &outBytes,
                                           TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT);
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
        if (!otaFeed(otaWin[slot], otaWinLen[slot])) return false;
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
    uint8_t f[16];
    rawTxFrame(f, rawBuildFrame(f, RAW_TYPE_FAIL, 0, NULL, 0));
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

#endif // SENSOR_NODE

// Диспетчер: main loop зовёт это, когда поймал raw-фрейм (магия 0xBE 0xEF)
// в fast-режиме.
void otaHandleRawFrame(const uint8_t* buf, int len) {
    #ifdef MQTT_ENABLED
    otaHandleRawBot(buf, len);
    #elif defined(SENSOR_NODE)
    otaHandleRawSensor(buf, len);
    #endif
}