#include "config.h"
#include "globals.h"
#include "fwupdate.h"
#include "ota.h"
#include "ota_internal.h"   // otaFwName: имя скачанного образа показывает страница
#include "mesh.h"

#ifdef MQTT_ENABLED
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

FwLatest fwLatest;

// Прогресс скачивания образа для страницы (см. fwupdate.h)
volatile uint8_t  fwDlPhase = 0;
volatile uint32_t fwDlGot = 0;
volatile uint32_t fwDlTotal = 0;
volatile uint8_t  fwDlAttempt = 0;
volatile char     fwDlTarget[16] = "";

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
    // Ответ с тремя ассетами вплотную подходит к прежнему пределу в 16 КБ, а при обрезке
    // теряется ссылка на собственный образ — самообновление молча перестаёт работать.
    if (!httpGetString(FW_RELEASE_API, body, 32768)) return false;

    String tag = jsonField(body, "tag_name");
    if (tag.startsWith("v")) tag.remove(0, 1);
    if (tag.length() == 0) return false;

    fwLatest.version = tag;
    fwLatest.binUrl = "";
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
// Адрес ассета на GitHub — это перенаправление на хранилище. Встроенное следование за
// перенаправлением тянет новый адрес, не разрывая уже открытое TLS-соединение, и загрузка
// рвётся: то HTTP -1, то файл приходит короче заявленного. Поэтому конечную ссылку
// выясняем отдельным запросом, а качаем по ней уже чистым соединением.
static String fwResolveUrl(const String& url) {
    String cur = url;
    for (int hop = 0; hop < 4; hop++) {
        WiFiClientSecure cl;
        cl.setInsecure();
        HTTPClient http;
        http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
        http.setTimeout(15000);
        if (!http.begin(cl, cur)) return cur;
        http.addHeader("User-Agent", "meshcore-bot");
        const char* want[] = { "Location" };
        http.collectHeaders(want, 1);
        // HEAD, а не GET: тела он не тянет, а перенаправление отдаёт так же. Прежде на
        // каждую попытку приходилось три запроса, из них один — полная загрузка, которую
        // тут же бросали. Лишнее рукопожатие TLS било и по куче, и по стеку задачи.
        int code = http.sendRequest("HEAD");
        if (code == HTTP_CODE_MOVED_PERMANENTLY || code == HTTP_CODE_FOUND ||
            code == HTTP_CODE_SEE_OTHER || code == HTTP_CODE_TEMPORARY_REDIRECT ||
            code == HTTP_CODE_PERMANENT_REDIRECT) {
            String loc = http.header("Location");
            http.end();
            if (loc.length() == 0) return cur;
            slog("[FW] переход %d, шаг %d\n", code, hop + 1);
            cur = loc;
            continue;
        }
        http.end();
        return cur;
    }
    return cur;
}

