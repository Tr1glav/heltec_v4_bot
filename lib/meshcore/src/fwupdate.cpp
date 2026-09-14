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
// from — сколько байт уже лежит в файле: просим сервер отдать остаток. Признак partial
// выставляется ДО первого куска, чтобы вызывающий знал, дописывать файл или начинать
// заново: сервер вправе не понять запрос диапазона и прислать файл целиком.
template <typename Sink>
static bool httpStream(const String& url, Sink sink, uint32_t from = 0, bool* partial = nullptr,
                       uint32_t* gotOut = nullptr) {
    WiFiClientSecure cl;
    cl.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(20000);
    if (!http.begin(cl, url)) return false;
    http.addHeader("User-Agent", "meshcore-bot");
    if (from > 0) http.addHeader("Range", "bytes=" + String(from) + "-");
    int code = http.GET();
    bool isPartial = (code == HTTP_CODE_PARTIAL_CONTENT);
    if (partial) *partial = isPartial;
    if (code != HTTP_CODE_OK && !isPartial) {
        slog("[FW] загрузка -> HTTP %d\n", code);
        http.end();
        return false;
    }
    if (from > 0 && !isPartial) slog("[FW] сервер отдал файл целиком, качаю заново\n");
    int total = http.getSize();
    WiFiClient* st = http.getStreamPtr();
    uint8_t buf[1024];
    int got = 0;
    unsigned long lastData = millis();
    while (http.connected() && (total < 0 || got < total)) {
        int avail = st->available();
        if (avail <= 0) {
            if (millis() - lastData > 20000) {
                // Сколько успело прийти — по этому видно, оборвался поток или связь
                // вообще не установилась.
                slog("[FW] обрыв загрузки, принято %d из %d байт\n", got, total);
                if (gotOut) *gotOut = (uint32_t)got;
                http.end();
                return false;
            }
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
    if (gotOut) *gotOut = (uint32_t)got;
    if (isPartial) slog("[FW] докачано %d байт (было %u)\n", got, (unsigned)from);
    else           slog("[FW] принято %d байт\n", got);
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

// Сколько раз пробуем скачать образ. Соединение с GitHub срывается не всегда, а через
// раз (HTTPClient возвращает -1 ещё на установке связи), и повтор через паузу обычно
// проходит. Без него единичный срыв отменял всё обновление до следующей проверки.
#define FW_DOWNLOAD_TRIES 3

// Заполняются в цикле ДО запуска задачи и после этого не меняются: так задача не читает
// то, что цикл может переписать при очередном hello.
static String fwFetchUrl, fwFetchTarget;

bool fwFetchNodeImage(const String& url) {
    // Узел может быть и сенсором, и компаньоном — пишем, кому именно качаем:
    // подпись «образ сенсора» рядом со ссылкой на образ компаньона сбивает с толку.
    slog("[FW] образ для %s: %s\n", fwFetchTarget.c_str(), url.c_str());
    if (otaFile) { otaFile.close(); otaFile = File(); }
    bool ok = false;
    for (int attempt = 1; attempt <= FW_DOWNLOAD_TRIES && !ok; attempt++) {
        // Сколько уже лежит от прошлой попытки — с этого места и просим продолжить
        uint32_t have = 0;
        File chk = LittleFS.open("/ota.bin.part", "r");
        if (chk) { have = (uint32_t)chk.size(); chk.close(); }
        if (have > 0) slog("[FW] в файле уже %u байт, прошу остаток\n", (unsigned)have);

        File f;
        uint32_t got = 0;
        bool partial = false, opened = false, openFail = false;
        ok = httpStream(url, [&](const uint8_t* d, size_t n) {
            if (!opened) {
                // Режим выбираем по ответу сервера: дописываем только если он понял
                // запрос диапазона, иначе начинаем файл заново.
                f = LittleFS.open("/ota.bin.part", partial ? "a" : "w");
                opened = true;
                if (!f) { openFail = true; return false; }
            }
            return f.write(d, n) == n;
        }, have, &partial, &got);
        if (openFail) { slog("[FW] не открылся файл на боте\n"); return false; }
        // Сброс буфера до закрытия: последняя неполная страница кэша LittleFS не доезжала
        // до флеша, и файл выходил кратным 4096 — короче принятого. Образ уходил в эфир
        // обрезанным, а узел не мог распаковать хвост и отвечал «size mismatch».
        if (f) { f.flush(); f.close(); }
        if (ok) {
            // Верим файлу на флеше, а не счётчику принятого: не сошлось — не беда,
            // следующая попытка попросит у сервера ровно недостающий остаток.
            uint32_t want = (partial ? have : 0) + got;
            File chk2 = LittleFS.open("/ota.bin.part", "r");
            uint32_t sz = chk2 ? (uint32_t)chk2.size() : 0;
            if (chk2) chk2.close();
            if (sz != want) {
                slog("[FW] в файле %u из %u байт, дописываю остаток\n", (unsigned)sz, (unsigned)want);
                ok = false;
            }
        }
        if (!ok && attempt < FW_DOWNLOAD_TRIES) {
            slog("[FW] попытка %d не удалась, повторяю\n", attempt);
            delay(2000);
        }
    }
    if (!ok) { LittleFS.remove("/ota.bin.part"); return false; }
    LittleFS.remove("/ota.bin");
    // переименование в конце: оборванная загрузка не должна выглядеть готовой прошивкой
    if (!LittleFS.rename("/ota.bin.part", "/ota.bin")) { slog("[FW] rename не удался\n"); return false; }
    otaInspectStoredFw();
    return otaFwReady;
}

// Когда до следующей проверки: после начатой сессии очередь разбирается быстрее.
static unsigned long fwNextInterval = FW_CHECK_INTERVAL_MS;
// Самообновление, отложенное до следующего прохода цикла. Прошивка себя заканчивается
// перезагрузкой, и начинать её прямо в обработчике запроса нельзя: страница не успеет
// получить ответ.
static bool fwSelfPending = false;
// Нажата кнопка «Проверить обновления».
static bool fwCheckRequested = false;
// Проверку запустила кнопка, а не расписание: тогда настройка auto_upd не учитывается.
static bool fwManual = false;

// ===== Сеть — в отдельной задаче =====
// Опрос GitHub и особенно скачивание образа занимают десятки секунд. Пока это шло в
// главном цикле, координатор всё это время молчал: не отвечал странице, не объявлял себя
// в сети и не обслуживал радио — проверено, до полутора минут тишины в эфире. Поэтому
// сетевая часть вынесена в задачу, а в цикле осталось только то, что трогает радио и
// общие данные об узлах.
enum FwNetStage : uint8_t { FW_NET_IDLE, FW_NET_BUSY, FW_NET_CHECKED, FW_NET_FETCHED };
static volatile FwNetStage fwNetStage = FW_NET_IDLE;
static volatile bool fwNetOk = false;

static void fwCheckTask(void*) {
    fwNetOk = fwCheckLatest();
    fwNetStage = FW_NET_CHECKED;
    vTaskDelete(NULL);
}

static void fwFetchTask(void*) {
    fwNetOk = fwFetchNodeImage(fwFetchUrl);
    fwNetStage = FW_NET_FETCHED;
    vTaskDelete(NULL);
}

static bool fwStartNetTask(TaskFunction_t fn, const char* name) {
    fwNetStage = FW_NET_BUSY;
    // 16 КБ стека: рукопожатию TLS обычного размера не хватает
    if (xTaskCreate(fn, name, 16384, nullptr, 1, nullptr) == pdPASS) return true;
    fwNetStage = FW_NET_IDLE;
    slog("[FW] не удалось создать задачу %s\n", name);
    return false;
}

// Что делать с результатом проверки: выбрать узел и заказать скачивание образа либо
// решить, что обновлять нечего. Выполняется в главном цикле — здесь читаются общие
// данные об узлах.
static void fwAfterCheck() {
    if (!fwManual && !cfg.autoUpd) { slog("[FW] автообновление выключено\n"); return; }

    fwNextInterval = FW_CHECK_INTERVAL_MS;
    // Сначала узлы: обновление себя означает перезагрузку и потерю сессии. Берём ровно
    // один узел за проход — прошивка по радио занимает эфир целиком.
    for (int i = 0; i < sensorDeviceDiscCount; i++) {
        if (!sensorOnlineNow[i] || sensorFwVersion[i].length() == 0) continue;
        if (fwVersionCmp(fwLatest.version, sensorFwVersion[i]) <= 0) continue;
        // Окружение берём из hello; у прошивок постарше его нет — тогда по плате
        String envName = sensorEnv[i];
        if (envName.length() == 0) envName = fwSensorEnvForBoard(sensorBoard[i]);
        if (envName.length() == 0) continue;
        fwFetchTarget = sensorDeviceDisc[i];
        fwFetchUrl = String(FW_RELEASE_DL) + "v" + fwLatest.version + "/"
                   + envName + "_v" + fwLatest.version + ".otaz";
        slog("[FW] узел %s: %s -> %s, качаю образ\n", fwFetchTarget.c_str(),
             sensorFwVersion[i].c_str(), fwLatest.version.c_str());
        fwStartNetTask(fwFetchTask, "fwfetch");
        return;
    }
    if (fwLatest.binUrl.length() > 0 && fwVersionCmp(fwLatest.version, FW_VERSION) > 0) {
        slog("[FW] своя прошивка устарела: %s -> %s\n", FW_VERSION, fwLatest.version.c_str());
        fwSelfPending = true;   // прошьём себя следующим проходом
        return;
    }
    slog("[FW] обновлять нечего, последняя версия %s\n", fwLatest.version.c_str());
}

void fwUpdateTick() {
    if (!wifiConnected || !cfgReady()) return;

    // Пока сетевая задача работает, в цикле делать нечего: он свободен и обслуживает
    // страницу и радио.
    if (fwNetStage == FW_NET_BUSY) return;

    if (fwNetStage == FW_NET_CHECKED) {
        fwNetStage = FW_NET_IDLE;
        if (!fwNetOk) { slog("[FW] не удалось получить сведения о релизе\n"); return; }
        fwAfterCheck();
        return;
    }
    if (fwNetStage == FW_NET_FETCHED) {
        fwNetStage = FW_NET_IDLE;
        if (!fwNetOk) { slog("[FW] образ для %s взять не удалось\n", fwFetchTarget.c_str()); return; }
        if (otaStartSession(fwFetchTarget)) {
            fwNextInterval = FW_RECHECK_AFTER_MS;   // очередь разберём следующим проходом
            slog("[FW] прошиваю %s до %s\n", fwFetchTarget.c_str(), fwLatest.version.c_str());
        }
        return;
    }

    if (fwSelfPending && !otaSessionActive()) {
        fwSelfPending = false;
        fwSelfUpdate(fwLatest.binUrl);   // отсюда возврата обычно нет: плата перезагружается
        return;
    }
    if (otaSessionActive()) return;

    if (fwCheckRequested) {
        fwCheckRequested = false;
        fwManual = true;
        fwStartNetTask(fwCheckTask, "fwcheck");
        return;
    }

    static unsigned long lastCheck = 0;
    if (lastCheck != 0 && millis() - lastCheck < fwNextInterval) return;
    lastCheck = millis();
    fwManual = false;
    fwStartNetTask(fwCheckTask, "fwcheck");
}

void fwRequestCheck() { fwCheckRequested = true; }
#endif
