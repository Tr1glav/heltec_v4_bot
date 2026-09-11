#include "config.h"
#include "globals.h"
#include "crypto.h"
#include "radio.h"
#include "mesh.h"
#include "ota.h"
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

#ifdef MQTT_ENABLED
static uint32_t otaImgSize = 0;       // размер прошивки после распаковки
static uint16_t otaWinAcked = 0;      // бит i — чанк otaSeq+i уже у сенсора
static unsigned long otaBurstMs = 0;  // когда ушёл последний кадр пачки

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
}

bool otaSessionActive() {
    return otaPhase == OTA_PHASE_WAIT_START || otaPhase == OTA_PHASE_DATA || otaPhase == OTA_PHASE_WAIT_END;
}

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
    if (otaFastMode) {
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

// v3: неподтверждённые чанки окна подряд; последний кадр просит у сенсора маску принятых
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
    if (!lastMessage.startsWith("ota:ackstart")) return;
    slog("[OTA] <- %s: %s (phase=%d)\n", lastSender.c_str(), lastMessage.c_str(), otaPhase);
    if (otaPhase != OTA_PHASE_WAIT_START || lastSender != otaTarget) return;
    if (lastMessage != "ota:ackstart:3") {
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
    if (otaRetries > OTA_MAX_RETRIES) {
        char why[24];
        snprintf(why, sizeof(why), "timeout p%d", otaPhase);
        otaBotAbort(why);
        return;
    }
    if (otaPhase == OTA_PHASE_WAIT_START) otaSendStart();
    else if (otaPhase == OTA_PHASE_DATA) {
        slog("[OTA] poll seq=%u\n", (unsigned)otaSeq);
        uint8_t frame[16];
        int f = rawBuildFrame(frame, RAW_TYPE_POLL, otaSeq, NULL, 0);
        if (rawTxFrame(frame, f) == RADIOLIB_ERR_NONE) otaSince = millis();
    }
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
        "Перетащи .bin (бот) или .otaz (сенсор)</span><span id='fname'></span></div>"
        "<input id='file' type='file' accept='.bin,.otaz' style='display:none'>"
        "<button id='go' disabled>&#10133; Начать обновление</button>"
        "<div id='secrow'><button id='ab' class='sec' style='display:none'>&#10060; Прервать</button>"
        "<button class='sec' onclick='loadLogs()'>&#128220; Логи</button></div>"
        "<div id='bar'><div class='t'><span id='pc'>0%</span><span id='sz'></span></div>"
        "<div class='w'><div class='f'></div></div></div>"
        "<div id='st'></div>"
        "<pre id='logs'></pre>"
        "<div style='margin-top:10px;font-size:10px;color:#64748b;text-align:right'>v" FW_VERSION "</div></div>"
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
    }
    case UPLOAD_FILE_WRITE:
    {
        if (!Update.isRunning()) break;
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

static bool otaSaveTooBig = false;

void otaHandleSaveFw() {
    HTTPUpload& up = otaServer.upload();
    switch (up.status) {
    case UPLOAD_FILE_START:
    {
        if (otaPhase != OTA_PHASE_IDLE && otaPhase != OTA_PHASE_DONE) otaBotAbort("новый .bin");
        otaSaving = true;
        otaSaveOk = false;
        otaSaveTooBig = false;
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
                 otaFwReady ? "готово, .otaz на боте" : "нет прошивки на боте");
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
        #ifdef SENSOR_NODE
        sensorLastSent = msg;
        #endif
    }
}

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

void otaSensorSend(const String& msg) {
    if (sensorChannelIdx < 0) return;
    uint8_t enc[256];
    int enclen = buildGroupEnc(sensorChannelIdx, msg, enc);
    if (enclen <= 0) return;
    uint8_t frame[300];
    int f = buildGroupFrameFlood(sensorChannelIdx, msg, frame, sizeof(frame), enc, enclen);
    if (f > 0) floodSend3(-1, frame, f, 20);
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
    Serial.printf("[OTA] abort (%s), остаёмся на текущей прошивке\n", why);
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
        if (target != DEVICE_NAME) return;
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
        Serial.printf("[OTA] start %s: %u байт crc=%08X\n", DEVICE_NAME, total, crc);
        otaSensorDraw();
        // ackstart уходит на штатном конфиге; бот после него ждёт OTA_FAST_SETTLE_MS.
        otaSensorSend("ota:ackstart:3");
        radioSetFastConfig();
        otaFastMode = true;
        Serial.printf("[OTA] fast config %s\n", OTA_FAST_FSK ? "FSK" : "LoRa");
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