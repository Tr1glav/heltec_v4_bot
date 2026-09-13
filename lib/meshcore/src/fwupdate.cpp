#include "config.h"
#include "globals.h"
#include "fwupdate.h"
#include "ota.h"
#include "mesh.h"

#ifdef MQTT_ENABLED
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

FwLatest fwLatest;

int fwVersionCmp(const String& a, const String& b) {
    int ai = 0, bi = 0;
    for (int part = 0; part < 3; part++) {
        long av = strtol(a.c_str() + ai, NULL, 10);
        long bv = strtol(b.c_str() + bi, NULL, 10);
        if (av != bv) return av > bv ? 1 : -1;
        int an = a.indexOf('.', ai), bn = b.indexOf('.', bi);
        if (an < 0 || bn < 0) break;
        ai = an + 1; bi = bn + 1;
    }
    return 0;
}

const char* fwSensorEnvForBoard(const String& board) {
    if (board == "h43") return "heltec_v4_3_sensors";
    if (board == "h3")  return "heltec_v3_sensors";
    return "";
}

// Один HTTPS-запрос. Сертификаты не проверяем: корневые сертификаты пришлось бы носить
// в прошивке и обновлять при их смене. Защита здесь другая — маркер платы в образе и
// CRC32, которые проверяются перед записью.
static bool httpGetString(const String& url, String& out, size_t limit) {
    WiFiClientSecure cl;
    cl.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(15000);
    if (!http.begin(cl, url)) return false;
    http.addHeader("User-Agent", "meshcore-bot");   // без него GitHub отвечает отказом
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        slog("[FW] %s -> HTTP %d\n", url.c_str(), code);
        http.end();
        return false;
    }
    out = http.getString();
    http.end();
    if (out.length() > limit) out.remove(limit);
    return true;
}

// Простейший разбор: JSON-библиотеки ради трёх полей тянуть незачем
static String jsonField(const String& src, const String& key, int from = 0) {
    int k = src.indexOf("\"" + key + "\":\"", from);
    if (k < 0) return "";
    k += key.length() + 4;
    int e = src.indexOf('"', k);
    return (e < 0) ? "" : src.substring(k, e);
}

bool fwCheckLatest() {
    if (!wifiConnected) return false;
    String body;
    if (!httpGetString(FW_RELEASE_API, body, 16384)) return false;

    String tag = jsonField(body, "tag_name");
    if (tag.startsWith("v")) tag.remove(0, 1);
    if (tag.length() == 0) return false;

    fwLatest.version = tag;
    fwLatest.binUrl = "";
    fwLatest.otazUrl = "";
    // Файлы названы <окружение>_v<версия>.<тип>, поэтому своё берём по имени окружения
    String selfPrefix = String(FW_ENV) + "_v";
    int pos = 0;
    while (true) {
        int u = body.indexOf("\"browser_download_url\":\"", pos);
        if (u < 0) break;
        u += 24;
        int e = body.indexOf('"', u);
        if (e < 0) break;
        String url = body.substring(u, e);
        pos = e;
        int slash = url.lastIndexOf('/');
        String name = (slash < 0) ? url : url.substring(slash + 1);
        if (name.startsWith(selfPrefix) && name.endsWith(".bin")) fwLatest.binUrl = url;
        if (name.endsWith(".otaz")) fwLatest.otazUrl = url;
    }
    fwLatest.checkedMs = millis();
    fwLatest.valid = true;
    slog("[FW] последний релиз %s (своя %s)\n", fwLatest.version.c_str(), FW_VERSION);
    return true;
}

// Общая часть скачивания: тянем поток и отдаём кусками в приёмник
template <typename Sink>
static bool httpStream(const String& url, Sink sink) {
    WiFiClientSecure cl;
    cl.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(20000);
    if (!http.begin(cl, url)) return false;
    http.addHeader("User-Agent", "meshcore-bot");
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        slog("[FW] загрузка -> HTTP %d\n", code);
        http.end();
        return false;
    }
    int total = http.getSize();
    WiFiClient* st = http.getStreamPtr();
    uint8_t buf[1024];
    int got = 0;
    unsigned long lastData = millis();
    while (http.connected() && (total < 0 || got < total)) {
        int avail = st->available();
        if (avail <= 0) {
            if (millis() - lastData > 20000) { slog("[FW] обрыв загрузки\n"); http.end(); return false; }
            delay(5);
            continue;
        }
        int n = st->readBytes(buf, min(avail, (int)sizeof(buf)));
        if (n <= 0) continue;
        lastData = millis();
        got += n;
        if (!sink(buf, (size_t)n)) { http.end(); return false; }
    }
    http.end();
    slog("[FW] принято %d байт\n", got);
    return total < 0 || got == total;
}

