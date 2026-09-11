#include "config.h"
#include "globals.h"
#include "crypto.h"
#include "radio.h"
#include "mesh.h"
#include "ota.h"
#include <stdarg.h>
#include <stdio.h>

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

#ifdef MQTT_ENABLED
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
    if (otaRawMode && otaFastMode) {
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
    otaRawMode = false;
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
    snprintf(msg, sizeof(msg), "ota:start:%s:%u:%08X", otaTarget.c_str(),
             (unsigned)otaFwSize, (unsigned)otaFwCrc);
    slog("[OTA] -> %s: %s\n", otaTarget.c_str(), msg);
    otaTxGroup(msg);
}

void otaSendChunk() {
    // ---- чистый LoRa OTA: сырые фреймы на быстрой конфигурации ----
    if (otaRawMode && otaFastMode) {
        if (!otaFile) { otaBotAbort("no file"); return; }
        uint32_t off = otaSeq * OTA_RAW_CHUNK_BYTES;
        if (off >= otaFwSize) return;
        int n = min((int)OTA_RAW_CHUNK_BYTES, (int)(otaFwSize - off));
        uint8_t chunk[OTA_RAW_CHUNK_BYTES];
        otaFile.seek(off);
        if (otaFile.read(chunk, n) != n) { otaBotAbort("read err"); return; }

        // Шифруем чанк секретом канала сенсора: [MAC 2B][ciphertext]
        uint8_t enc[OTA_RAW_CHUNK_BYTES + 18];
        int enc_len = (sensorChannelIdx >= 0)
            ? encryptGroupText(channels[sensorChannelIdx].secret, enc, chunk, n)
            : 0;
        if (enc_len <= 0) { otaBotAbort("encrypt err"); return; }

        uint8_t frame[OTA_RAW_FRAME_MAX];
        int f = rawBuildFrame(frame, RAW_TYPE_DATA, otaSeq, enc, enc_len);
        if (f <= 0) { otaBotAbort("frame err"); return; }
        if (otaSeq % 25 == 0 || otaRetries > 0) {
            uint32_t pct = otaFwSize ? (uint64_t)otaSentBytes * 100 / otaFwSize : 0;
            Serial.printf("[OTA] seq=%u %u%% retr=%u raw=%dB\n",
                          (unsigned)otaSeq, (unsigned)pct, otaRetries, n);
        }
        if (rawTxFrame(frame, f) == RADIOLIB_ERR_NONE) otaSince = millis();
        return;
    }

    // ---- legacy: текстовые hex-чанки по meshcore ----
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
    unsigned long gap = millis() - otaSince;
    Serial.printf("[OTA] seq=%u %u%% retr=%u gap=%lums\n",
                  (unsigned)otaSeq, (unsigned)sentPct, otaRetries, gap);
    otaTxGroup(msg);
}

void otaSendEnd() {
    if (otaRawMode && otaFastMode) {
        uint8_t frame[16];
        int f = rawBuildFrame(frame, RAW_TYPE_DONE, 0, NULL, 0);
        if (f > 0 && rawTxFrame(frame, f) == RADIOLIB_ERR_NONE) {
            slog("[OTA] -> raw DONE\n");
            otaSince = millis();
        }
        return;
    }
    slog("[OTA] -> ota:end\n");
    otaTxGroup("ota:end");
}

