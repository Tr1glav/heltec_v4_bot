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

int rawTxFrame(const uint8_t* frm, int f) {
    otaRawDidTx = true;
    #if HAS_FEM
    digitalWrite(FEM_TX_PIN, HIGH);
    #endif
    int st = radio.transmit((uint8_t*)frm, f);
    #if HAS_FEM
    digitalWrite(FEM_TX_PIN, LOW);
    #endif
    if (st != RADIOLIB_ERR_NONE) Serial.printf("[RAW-TX] failed %d\n", st);
    if (radio.startReceive() != RADIOLIB_ERR_NONE) rearmRadioAGC();
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
    display.printf("RSSI:%.0f SNR:%.0f", lastRSSI, lastSNR);
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
        int f = otaBuildRawData(frame, i == last ? RAW_TYPE_DATA_LAST : RAW_TYPE_DATA, otaSeq + i);
        if (f <= 0) return;
        if (!first) delay(OTA_BURST_GAP_MS);
        first = false;
        rawTxFrame(frame, f);
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
             "\"board\":\"" BOARD_CODE "\",\"ver\":\"" FW_VERSION "\","
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
        char name[48], ver[32], board[16], item[240];
        jsonEscape(sensorDeviceDisc[i].c_str(), name, sizeof(name));
        jsonEscape(sensorFwVersion[i].c_str(), ver, sizeof(ver));
        jsonEscape(sensorBoard[i].c_str(), board, sizeof(board));
        snprintf(item, sizeof(item),
                 "%s{\"name\":\"%s\",\"ver\":\"%s\",\"board\":\"%s\",\"online\":%s,"
                 "\"seen_s\":%lu,\"bat\":%d,\"rssi\":%.0f}",
                 i ? "," : "", name, ver, board, sensorOnlineNow[i] ? "true" : "false",
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

// CSS и JS отдаются отдельными адресами и кэшируются браузером: HTML в памяти бота
// собирается маленьким, а повторные открытия страницы тянут только его. Версия в адресе
// (?v=) сбрасывает кэш при обновлении прошивки.
static const char PAGE_CSS[] PROGMEM = R"CSS(
*{box-sizing:border-box}
body{font-family:system-ui,sans-serif;background:#1c2333;color:#e8ecf3;margin:0;padding:10px;display:flex;justify-content:center}
.wrap{display:grid;grid-template-columns:minmax(320px,390px) 1fr;gap:10px;width:100%;max-width:1220px;align-items:start}
.card{background:#242d40;border:1px solid #35405a;border-radius:10px;padding:12px}
.hdr{display:flex;justify-content:space-between;align-items:baseline;gap:8px}
h2{margin:0;font-size:16px}#dev{color:#93a4c4;font-size:12px}
.info{margin:6px 0 10px;font-size:11px;color:#93a4c4;line-height:1.6}
label{display:block;font-size:11px;color:#93a4c4;margin:0 0 4px}
.row{display:flex;gap:6px;margin-top:6px}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#f87171;margin-right:3px;vertical-align:middle}
.dot.on{background:#34d399}
.bat{display:inline-block;width:26px;height:9px;border:1px solid #4a5775;border-radius:2px;vertical-align:middle;overflow:hidden}
.bat i{display:block;height:100%;background:#34d399}
.bat.low i{background:#f87171}
.tgt{display:flex;gap:8px;align-items:center;padding:7px 8px;border:1px solid #35405a;border-radius:8px;background:#1e2638;cursor:pointer;margin-bottom:6px}
.tgt:hover{border-color:#4a5775}
.tgt.sel{border-color:#0ea5e9;background:#1b2b3d}
.tgt .nm{font-size:13px;font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.tgt .meta{font-size:10px;color:#93a4c4;margin-top:2px}
.tgt .meta b{color:#c7d3e8;font-weight:600}
.grow{flex:1;min-width:0}
#drop{margin-top:8px;border:2px dashed #4a5775;border-radius:8px;padding:10px;text-align:center;cursor:pointer;background:#1e2638;font-size:12px;color:#93a4c4}
#drop.hover{border-color:#5eead4;background:#1b2233}
#fname{margin-top:4px;color:#5eead4;word-break:break-all;font-size:12px}#fver{font-size:11px}
.fw{margin-top:8px;font-size:11px;color:#93a4c4;background:#1e2638;border:1px solid #35405a;border-radius:8px;padding:8px}
.fw b{color:#c7d3e8}
button{padding:8px;border:0;border-radius:6px;background:#0ea5e9;color:#fff;font-size:13px;font-weight:600;cursor:pointer}
button:hover{background:#38bdf8}button:disabled{background:#33415c;color:#8391ab;cursor:not-allowed}
button.sec{background:#475569}button.sec:hover{background:#64748b}
button.sm{padding:5px 10px;font-size:12px;font-weight:500}
#go{display:block;width:100%;margin-top:8px}
#prog{margin-top:10px;background:#1e2638;border:1px solid #35405a;border-radius:8px;padding:10px}
.pct{font-size:34px;font-weight:700;line-height:1}.pct small{font-size:16px;color:#93a4c4;margin-left:2px}
.w{height:8px;background:#33415c;border-radius:5px;overflow:hidden;margin:8px 0 4px}
#fill{height:100%;width:0;background:#0ea5e9;transition:width .3s}
.t{display:flex;justify-content:space-between;gap:8px;font-size:11px;color:#93a4c4}
#ab{width:100%;margin-top:8px}
#st{margin-top:8px;font-size:12px;min-height:14px}#st.err{color:#f87171}#st.ok{color:#5eead4}
.ft{margin-top:8px;font-size:10px;color:#64748b;text-align:right}
.ft a{color:#64748b}
details.cfg{margin-top:8px;border:1px solid #35405a;border-radius:8px;background:#1e2638;padding:8px}
details.cfg summary{cursor:pointer;font-size:12px;color:#93a4c4}
.cfgrow{display:flex;align-items:center;gap:8px;margin-top:6px}
.cfgrow span{width:76px;flex:none;font-size:11px;color:#93a4c4}
.cfgrow input{flex:1;min-width:0;padding:4px 6px;border:1px solid #35405a;border-radius:5px;background:#242d40;color:#e8ecf3;font-size:12px}
.cfgrow input.dirty{border-color:#0ea5e9}
.cfghint{margin-top:8px;font-size:10px;color:#64748b;line-height:1.5}
.chk{display:block;font-size:11px;color:#93a4c4;margin-top:6px}
.chk input{margin-right:6px;vertical-align:middle}
.tools{display:flex;gap:6px}
#q{flex:1;min-width:0;padding:5px 8px;border:1px solid #35405a;border-radius:6px;background:#1e2638;color:#e8ecf3;font-size:12px}
.logwrap{position:relative;margin-top:8px}
#logs{height:calc(100vh - 108px);min-height:260px;overflow:auto;margin:0;background:#1e2638;border:1px solid #35405a;border-radius:6px;padding:8px;
font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:10px;line-height:1.45}
#logs div{white-space:pre-wrap;word-break:break-all}
.l-e{color:#f87171}.l-o{color:#7dd3fc}.l-s{color:#a7f3d0}.l-w{color:#c4b5fd}
.jump{position:absolute;right:12px;bottom:12px;padding:6px 10px;font-size:11px;background:#0ea5e9}
@media(max-width:820px){.wrap{grid-template-columns:1fr}#logs{height:52vh}}
)CSS";

static const char PAGE_JS[] PROGMEM = R"JS(
const $=id=>document.getElementById(id);
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
const kb=b=>b>=1048576?(b/1048576).toFixed(2)+' МБ':Math.round(b/1024)+' КБ';
const dur=s=>{s=Math.max(0,Math.round(s));return s>=60?Math.floor(s/60)+' мин '+(s%60)+' с':s+' с'};
const ago=s=>s<60?s+' с':s<3600?Math.floor(s/60)+' мин':Math.floor(s/3600)+' ч';
const upfmt=s=>s>=86400?Math.floor(s/86400)+' д '+Math.floor(s%86400/3600)+' ч':s>=3600?Math.floor(s/3600)+' ч '+Math.floor(s%3600/60)+' мин':Math.floor(s/60)+' мин';
const ERRS={'timeout p1':'сенсор не ответил на старт','timeout p2':'сенсор перестал отвечать на чанки','timeout p3':'сенсор не подтвердил прошивку','no progress':'сенсор не принимает чанки','sensor fail':'сенсор сообщил об ошибке записи','read err':'не читается файл на боте','encrypt err':'ошибка шифрования чанка','no file':'файл на боте не открыт','новый файл':'сессия прервана загрузкой нового файла','manual':'прервано вручную','no RAM':'на сенсоре не хватило памяти','begin fail':'сенсор не смог открыть раздел прошивки','no first chunk':'сенсор не дождался первого чанка','stall timeout':'сенсор перестал получать данные','write fail':'сенсор не смог записать образ','size mismatch':'на сенсор пришло не столько байт','crc mismatch':'контрольная сумма образа не совпала','end fail':'сенсор забраковал образ при записи','board mismatch':'образ собран для другой платы','from bot':'сессию прервал бот'};
const errText=e=>ERRS[e]||e;
let file=null,poll=null,busy=false,vErr=false,sensors=[],info={},target='__self__';
const isSelf=()=>target=='__self__';
function st(t,c){$('st').textContent=t;$('st').className=c||'';vErr=false}
function bar(p,l,r){$('prog').hidden=false;$('pctv').textContent=Math.round(p);$('fill').style.width=p+'%';$('pl').textContent=l;$('pr').textContent=r||''}
function batHtml(p){return '<span class="bat'+(p<20?' low':'')+'"><i style="width:'+Math.max(0,Math.min(100,p))+'%"></i></span>'}
function refresh(){
  const need=isSelf()?'bin':'otaz';
  $('hint').textContent=file?'':'Перетащи .'+need+' сюда или нажми';
  $('file').accept='.'+need;
  const bad=!!file&&file.name.split('.').pop().toLowerCase()!=need;
  if(!busy){
    if(bad){st(isSelf()?'Для бота нужен .bin':'Для сенсора нужен .otaz','err');vErr=true}
    else if(vErr)st('','');
  }
  $('go').textContent=isSelf()?'Прошить бота':'Прошить '+target;
  $('go').disabled=busy||!file||bad;
  $('fwgo')&&($('fwgo').disabled=busy||isSelf());
  $('scfgbox').hidden=isSelf();
}
function setFile(f){
  if(!f)return;
  file=f;
  $('fname').textContent=f.name+' ('+kb(f.size)+')';
  const m=f.name.match(/_v(\d+\.\d+\.\d+)\./);
  $('fver').textContent=m?'версия '+m[1]:'';
  refresh();
}
function addTarget(id,name,meta,online,bat){
  const d=document.createElement('div');
  d.className='tgt'+(target==id?' sel':'');
  d.innerHTML='<span class="dot'+(online?' on':'')+'"></span>'
    +'<span class="grow"><span class="nm">'+name+'</span><div class="meta">'+meta+'</div></span>'
    +(bat>=0?batHtml(bat):'');
  d.onclick=()=>{target=id;renderTargets();refresh()};
  $('targets').appendChild(d);
}
function renderTargets(){
  $('targets').innerHTML='';
  addTarget('__self__',$('dev').textContent+' — этот бот',
            '<b>'+(info.board||'?')+'</b> · v'+(info.ver||'?')+' · файл .bin',true,
            info.bat>=0?info.bat:-1);
  for(const s of sensors){
    const meta='<b>'+(s.board||'?')+'</b> · '+(s.ver?'v'+s.ver:'версия ?')
      +' · '+(s.online?'онлайн':'был '+ago(s.seen_s)+' назад')
      +(s.rssi?' · '+s.rssi+' dBm':'');
    addTarget(s.name,s.name,meta,s.online,s.bat);
  }
  if(!sensors.length){
    const d=document.createElement('div');
    d.style.cssText='font-size:11px;color:#93a4c4;margin-bottom:6px';
    d.textContent='сенсоры ещё не выходили на связь';
    $('targets').appendChild(d);
  }
}
async function loadSensors(){
  try{sensors=await (await fetch('/sensors')).json()}catch(e){return}
  if(!isSelf()&&!sensors.some(s=>s.name==target))target='__self__';
  renderTargets();refresh();
}
async function loadInfo(){
  try{
    info=await (await fetch('/info')).json();
    $('info').innerHTML='<span class="dot'+(info.wifi?' on':'')+'"></span>WiFi'
      +' <span class="dot'+(info.mqtt?' on':'')+'"></span>MQTT · '+info.ip
      +' · '+upfmt(info.up)+' · '+info.temp.toFixed(0)+'°C · heap '+Math.round(info.heap/1024)+' КБ'
      +(info.bat>=0?' · '+batHtml(info.bat)+' '+info.bat+'% ('+info.volt.toFixed(2)+' V)':'');
    const fw=$('fw');
    if(info.fwready){
      fw.hidden=false;
      fw.innerHTML='На боте: <b>'+(info.fwname||'файл .otaz')+'</b><br>'+kb(info.fwsize)
        +' сжато, образ '+kb(info.fwimg)
        +' <button id="fwgo" class="sec sm" style="margin-top:6px">Прошить сохранённым</button>';
      $('fwgo').onclick=flashStored;
    }else fw.hidden=true;
    renderTargets();refresh();
  }catch(e){$('info').textContent='нет связи с ботом'}
}
function finish(){busy=false;$('ab').hidden=true;clearInterval(poll);poll=null;refresh()}
$('drop').onclick=()=>$('file').click();
$('drop').ondragover=e=>{e.preventDefault();$('drop').classList.add('hover')};
$('drop').ondragleave=()=>$('drop').classList.remove('hover');
$('drop').ondrop=e=>{e.preventDefault();$('drop').classList.remove('hover');setFile(e.dataTransfer.files[0])};
$('file').onchange=()=>setFile($('file').files[0]);
$('ab').onclick=()=>fetch('/ota/abort',{method:'POST'}).catch(()=>{});
$('ask').onclick=async()=>{
  $('ask').disabled=true;
  try{
    const r=await fetch('/sensors/hello',{method:'POST'});
    if(!r.ok)throw new Error(await r.text());
    const t0=Date.now();
    if(!busy)st('Запрос отправлен, ждём ответы…','ok');
    for(let i=0;i<3;i++){await sleep(3000);await loadSensors()}
    const n=sensors.filter(s=>s.seen_s<=(Date.now()-t0)/1000).length;
    if(!busy)st('Ответили: '+n+' из '+sensors.length,n?'ok':'err');
  }catch(e){if(!busy)st(e.message,'err')}
  $('ask').disabled=false;
};
let logPos=0,logTimer=null,logAll='',logLines=[],filterQ='';
const atBottom=()=>{const l=$('logs');return l.scrollTop+l.clientHeight>=l.scrollHeight-12};
function scrollBottom(){const l=$('logs');l.scrollTop=l.scrollHeight;$('down').hidden=true}
function lineClass(s){
  if(/(abort|fail|error|ошиб|timeout|mismatch|прерван)/i.test(s))return 'l-e';
  if(s.startsWith('[OTA'))return 'l-o';
  if(s.startsWith('[SNS'))return 'l-s';
  if(s.startsWith('[WEB')||s.startsWith('[MQTT'))return 'l-w';
  return '';
}
function appendLines(arr){
  const l=$('logs'),frag=document.createDocumentFragment();
  for(const s of arr){
    if(filterQ&&!s.toLowerCase().includes(filterQ))continue;
    const d=document.createElement('div');
    d.className=lineClass(s);
    d.textContent=s;
    frag.appendChild(d);
  }
  l.appendChild(frag);
  while(l.childElementCount>2000)l.removeChild(l.firstChild);
}
function renderLog(){$('logs').textContent='';appendLines(logLines);scrollBottom()}
function addChunk(t){
  const stick=atBottom();
  logAll+=t;
  if(logAll.length>120000)logAll=logAll.slice(-80000);
  const lines=t.split('\n').filter(s=>s.length>0);
  logLines.push(...lines);
  if(logLines.length>3000)logLines=logLines.slice(-2000);
  appendLines(lines);
  if(stick)scrollBottom();else $('down').hidden=false;
}
async function pullLog(){
  try{
    const r=await fetch('/logs/tail?from='+logPos);
    const t=await r.text();
    logPos=+r.headers.get('X-Log-Pos')||logPos;
    if(t)addChunk(t);
  }catch(e){}
}
async function initLog(){
  try{
    const r=await fetch('/logs');
    logPos=+r.headers.get('X-Log-Pos')||0;
    logAll=await r.text();
    logLines=logAll.split('\n').filter(s=>s.length>0);
    renderLog();
  }catch(e){$('logs').textContent='нет связи с ботом'}
  clearInterval(logTimer);
  logTimer=setInterval(pullLog,1500);
}
$('logs').onscroll=()=>{if(atBottom())$('down').hidden=true};
$('down').onclick=scrollBottom;
$('q').oninput=()=>{filterQ=$('q').value.trim().toLowerCase();renderLog()};
$('clr').onclick=()=>{logAll='';logLines=[];renderLog()};
$('dl').onclick=()=>{
  const a=document.createElement('a');
  a.href=URL.createObjectURL(new Blob([logAll],{type:'text/plain'}));
  a.download='meshbot-log.txt';
  a.click();
  URL.revokeObjectURL(a.href);
};
function upload(url){
  return new Promise((ok,fail)=>{
    const x=new XMLHttpRequest(),fd=new FormData();
    fd.append('fw',file);
    x.open('POST',url);
    x.upload.onprogress=e=>{if(e.lengthComputable)bar(Math.round(e.loaded*100/e.total),'Загрузка на бот',kb(e.loaded)+' из '+kb(e.total))};
    x.onload=()=>ok(x.responseText);
    x.onerror=()=>fail(new Error('нет связи с ботом'));
    x.send(fd);
  });
}
async function waitBot(){
  await sleep(4000);
  for(let i=0;i<30;i++){
    try{if((await fetch('/info')).ok){location.reload();return}}catch(e){}
    await sleep(2000);
  }
  st('Бот не вернулся за минуту — проверьте питание и WiFi','err');
}
function track(){
  let doneAt=0;
  poll=setInterval(async()=>{
    let j;
    try{j=await (await fetch('/ota/status')).json()}catch(e){return}
    const sec=j.elapsed_ms/1000;
    const stats='повторы: '+j.retrs+' · опросы: '+j.polls;
    if(j.phase==1){bar(0,'Ждём ответ сенсора…',dur(sec))}
    else if(j.phase==2){
      const p=j.total?j.sent*100/j.total:0,rate=sec>0?j.sent/sec:0;
      bar(p,kb(j.sent)+' из '+kb(j.total),rate>0?(rate/1024).toFixed(1)+' КБ/с · осталось '+dur((j.total-j.sent)/rate):'');
      st(j.retr?'Повторы подряд: '+j.retr:'','');
    }
    else if(j.phase==3){bar(100,'Сенсор проверяет прошивку…',dur(sec))}
    else if(j.phase==4){
      bar(100,'Передано за '+dur(sec),stats);
      if(j.back){st('Готово: '+j.target+' загрузился'+(j.ver?' с версией '+j.ver:''),'ok');finish();loadSensors()}
      else{
        doneAt=doneAt||Date.now();
        if(Date.now()-doneAt>120000){st('Прошивка принята, но сенсор пока не вышел на связь','err');finish()}
        else st('Прошивка принята, ждём перезагрузку сенсора…','ok');
      }
    }
    else{st(j.err?'Ошибка: '+errText(j.err)+' ('+stats+')':'Сессия завершена','err');finish()}
  },1000);
}
async function startSession(){
  const s=await fetch('/ota/start?target='+encodeURIComponent(target),{method:'POST'});
  if(!s.ok)throw new Error(await s.text());
  $('ab').hidden=false;
  bar(0,'Ждём ответ сенсора…','');
  track();
}
async function flashStored(){
  if(isSelf())return;
  busy=true;refresh();
  try{await startSession()}catch(e){st(e.message,'err');finish()}
}
$('go').onclick=async()=>{
  if(isSelf()&&!confirm('Прошить сам бот? Он перезагрузится, связь ненадолго пропадёт.'))return;
  busy=true;refresh();
  try{
    if(isSelf()){
      const r=await upload('/update');
      if(r.indexOf('OK')<0)throw new Error(r||'FAIL');
      bar(100,'Прошито','');st('Бот перезагружается, страница обновится сама…','ok');
      waitBot();
      return;
    }
    const r=await upload('/savefw');
    if(r.indexOf('OK')<0)throw new Error(r||'FAIL');
    await startSession();
  }catch(e){st(e.message,'err');finish()}
};
async function loadCfg(){
  let list;
  try{list=await (await fetch('/config')).json()}catch(e){return}
  const box=$('cfg');
  box.innerHTML='';
  for(const it of list){
    const row=document.createElement('div');
    row.className='cfgrow';
    const lab=document.createElement('span');
    lab.textContent=it.f;
    const inp=document.createElement('input');
    // секрет приходит маской: показываем её подсказкой, а значение оставляем пустым,
    // чтобы случайно не записать маску вместо пароля
    if(it.s){inp.placeholder=it.v;inp.value=''}
    else{inp.value=(it.v=='(пусто)')?'':it.v}
    inp.dataset.f=it.f;
    inp.oninput=()=>{inp.dataset.dirty='1';inp.classList.add('dirty')};
    row.appendChild(lab);row.appendChild(inp);
    box.appendChild(row);
  }
}
$('cfgsave').onclick=async()=>{
  const body=new URLSearchParams();
  let n=0;
  document.querySelectorAll('#cfg input').forEach(i=>{
    if(i.dataset.dirty){body.append(i.dataset.f,i.value);n++}
  });
  if(!n){st('Ничего не изменено','');return}
  if(!confirm('Сохранить изменённых полей: '+n+'? Бот перезагрузится.'))return;
  body.append('reboot','1');
  try{
    const r=await fetch('/config',{method:'POST',body});
    const t=await r.text();
    if(!r.ok)throw new Error(t);
    st(t+' — ждём возврата бота…','ok');
    waitBot();
  }catch(e){st(e.message,'err')}
};
const SCFG=['name','sns_name','sns_key','lora_freq','lora_bw','lora_sf','lora_cr',
            'lora_tx','lora_pre','lora_sync','tz','disp_bri','vext_on'];
function buildSensorCfg(){
  const box=$('scfg');
  if(box.childElementCount)return;
  for(const f of SCFG){
    const row=document.createElement('div');
    row.className='cfgrow';
    const lab=document.createElement('span');
    lab.textContent=f;
    const inp=document.createElement('input');
    inp.placeholder='не менять';
    inp.dataset.f=f;
    inp.oninput=()=>inp.classList.toggle('dirty',!!inp.value);
    row.appendChild(lab);row.appendChild(inp);
    box.appendChild(row);
  }
}
buildSensorCfg();
$('scfgget').onclick=async()=>{
  if(isSelf()){st('Сначала выберите сенсор','err');return}
  const body=new URLSearchParams();
  body.append('target',target);
  body.append('get','1');
  try{
    const r=await fetch('/sensors/config',{method:'POST',body});
    st(await r.text(),'ok');
  }catch(e){st(e.message,'err')}
};
$('scfgsend').onclick=async()=>{
  if(isSelf()){st('Сначала выберите сенсор','err');return}
  const body=new URLSearchParams();
  body.append('target',target);
  let n=0;
  document.querySelectorAll('#scfg input').forEach(i=>{
    if(i.value){body.append(i.dataset.f,i.value);n++}
  });
  const save=$('scfgsave').checked,reboot=$('scfgreboot').checked;
  if(!n&&!save&&!reboot){st('Нечего отправлять','');return}
  if(!confirm('Отправить '+n+' полей сенсору '+target+'?'
      +(save?' Настройки будут сохранены.':' Без сохранения — до перезагрузки.')
      +(reboot?' Сенсор перезагрузится.':'')))return;
  if(save)body.append('save','1');
  if(reboot)body.append('reboot','1');
  $('scfgsend').disabled=true;
  try{
    const r=await fetch('/sensors/config',{method:'POST',body});
    const t=await r.text();
    if(!r.ok)throw new Error(t);
    st(t,'ok');
    document.querySelectorAll('#scfg input').forEach(i=>{i.value='';i.classList.remove('dirty')});
  }catch(e){st(e.message,'err')}
  $('scfgsend').disabled=false;
};
loadInfo();
loadSensors();
loadCfg();
initLog();
setInterval(loadInfo,5000);
setInterval(loadSensors,20000);
fetch('/ota/status').then(r=>r.json()).then(j=>{if(j.phase>=1&&j.phase<=3){busy=true;$('ab').hidden=false;$('prog').hidden=false;refresh();track()}}).catch(()=>{});
)JS";

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

// Статика отдаётся потоком из флеша, без копии в куче
static void sendStatic(const char* type, PGM_P body) {
    otaServer.sendHeader("Cache-Control", "public, max-age=31536000, immutable");
    otaServer.setContentLength(strlen_P(body));
    otaServer.send(200, type, "");
    otaServer.sendContent_P(body);
}

void otaHandleCss() { sendStatic("text/css; charset=utf-8", PAGE_CSS); }
void otaHandleJs()  { sendStatic("application/javascript; charset=utf-8", PAGE_JS); }

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
    display.printf("RSSI:%.0f SNR:%.0f", lastRSSI, lastSNR);
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
        Serial.printf("[OTA] fast config: FSK %.0f кбит/с\n", (double)OTA_FSK_BR);
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