// Образ тянем только целиком. Докачка с середины запрещена: склейка кусков от разных
// ответов сервера даёт не тот поток, и контрольная сумма образа перестаёт сходиться —
// сорвавшаяся попытка начинается с чистого листа.
template <typename Sink>
static bool httpStream(const String& url, Sink sink, uint32_t* gotOut = nullptr) {
    String real = fwResolveUrl(url);
    WiFiClientSecure cl;
    cl.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.setTimeout(20000);
    if (!http.begin(cl, real)) return false;
    http.addHeader("User-Agent", "meshcore-bot");
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        slog("[FW] загрузка -> HTTP %d\n", code);
        http.end();
        return false;
    }
    int total = http.getSize();
    if (total < 0) {
        // Без объявленной длины полноту образа проверить нечем: обрезанный файл прошёл бы
        // как удачный и уехал в эфир. Для прошивки это недопустимо.
        slog("[FW] сервер не сообщил длину файла, отказ\n");
        http.end();
        return false;
    }
    if (gotOut) { fwDlTotal = (uint32_t)total; fwDlGot = 0; }   // для страницы
    WiFiClient* st = http.getStreamPtr();
    // Буфер статический, не на стеке: в сетевой задаче рядом живут клиент TLS и его
    // рукопожатие, и лишний килобайт там на вес золота. Загрузки не идут параллельно.
    static uint8_t buf[1024];
    int got = 0;
    unsigned long lastData = millis();
    // Сервер вправе закрыть соединение сразу за последним байтом, когда часть данных ещё
    // лежит в буфере клиента. Выходить только по connected() — значит терять хвост.
    while (got < total && (http.connected() || st->available() > 0)) {
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
        if (gotOut) fwDlGot = (uint32_t)got;   // живьём для полосы прогресса
        if (!sink(buf, (size_t)n)) { http.end(); return false; }
    }
    http.end();
    if (gotOut) *gotOut = (uint32_t)got;
    slog("[FW] принято %d из %d байт\n", got, total);
    return got == total;
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
    // Прежний образ не трогаем: сорвётся загрузка — он останется рабочим, и его
    // по-прежнему можно отправить в эфир. Удаляем его только перед переименованием.
    LittleFS.remove("/ota.bin.part");
    // Прогресс для полосы на странице
    size_t tn = 0;
    while (tn < sizeof(fwDlTarget) - 1 && fwFetchTarget[tn]) { fwDlTarget[tn] = fwFetchTarget[tn]; tn++; }
    fwDlTarget[tn] = 0;   // ноль сразу за именем, иначе в хвосте остаётся прежнее
    fwDlPhase = 1;
    fwDlGot = 0;
    fwDlTotal = 0;
    bool ok = false;
    for (int attempt = 1; attempt <= FW_DOWNLOAD_TRIES && !ok; attempt++) {
        fwDlAttempt = attempt;
        File f;
        uint32_t got = 0;
        bool opened = false, openFail = false;
        ok = httpStream(url, [&](const uint8_t* d, size_t n) {
            if (!opened) {
                f = LittleFS.open("/ota.bin.part", "w");   // только с нуля, докачек нет
                opened = true;
                if (!f) { openFail = true; return false; }
            }
            // Пишем кусок сети как есть, как это делает ручная загрузка через /savefw.
            // Запись, ровно равная блоку файловой системы, на этой плате до флеша не
            // доезжает: финальный кусок с нулевым добиванием в прошлом замирал на
            // границе блока и файл выходил короче принятого.
            return f.write(d, n) == n;
        }, &got);
        if (openFail) { fwDlPhase = 0; slog("[FW] не открылся файл на боте\n"); return false; }
        if (f) { f.flush(); f.close(); }
        if (ok) {
            // Верим файлу на флеше, а не счётчику принятого: короткий хвост мог уйти
            // в отчёт, но не доехать до страницы флеша. Догонять его нулями нельзя —
            // в потерянном хвосте лежат последние байты сжатого потока, и контрольная
            // сумма образа перестанет сходиться. Такую попытку не засчитываем: ниже
            // файл стирается и начнётся следующая, с чистого листа.
            uint32_t want = got;
            File chk2 = LittleFS.open("/ota.bin.part", "r");
            uint32_t sz = chk2 ? (uint32_t)chk2.size() : 0;
            if (chk2) chk2.close();
            if (sz != want) {
                slog("[FW] в файле %u из %u байт — хвост не дошёл, качаю заново\n",
                     (unsigned)sz, (unsigned)want);
                ok = false;
            }
        }
        if (!ok) {
            LittleFS.remove("/ota.bin.part");   // следующая попытка начинается с чистого листа
            if (attempt < FW_DOWNLOAD_TRIES) {
                slog("[FW] попытка %d не удалась, качаю заново\n", attempt);
                delay(2000);
            }
        }
    }
    fwDlPhase = 0;
    if (!ok) { LittleFS.remove("/ota.bin.part"); return false; }
    LittleFS.remove("/ota.bin");
    // переименование в конце: оборванная загрузка не должна выглядеть готовой прошивкой
    if (!LittleFS.rename("/ota.bin.part", "/ota.bin")) { slog("[FW] rename не удался\n"); return false; }
    // Имя образа кладём рядом с файлом: без этого страница показывает имя от прошлой
    // ручной заливки — например сенсорное рядом с образом компаньона.
    int slash = url.lastIndexOf('/');
    otaFwName = (slash < 0) ? url : url.substring(slash + 1);
    File nm = LittleFS.open("/ota.name", "w");
    if (nm) { nm.print(otaFwName); nm.close(); }
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
    // Запас стека на выходе: здесь же живут рукопожатие TLS и буфер чтения, а
    // переполнение портит кучу — в ней лежат дескриптор и кэш файловой системы, и
    // записи тогда принимаются, но до флеша не доезжают.
    slog("[FW] запас стека задачи: %u Б\n", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    fwNetStage = FW_NET_FETCHED;
    vTaskDelete(NULL);
}

static bool fwStartNetTask(TaskFunction_t fn, const char* name) {
    fwNetStage = FW_NET_BUSY;
    // 24 КБ стека: рукопожатию TLS обычного размера не хватает, а в задаче загрузки к
    // нему добавляются кадры разбора ссылки и потоковой записи. Запас печатается в
    // журнал на выходе из задачи — по нему видно, не подошли ли мы к краю.
    if (xTaskCreate(fn, name, 24576, nullptr, 1, nullptr) == pdPASS) return true;
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