void otaHandleAck() {
    if (otaRawMode) return;                      // raw-сессия: mesh-acks больше не ждём
    if (lastSender != otaTarget) return;   // ответ не целивого сенсора
    String m = lastMessage;

    if (m == "ota:ackstart" || m == "ota:ackstart:2") {
        if (otaPhase != OTA_PHASE_WAIT_START) return;
        otaRawMode = (m == "ota:ackstart:2");   // новый сенсор подтверждает raw LoRa OTA
        otaPhase = OTA_PHASE_DATA;
        otaSeq = 0;
        otaSentBytes = 0;
        otaRetries = 0;
        slog("[OTA] ackstart%s -> быстрый конфиг (%.1f MHz SF%d), пауза %dms\n",
             otaRawMode ? ":2 (raw LoRa)" : " (legacy mesh)",
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

// Приём raw-фреймов на боте (сенсор -> бот) во время чистой LoRa OTA
void otaHandleRawBot(const uint8_t* buf, int len) {
    if (!otaRawMode) return;
    if (len < 9 || buf[0] != RAW_MAGIC0 || buf[1] != RAW_MAGIC1) return;
    int paylen = len - 2;
    uint16_t c = (uint16_t)(buf[len - 1] << 8) | buf[len - 2];
    if (crc16buf(buf, paylen) != c) return;
    uint8_t type = buf[2];
    uint32_t seq = (uint32_t)buf[3] | ((uint32_t)buf[4] << 8) |
                   ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 24);

    if (type == RAW_TYPE_ACK) {
        if (otaPhase != OTA_PHASE_DATA) return;
        if (seq < otaSeq) return;                        // устаревший повторный ACK
        if (seq > otaSeq) {
            // Потерянные ack: сенсор уже принял до seq-1 — прыгаем вперёд.
            uint32_t totalChunks = (otaFwSize + OTA_RAW_CHUNK_BYTES - 1) / OTA_RAW_CHUNK_BYTES;
            if (seq >= totalChunks) return;
            uint32_t off = seq * OTA_RAW_CHUNK_BYTES;
            otaSentBytes = min(otaFwSize, off);
            otaSeq = seq;
            otaRetries = 0;
            slog("[OTA] lost-ack jump seq=%u\n", seq);
        } else {
            otaRetries = 0;
            uint32_t off = otaSeq * OTA_RAW_CHUNK_BYTES;
            int n = min((int)OTA_RAW_CHUNK_BYTES, (int)(otaFwSize - off));
            otaSentBytes += n;
            otaSeq++;
        }
        otaDrawProgress();
        if (otaSentBytes >= otaFwSize) {
            otaPhase = OTA_PHASE_WAIT_END;
            slog("[OTA] все байты подтверждены, ждём финал\n");
            otaSendEnd();
        } else {
            otaSendChunk();
        }
        return;
    }
    if (type == RAW_TYPE_NACK) {
        if (otaPhase != OTA_PHASE_DATA) return;
        if (seq < otaSeq) return;
        if (seq > otaSeq) {
            // Lost-ack: сенсор ждёт seq (уже принял до seq-1) — прыгаем вперёд.
            uint32_t off = seq * OTA_RAW_CHUNK_BYTES;
            otaSentBytes = min(otaFwSize, off);
            otaSeq = seq;
            otaRetries = 0;
            slog("[OTA] nack=%u (ack потерялся) -> прыжок на seq=%u\n", seq, seq);
            otaDrawProgress();
            if (otaSentBytes >= otaFwSize) {
                otaPhase = OTA_PHASE_WAIT_END;
                otaSendEnd();
            } else {
                otaSendChunk();
            }
            return;
        }
        otaRetries++;
        if (otaRetries > OTA_MAX_RETRIES) { otaBotAbort("nack raw"); return; }
        otaSendChunk();
        return;
    }
    if (type == RAW_TYPE_DONE_ACK) {
        if (otaPhase != OTA_PHASE_WAIT_END) return;
        otaPhase = OTA_PHASE_DONE;
        slog("[OTA] DONE raw: сенсор %s применил прошивку, CRC32 OK\n", otaTarget.c_str());
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
        otaRawMode = false;
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

#endif // MQTT_ENABLED
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
void otaSensorSend(const String& msg, bool flood, unsigned int staggerMs) {
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

    if (type == RAW_TYPE_DATA) {
        if (!otaGotStart) return;
        if (seq != otaSeqExp) {
            Serial.printf("[OTA] raw REJECT seq %u exp %u\n", seq, otaSeqExp);
            uint8_t f[16];
            int ff = rawBuildFrame(f, RAW_TYPE_NACK, otaSeqExp, NULL, 0);
            if (ff > 0) rawTxFrame(f, ff);
            otaLastActivity = millis();
            return;
        }
        // Фрейм: [magic 2B][type 1B][seq 4B][MAC 2B][ciphertext][crc16 2B]
        if (len < 13) {   // минимум: 7 + MAC(2) + блок 16 + crc16(2)
            uint8_t f[16];
            int ff = rawBuildFrame(f, RAW_TYPE_NACK, otaSeqExp, NULL, 0);
            if (ff > 0) rawTxFrame(f, ff);
            otaLastActivity = millis();
            return;
        }
        const uint8_t* mac = &buf[7];
        const uint8_t* ct = &buf[9];
        int ct_len = len - 11;   // минус заголовок(7) + MAC(2) + crc16(2)
        if (ct_len <= 0 || ct_len % 16 != 0) {
            uint8_t f[16];
            int ff = rawBuildFrame(f, RAW_TYPE_NACK, otaSeqExp, NULL, 0);
            if (ff > 0) rawTxFrame(f, ff);
            otaLastActivity = millis();
            return;
        }
        // Расшифровка секретом канала (MAC внутри проверяется в decryptRaw)
        uint8_t plain[OTA_RAW_CHUNK_BYTES + 16];
        int n = (sensorChannelIdx >= 0)
            ? decryptRaw(channels[sensorChannelIdx].secret, mac, ct, ct_len, plain, sizeof(plain))
            : 0;
        if (n <= 0 || n > OTA_RAW_CHUNK_BYTES) {
            Serial.printf("[OTA] raw decrypt FAIL (len=%d)\n", n);
            uint8_t f[16];
            int ff = rawBuildFrame(f, RAW_TYPE_NACK, seq, NULL, 0);
            if (ff > 0) rawTxFrame(f, ff);
            otaLastActivity = millis();
            return;
        }
        if (Update.write(plain, n) != (size_t)n) {
            Update.printError(Serial);
            uint8_t f[16];
            int ff = rawBuildFrame(f, RAW_TYPE_NACK, seq, NULL, 0);
            if (ff > 0) rawTxFrame(f, ff);
            otaLastActivity = millis();
            return;
        }
        otaCrcAcc = crc32_upd(otaCrcAcc, plain, n);
        otaGot += n;
        otaSeqExp = seq + 1;
        otaLastActivity = millis();
        if (seq % 25 == 0)
            Serial.printf("[OTA] raw seq=%u got=%u/%u\n", seq, otaGot, otaTotal);
        otaSensorDraw();
        uint8_t f[16];
        int ff = rawBuildFrame(f, RAW_TYPE_ACK, seq, NULL, 0);
        if (ff > 0) rawTxFrame(f, ff);
        return;
    }

    if (type == RAW_TYPE_DONE) {
        if (otaGot != otaTotal || !otaGotStart) {
            Serial.printf("[OTA] raw DONE size mismatch got=%u total=%u\n", otaGot, otaTotal);
            uint8_t f[16];
            int ff = rawBuildFrame(f, RAW_TYPE_FAIL, 0, NULL, 0);
            if (ff > 0) rawTxFrame(f, ff);
            otaSensorAbort("size mismatch");
            return;
        }
        uint32_t actCrc = ~otaCrcAcc;
        if (actCrc != otaCrcExp) {
            Serial.printf("[OTA] CRC MISMATCH exp=%08X got=%08X\n", otaCrcExp, actCrc);
            uint8_t f[16];
            int ff = rawBuildFrame(f, RAW_TYPE_FAIL, 0, NULL, 0);
            if (ff > 0) rawTxFrame(f, ff);
            otaSensorAbort("crc mismatch");
            return;
        }
        if (!Update.end(true)) {
            Update.printError(Serial);
            uint8_t f[16];
            int ff = rawBuildFrame(f, RAW_TYPE_FAIL, 0, NULL, 0);
            if (ff > 0) rawTxFrame(f, ff);
            otaSensorAbort("end fail");
            return;
        }
        otaActive = false;
        otaGotStart = false;
        Serial.println("[OTA] raw DONE, rebooting...");
        uint8_t f[16];
        int ff = rawBuildFrame(f, RAW_TYPE_DONE_ACK, 0, NULL, 0);
        if (ff > 0) rawTxFrame(f, ff);
        delay(300);
        otaFastMode = false;
        ESP.restart();
        return;
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

void otaSensorTick() {
    if (!otaActive) return;
    if (millis() - otaLastActivity > OTA_SENSOR_STALL_MS) {
        otaSensorAbort("stall timeout");
    }
}

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
        // ":2" сообщает боту, что сенсор поддерживает чистую LoRa OTA (raw-фреймы).
        // Старый бот этот ответ не поймёт — тогда сенсор отвалится по сторожевому
        // таймеру и вернётся на mesh (нужно прошить сенсор по USB один раз).
        otaSensorSend("ota:ackstart:2", true);
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