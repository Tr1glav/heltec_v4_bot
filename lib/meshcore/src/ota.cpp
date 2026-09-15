#include "config.h"
#include "globals.h"
#include "crypto.h"
#include "radio.h"
#include "mesh.h"
#include "ota.h"
#include "fwupdate.h"   // проверка обновлений по кнопке
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
int rawTxFrame(const uint8_t* frm, int f, bool listenAfter) {
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
uint32_t otaImgSize = 0;       // размер прошивки после распаковки
static uint16_t otaWinAcked = 0;      // бит i — чанк otaSeq+i уже у сенсора
static unsigned long otaBurstMs = 0;  // когда ушёл последний кадр пачки
static unsigned long otaPolledMs = 0; // когда ушёл POLL (0 — POLL на связи нет)
unsigned long otaSessionMs = 0; // старт сессии — для скорости и длительности на странице
unsigned long otaDoneMs = 0;    // когда сенсор подтвердил прошивку
char otaLastErr[48] = "";       // причина последнего abort — показывается на странице
String otaFwName;               // имя последнего загруженного файла — для страницы
uint16_t otaPolls = 0;          // сколько раз пришлось переспрашивать маску за сессию
// Замер «куда уходит время»: чтение с ФС и шифрование против собственно передачи.
// Расчёт даёт 20 мс эфира на кадр 251 Б при 100 кбит/с, а по факту выходит втрое больше.
uint32_t otaUsBuild = 0, otaUsTx = 0, otaChunksSent = 0;
uint16_t otaRetrTotal = 0;      // сколько всего было повторов за сессию

static uint32_t otaZReal = 0;   // сколько байт потока лежит в файле (без хвостовых нулей)

// Для сенсора годится только .otaz (формат OTA_Z_MAGIC); otaFwSize — длина сжатого потока
void otaInspectStoredFw() {
    otaFwCrc = 0;
    otaFwSize = 0;
    otaZReal = 0;
    otaImgSize = 0;   // иначе страница показывает размер от прошлого образа
    File f = LittleFS.open("/ota.bin", "r");
    uint32_t sz = f ? (uint32_t)f.size() : 0;
    uint8_t hdr[OTA_Z_HDR];
    if (sz > OTA_Z_HDR && f.read(hdr, OTA_Z_HDR) == (size_t)OTA_Z_HDR && memcmp(hdr, OTA_Z_MAGIC, 4) == 0) {
        memcpy(&otaImgSize, hdr + 4, 4);
        memcpy(&otaFwCrc, hdr + 8, 4);
        otaZReal = sz - OTA_Z_HDR;
        otaFwSize = otaZReal + OTA_Z_TAIL_PAD;   // хвост нулей уходит в эфир, см. OTA_Z_TAIL_PAD
        slog("[OTA] сжатая прошивка: %u -> %u байт\n", (unsigned)otaImgSize, (unsigned)otaZReal);
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
    // За концом файла идут хвостовые нули дополнения — в файле их нет, дописываем сами
    int real = (off < otaZReal) ? (int)min((uint32_t)n, otaZReal - off) : 0;
    if (real < n) memset(chunk + real, 0, n - real);
    if (real > 0) {
        otaFile.seek(OTA_Z_HDR + off);
        if (otaFile.read(chunk, real) != real) { otaBotAbort("read err"); return 0; }
    }
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
    if (!radioFastReadyNow()) { otaBotAbort("радио не переключилось"); return; }
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
        // WACK без прогресса сразу после пачки и без висящего POLL — запоздалый ответ на
        // прошлую пачку, отвечать на неё незачем. Ответ на POLL так не отбрасываем: это
        // честное «ничего нового» на прямой вопрос, и ретрай за него уже на боте учтён.
        if (!progress && otaPolledMs == 0 && millis() - otaBurstMs < OTA_ACK_TIMEOUT_MS) return;
        otaPolledMs = 0;   // сенсор ответил — висящего POLL больше нет
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
        // Кадр: [BE EF][тип][seq 4][причина][crc16] — причина необязательна, у прошивок
        // постарше её нет, поэтому оставляем прежний общий текст.
        char why[24] = "sensor fail";
        int n = len - 9;
        if (n > 0) {
            if (n > (int)sizeof(why) - 1) n = (int)sizeof(why) - 1;
            memcpy(why, &buf[7], n);
            why[n] = 0;
        }
        otaBotAbort(why);
        return;
    }
}

void otaBotTick() {
    if (otaPhase != OTA_PHASE_WAIT_START &&
        otaPhase != OTA_PHASE_DATA &&
        otaPhase != OTA_PHASE_WAIT_END) return;
    // Сторона DATA ждёт ответ на пачку либо на POLL; строго один источник на эпизод.
    unsigned long wait = (otaPhase == OTA_PHASE_WAIT_START) ? OTA_START_TIMEOUT_MS
                       : (otaPhase == OTA_PHASE_WAIT_END)   ? OTA_END_TIMEOUT_MS
                       : (otaPolledMs == 0)                 ? OTA_ACK_TIMEOUT_MS
                       : OTA_ACK_TIMEOUT_MS;
    unsigned long since = (otaPhase == OTA_PHASE_DATA && otaPolledMs != 0) ? otaPolledMs : otaSince;
    if (millis() - since < wait) return;

    if (otaPhase == OTA_PHASE_DATA) {
        // Пачка зависла (потерян ответ). POLL — не ретрай: он лишь запрашивает состояние
        // сенсора. Ретрай копим по завершённым эпизодам без прогресса: пачка не собралась
        // и честный ответ «ничего нового» (WACK без прогресса) либо вообще нет ответа.
        // Так потерянная пачка не сжигает две единицы ретраев: одну за таймаут пачки и
        // вторую за правдивый ответ сенсора в otaHandleRawBot.
        if (otaPolledMs != 0) {
            otaRetries++;          // предыдущий POLL остался без ответа
            otaRetrTotal++;
            if (otaRetries > OTA_MAX_RETRIES) {
                otaPolledMs = 0;
                otaBotAbort("no response");
                return;
            }
        }
        otaPolls++;
        slog("[OTA] poll seq=%u\n", (unsigned)otaSeq);
        uint8_t frame[16];
        int f = rawBuildFrame(frame, RAW_TYPE_POLL, otaSeq, NULL, 0);
        if (rawTxFrame(frame, f) == RADIOLIB_ERR_NONE) {
            otaSince = millis();
            otaBurstMs = millis();   // ответ на POLL — свежий, «запоздалым» его не считать
            otaPolledMs = millis();
        } else {
            otaPolledMs = 0;         // POLL не ушёл — просто ждём, ретраи то же, что и раньше
        }
        otaDrawProgress();
        return;
    }

    otaRetries++;
    otaRetrTotal++;
    if (otaRetries > OTA_MAX_RETRIES) {
        char why[24];
        snprintf(why, sizeof(why), "timeout p%d", otaPhase);
        otaBotAbort(why);
        return;
    }
    if (otaPhase == OTA_PHASE_WAIT_START) otaSendStart();
    else if (otaPhase == OTA_PHASE_WAIT_END) otaSendEnd();
    otaDrawProgress();
}

// Сколько байт лога записано с загрузки — позиция для живого вывода на странице (/logs/tail)
uint32_t logTotal = 0;

// logTail/logTotal пишутся сетевой задачей (slog) и читаются веб-обработчиками в главном
// цикле — но из разных контекстов ни одна пара операций не безопасна, поэтому вся работа
// с ними идёт под мьютексом. Читатели должны брать снимок одним вызовом, а не читать
// глобалы по отдельности: между чтением logTotal и logTail положение хвоста может сдвинуться,
// и X-Log-Pos разойдётся с текстом.
static SemaphoreHandle_t logLock = NULL;

static void logEnsureLock() {
    if (!logLock) logLock = xSemaphoreCreateMutex();
}

void logGetSnapshot(String& tailOut, uint32_t& totalOut) {
    logEnsureLock();
    xSemaphoreTake(logLock, portMAX_DELAY);
    tailOut = logTail;
    totalOut = logTotal;
    xSemaphoreGive(logLock);
}

void slog(const char* fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    size_t len = min((size_t)n, sizeof(tmp) - 1);   // vsnprintf возвращает длину без учёта обрезки
    Serial.write((const uint8_t*)tmp, len);
    logEnsureLock();
    xSemaphoreTake(logLock, portMAX_DELAY);
    logTail += tmp;
    logTotal += len;
    // logTail — точный хвост потока лога без вставок, иначе позиции /logs/tail разъедутся
    if (logTail.length() > LOG_TAIL_MAX) logTail.remove(0, logTail.length() - LOG_TAIL_MAX);
    xSemaphoreGive(logLock);
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
    otaPolledMs = 0;
    slog("[OTA] старт -> '%s' (%u байт, crc=%08X)\n",
         otaTarget.c_str(), (unsigned)otaFwSize, (unsigned)otaFwCrc);
    otaSendStart();
    otaDrawProgress();
    return true;
}

#endif // MQTT_ENABLED


// Диспетчер: main loop зовёт это, когда поймал raw-фрейм (магия 0xBE 0xEF)
// в fast-режиме.
void otaHandleRawFrame(const uint8_t* buf, int len) {
    #if FEATURE_MESH_OTA_SENDER
    otaHandleRawBot(buf, len);
    #elif FEATURE_MESH_OTA_RECEIVER
    otaHandleRawSensor(buf, len);
    #endif
}