bool fwSelfUpdate(const String& url) {
    slog("[FW] самообновление: %s\n", url.c_str());
    radio.sleep();
    isListening = false;
    #if HAS_FEM
    digitalWrite(FEM_EN_PIN, LOW);
    #endif
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
        return false;
    }
    FwScan scan;
    fwScanReset(&scan);
    bool ok = httpStream(url, [&](const uint8_t* d, size_t n) {
        fwScanFeed(&scan, d, n);
        return Update.write((uint8_t*)d, n) == n;
    });
    if (ok && fwScanVerdict(&scan) < 0) {
        slog("[FW] образ платы %s — не наш, отказ\n", scan.other);
        ok = false;
    }
    if (!ok || !Update.end(true)) {
        Update.abort();
        slog("[FW] самообновление не удалось\n");
        #if HAS_FEM
        digitalWrite(FEM_EN_PIN, HIGH);
        #endif
        radio.startReceive();
        isListening = true;
        return false;
    }
    slog("[FW] прошито, перезагрузка\n");
    delay(300);
    ESP.restart();
    return true;
}

bool fwFetchSensorImage(const String& url) {
    slog("[FW] образ сенсора: %s\n", url.c_str());
    if (otaFile) { otaFile.close(); otaFile = File(); }
    File f = LittleFS.open("/ota.bin.part", "w");
    if (!f) { slog("[FW] не открылся файл на боте\n"); return false; }
    bool ok = httpStream(url, [&](const uint8_t* d, size_t n) {
        return f.write(d, n) == n;
    });
    f.close();
    if (!ok) { LittleFS.remove("/ota.bin.part"); return false; }
    LittleFS.remove("/ota.bin");
    // переименование в конце: оборванная загрузка не должна выглядеть готовой прошивкой
    if (!LittleFS.rename("/ota.bin.part", "/ota.bin")) { slog("[FW] rename не удался\n"); return false; }
    otaInspectStoredFw();
    return otaFwReady;
}

void fwUpdateTick() {
    if (!wifiConnected || !cfgReady()) return;
    // Пока идёт прошивка, не проверяем и не начинаем ничего нового: сессия в эфире одна,
    // и вклиниваться в неё нельзя.
    if (otaSessionActive()) return;

    static unsigned long lastCheck = 0;
    static unsigned long interval = FW_CHECK_INTERVAL_MS;
    if (lastCheck != 0 && millis() - lastCheck < interval) return;
    lastCheck = millis();
    interval = FW_CHECK_INTERVAL_MS;
    if (!fwCheckLatest()) return;
    if (!cfg.autoUpd) return;

    // Сначала сенсоры: обновление себя означает перезагрузку и потерю сессии.
    // Берём ровно один сенсор за проход — прошивка по радио занимает эфир целиком,
    // и параллельно обновлять несколько физически нельзя.
    {
        for (int i = 0; i < sensorDeviceDiscCount; i++) {
            if (!sensorOnlineNow[i] || sensorFwVersion[i].length() == 0) continue;
            if (fwVersionCmp(fwLatest.version, sensorFwVersion[i]) <= 0) continue;
            // Окружение берём из hello; у прошивок постарше его нет — тогда по плате
            String envName = sensorEnv[i];
            if (envName.length() == 0) envName = fwSensorEnvForBoard(sensorBoard[i]);
            if (envName.length() == 0) continue;
            String url = String(FW_RELEASE_DL) + "v" + fwLatest.version + "/"
                       + envName + "_v" + fwLatest.version + ".otaz";
            slog("[FW] сенсор %s: %s -> %s\n", sensorDeviceDisc[i].c_str(),
                 sensorFwVersion[i].c_str(), fwLatest.version.c_str());
            if (fwFetchSensorImage(url) && otaStartSession(sensorDeviceDisc[i])) {
                interval = FW_RECHECK_AFTER_MS;   // очередь разберём следующим проходом
                return;
            }
        }
    }
    if (fwLatest.binUrl.length() > 0 && fwVersionCmp(fwLatest.version, FW_VERSION) > 0) {
        fwSelfUpdate(fwLatest.binUrl);
    }
}
#endif
