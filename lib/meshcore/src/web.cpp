#include "config.h"
#include "globals.h"
#include "crypto.h"
#include "radio.h"
#include "mesh.h"
#include "ota.h"
#include "ota_internal.h"
#include "fwupdate.h"   // проверка обновлений по кнопке
#include "display.h"
#include <esp_system.h>

// ===== Веб-страница координатора и диагностика =====
// Выделено из ota.cpp: разметка страницы, все обработчики HTTP, отчёт о состоянии и
// раздача сжатой статики. Раздача прошивки по радио осталась в ota.cpp — эти две части
// связаны лишь несколькими переменными состояния сессии, которые объявлены в
// ota_internal.h.

#if FEATURE_WEB

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
    {
        String tail;
        uint32_t total;
        logGetSnapshot(tail, total);
        if (total > tail.length()) r += "…(начало обрезано)…\r\n";
        r += tail;
    }
    return r;
}

// Проверка LittleFS по запросу. Раньше шла внутри /logs и писала во flash при каждом
// открытии страницы — теперь только когда её явно попросили.
void otaHandleSelfTest() {
    // Место на файловой системе: без этих чисел не отличить «файл не дописался» от
    // «в разделе кончились блоки», а скачанный образ молча обрывался именно так.
    String r = "Место: занято " + String((unsigned)LittleFS.usedBytes()) +
               " из " + String((unsigned)LittleFS.totalBytes()) + " Б\r\n";
    {
        File d = LittleFS.open("/");
        for (File e = d.openNextFile(); e; e = d.openNextFile()) {
            r += "  " + String(e.name()) + " — " + String((unsigned)e.size()) + " Б\r\n";
        }
    }
    r += "LittleFS: ";
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

// Живой вывод лога: текст, записанный после позиции from (счётчик xlogpPos)
void otaHandleLogTail() {
    String tail;
    uint32_t pos;
    logGetSnapshot(tail, pos);
    uint32_t from = strtoul(otaServer.arg("from").c_str(), NULL, 10);
    uint32_t tailStart = pos - tail.length();
    String out;
    if (from > pos) from = tailStart;   // бот перезагрузился — отдаём весь хвост
    if (from < tailStart) {
        out = "…(пропущено)…\n";
        from = tailStart;
    }
    out += tail.substring(from - tailStart);
    otaServer.sendHeader("X-Log-Pos", String(pos));
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

// Кнопка «Проверить обновления»: тот же ход, что у автообновления по расписанию, но
// запускается он в главном цикле. Отвечать надо сразу: проверка ходит в сеть и качает
// образ, и если делать это здесь, страница замолчит на всё время загрузки.
void otaHandleFwCheck() {
    if (otaSessionActive()) {
        otaServer.send(409, "text/plain; charset=utf-8", "идёт прошивка узла");
        return;
    }
    fwRequestCheck();
    otaServer.send(200, "text/plain; charset=utf-8",
                   "проверяю обновления, ход и результат — в журнале");
}

// Полоса прогресса скачивания образа: сколько байт из скольких уже легло на флеш.
// Фаза берётся из сетевой задачи загрузки, поэтому то, что качает, и эта функция
// живут в разных контекстах — они связаны только volatile-переменными.
void otaHandleFwStatus() {
    char copy[sizeof(fwDlTarget)];
    size_t i = 0;
    while (i < sizeof(copy) - 1 && fwDlTarget[i]) { copy[i] = fwDlTarget[i]; i++; }
    copy[i] = 0;   // ноль сразу за именем: дальше в буфере мусор со стека
    char tgt[sizeof(copy)];
    jsonEscape(copy, tgt, sizeof(tgt));
    char json[192];
    snprintf(json, sizeof(json),
             "{\"phase\":%u,\"got\":%u,\"total\":%u,\"attempt\":%u,\"target\":\"%s\"}",
             (unsigned)fwDlPhase, (unsigned)fwDlGot, (unsigned)fwDlTotal,
             (unsigned)fwDlAttempt, tgt);
    otaServer.send(200, "application/json", json);
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
<div class='row'><button id='ask' class='sec sm grow'>Опросить сенсоры</button>
<button id='fwchk' class='sec sm grow'>Проверить обновления</button></div>
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
        if (mqttConnected) {
            mqtt.disconnect();
            mqttConnected = false;   // при неудаче otaSelfUpdateResume не оставит ложь: цикл тут же переподключит
        }
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
        // Идёт сетевая загрузка образа (задача fwFetch): она пишет /ota.bin.part, и её
        // финализация в главном цикле держит общие флаги. Свою заливку начинать нельзя —
        // обе писали бы /ota.bin и otaFwReady/otaFwName, и результат смешался бы.
        if (fwNetBusy()) {
            otaSaving = false;
            otaSaveOk = false;
            slog("[OTA-SAVE] отклонено: идёт сетевая загрузка образа\n");
            otaServer.send(503, "text/plain; charset=utf-8", "идёт сетевая загрузка образа, подождите");
            return;
        }
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
                // Сверяем с длиной потока в файле: otaFwSize учитывает ещё и хвост нулей,
                // который дописывается в эфир, но в файле его нет (см. OTA_Z_TAIL_PAD).
                uint32_t inFile = otaFwSize - OTA_Z_TAIL_PAD + OTA_Z_HDR;
                if (otaFwReady && inFile != (uint32_t)otaWriteBytes) {
                    slog("[OTA-SAVE] ВНИМАНИЕ: size()=%u != writeBytes=%lu\n",
                         (unsigned)inFile, otaWriteBytes);
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
    // Образ сейчас качается из релиза или финализируется: /ota.bin ещё не готов, и его
    // могут переименовывать. Сессия по недописанному файлу ничего не даст.
    if (fwNetBusy()) {
        slog("[WEB] /ota/start: идёт сетевая загрузка образа\n");
        otaServer.send(503, "text/plain; charset=utf-8", "идёт загрузка образа, подождите");
        return;
    }
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
    otaServer.on("/fw/check", HTTP_POST, otaHandleFwCheck);
    otaServer.on("/fw/status", HTTP_GET, otaHandleFwStatus);
    otaServer.on("/sensors/config", HTTP_POST, otaHandleSensorsConfig);
    otaServer.on("/logs", HTTP_GET, []() {
        String tail;
        uint32_t pos;
        logGetSnapshot(tail, pos);
        otaServer.sendHeader("X-Log-Pos", String(pos));
        otaServer.send(200, "text/plain; charset=utf-8", buildDiagReport());
    });
    otaServer.on("/logs/tail", HTTP_GET, otaHandleLogTail);
    otaServer.on("/info", HTTP_GET, otaHandleInfo);
    otaServer.on("/selftest", HTTP_GET, otaHandleSelfTest);
    // Обслуживание: раздел, переставший принимать запись, лечится только пересозданием.
    // Образ прошивки не жаль — он всегда скачивается заново с релиза.
    otaServer.on("/fs/format", HTTP_POST, []() {
        if (fwNetBusy()) {
            otaServer.send(503, "text/plain; charset=utf-8", "идёт сетевая загрузка образа, подождите");
            return;
        }
        if (otaFile) { otaFile.close(); otaFile = File(); }
        otaFwReady = false;
        otaFwSize = 0;
        otaSaveOk = false;
        bool ok = LittleFS.format();
        LittleFS.begin(true);
        slog("[FS] пересоздание раздела: %s\n", ok ? "готово" : "ошибка");
        otaServer.send(ok ? 200 : 500, "text/plain; charset=utf-8", ok ? "OK" : "FAIL");
    });
    otaServer.on("/config", HTTP_GET, otaHandleConfigGet);
    otaServer.on("/config", HTTP_POST, otaHandleConfigPost);
    otaServer.on("/style.css", HTTP_GET, otaHandleCss);
    otaServer.on("/app.js", HTTP_GET, otaHandleJs);
    otaServer.begin();
    Serial.println("OTA server: http://<ip>:3232/update | /ota/start");
}


#endif // FEATURE_WEB
