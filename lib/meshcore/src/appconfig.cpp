#include "config.h"
#include "appconfig.h"
#include "globals.h"   // otaActive: во время прошивки по радио перезагружаться нельзя
#include "mesh.h"
#include <Preferences.h>

AppConfig cfg;

static const char* NS = "meshcfg";

// Одна таблица описывает поле сразу для трёх вещей: чтения из NVS, записи и команды "set".
// Добавить настройку — добавить строку. Значение по умолчанию берётся из макросов
// board_config.h: там же лежат параметры железа, и оба источника не расходятся.
enum CfgKind { CFG_STR, CFG_U16, CFG_I16, CFG_FLT };

struct CfgField {
    const char* cmd;          // имя в консоли, оно же ключ в NVS
    CfgKind kind;
    String AppConfig::*s;     // заполнено ровно одно поле в зависимости от kind
    uint16_t AppConfig::*u;
    int16_t AppConfig::*i;
    float AppConfig::*f;
    bool secret;              // скрывать значение в "show"
    float def;                // значение по умолчанию для числовых полей
    const char* defStr;       // ...и для строковых
};

#define F_STR(cmd, member, secret, def) \
    { cmd, CFG_STR, &AppConfig::member, nullptr, nullptr, nullptr, secret, 0, def }
#define F_U16(cmd, member, def) \
    { cmd, CFG_U16, nullptr, &AppConfig::member, nullptr, nullptr, false, def, "" }
#define F_I16(cmd, member, def) \
    { cmd, CFG_I16, nullptr, nullptr, &AppConfig::member, nullptr, false, def, "" }
#define F_FLT(cmd, member, def) \
    { cmd, CFG_FLT, nullptr, nullptr, nullptr, &AppConfig::member, false, def, "" }

static const CfgField FIELDS[] = {
    F_STR("name",      name,      false, ""),
    F_STR("wifi_ssid", wifiSsid,  false, ""),
    F_STR("wifi_pass", wifiPass,  true,  ""),
    F_STR("mqtt_host", mqttHost,  false, ""),
    F_U16("mqtt_port", mqttPort,  1883),
    F_STR("mqtt_user", mqttUser,  false, ""),
    F_STR("mqtt_pass", mqttPass,  true,  ""),
    F_STR("prv_name",  prvName,   false, ""),
    F_STR("prv_key",   prvKey,    true,  ""),
    F_STR("sns_name",  snsName,   false, ""),
    F_STR("sns_key",   snsKey,    true,  ""),
    F_STR("tx_ch",     txChannel, false, "#connections"),
    F_FLT("lora_freq", loraFreq,  LORA_FREQ),
    F_FLT("lora_bw",   loraBw,    LORA_BW),
    F_U16("lora_sf",   loraSf,    LORA_SF),
    F_U16("lora_cr",   loraCr,    LORA_CR),
    F_I16("lora_tx",   loraTx,    LORA_TX_POWER),
    F_U16("lora_pre",  loraPre,   LORA_PREAMBLE),
    F_U16("lora_sync", loraSync,  LORA_SYNC_WORD),
    F_I16("tz",        tzOffset,  TZ_OFFSET_HOURS),
    F_U16("disp_bri",  dispBri,   255),
    F_U16("vext_on",   vextOn,    VEXT_EN_ACTIVE),
    F_U16("auto_upd",  autoUpd,   1),
};
static const size_t FIELD_COUNT = sizeof(FIELDS) / sizeof(FIELDS[0]);

bool cfgReady() {
    return cfg.name.length() > 0;
}

static void cfgSetDefault(const CfgField& fl) {
    switch (fl.kind) {
        case CFG_STR: cfg.*(fl.s) = fl.defStr; break;
        case CFG_U16: cfg.*(fl.u) = (uint16_t)fl.def; break;
        case CFG_I16: cfg.*(fl.i) = (int16_t)fl.def; break;
        case CFG_FLT: cfg.*(fl.f) = fl.def; break;
    }
}

bool cfgLoad() {
    for (size_t i = 0; i < FIELD_COUNT; i++) cfgSetDefault(FIELDS[i]);

    Preferences p;
    // Открываем на запись: при первом запуске это создаёт пространство имён. В режиме
    // только для чтения несуществующее пространство даёт ошибку NOT_FOUND в логе при
    // каждом старте нового устройства, хотя ничего страшного не произошло.
    if (!p.begin(NS, false)) return false;
    for (size_t i = 0; i < FIELD_COUNT; i++) {
        const CfgField& fl = FIELDS[i];
        if (!p.isKey(fl.cmd)) continue;        // нет в NVS — остаётся значение по умолчанию
        switch (fl.kind) {
            case CFG_STR: cfg.*(fl.s) = p.getString(fl.cmd, fl.defStr); break;
            case CFG_U16: cfg.*(fl.u) = p.getUShort(fl.cmd, (uint16_t)fl.def); break;
            case CFG_I16: cfg.*(fl.i) = p.getShort(fl.cmd, (int16_t)fl.def); break;
            case CFG_FLT: cfg.*(fl.f) = p.getFloat(fl.cmd, fl.def); break;
        }
    }
    p.end();
    return cfgReady();
}

void cfgSave() {
    Preferences p;
    if (!p.begin(NS, false)) {
        Serial.println("[CFG] NVS недоступна, сохранить не удалось");
        return;
    }
    for (size_t i = 0; i < FIELD_COUNT; i++) {
        const CfgField& fl = FIELDS[i];
        switch (fl.kind) {
            case CFG_STR: p.putString(fl.cmd, cfg.*(fl.s)); break;
            case CFG_U16: p.putUShort(fl.cmd, cfg.*(fl.u)); break;
            case CFG_I16: p.putShort(fl.cmd, cfg.*(fl.i)); break;
            case CFG_FLT: p.putFloat(fl.cmd, cfg.*(fl.f)); break;
        }
    }
    p.end();
    Serial.println("[CFG] сохранено, перезагрузите устройство (reboot)");
}

// Значение поля строкой — для "show". Секреты заменяются длиной, чтобы их можно было
// сверить (например скриптом настройки), не раскрывая.
static String cfgValueStr(const CfgField& fl, bool secrets) {
    switch (fl.kind) {
        case CFG_STR: {
            const String& v = cfg.*(fl.s);
            if (fl.secret && !secrets) {
                if (v.length() == 0) return "(пусто)";
                return String("(задано, ") + v.length() + " симв.)";
            }
            return v.length() ? v : String("(пусто)");
        }
        case CFG_U16: {
            char b[16];
            if (strcmp(fl.cmd, "lora_sync") == 0) snprintf(b, sizeof(b), "0x%02X", cfg.*(fl.u));
            else snprintf(b, sizeof(b), "%u", cfg.*(fl.u));
            return b;
        }
        case CFG_I16: {
            char b[16];
            snprintf(b, sizeof(b), "%d", cfg.*(fl.i));
            return b;
        }
        case CFG_FLT: {
            char b[24];
            // 9 значащих цифр: 6 округляли бы 868.731018 до 868.731, и сверка
            // настроек внешним скриптом видела бы ложное расхождение
            snprintf(b, sizeof(b), "%.9g", (double)(cfg.*(fl.f)));
            return b;
        }
    }
    return "";
}

void cfgPrint(bool secrets) {
    Serial.println("\n[CFG] текущие настройки:");
    for (size_t i = 0; i < FIELD_COUNT; i++) {
        Serial.printf("  %-10s %s\n", FIELDS[i].cmd, cfgValueStr(FIELDS[i], secrets).c_str());
    }
    Serial.printf("  настроено: %s\n", cfgReady() ? "да" : "НЕТ — задайте хотя бы name");
}

static void cfgHelp() {
    Serial.println("\n[CFG] команды настройки:");
    Serial.println("  show            показать настройки (пароли скрыты)");
    Serial.println("  show all        показать настройки вместе с паролями и ключами");
    Serial.println("  set <поле> <значение>");
    Serial.println("  clear <поле>    вернуть полю значение по умолчанию");
    Serial.println("  save            записать в NVS");
    Serial.println("  reboot          перезагрузить");
    Serial.println("  bri <0..255>    применить яркость экрана сразу");
    Serial.println("  vext <0|1>      уровень питания периферии сразу (подбор полярности)");
    Serial.print("  поля:");
    for (size_t i = 0; i < FIELD_COUNT; i++) Serial.printf(" %s", FIELDS[i].cmd);
    Serial.println();
    Serial.println("  ключи каналов — base64 от 16 байт; пустой ключ = автоключ по имени");
    Serial.println("  параметры радио обязаны совпадать у всех узлов сети");
}

static const CfgField* cfgFind(const String& field) {
    for (size_t i = 0; i < FIELD_COUNT; i++) {
        if (field == FIELDS[i].cmd) return &FIELDS[i];
    }
    return NULL;
}

static bool cfgSetField(const CfgField& fl, const String& value) {
    switch (fl.kind) {
        case CFG_STR: cfg.*(fl.s) = value; break;
        // strtol с основанием 0 понимает и 18, и 0x12 — слово синхронизации привычнее в hex
        case CFG_U16: cfg.*(fl.u) = (uint16_t)strtoul(value.c_str(), NULL, 0); break;
        case CFG_I16: cfg.*(fl.i) = (int16_t)strtol(value.c_str(), NULL, 0); break;
        case CFG_FLT: cfg.*(fl.f) = value.toFloat(); break;
    }
    return true;
}

static void cfgHandleLine(String line) {
    line.trim();
    if (line.length() == 0) return;

    int sp = line.indexOf(' ');
    String verb = (sp < 0) ? line : line.substring(0, sp);
    String rest = (sp < 0) ? String() : line.substring(sp + 1);
    rest.trim();

    // Живые команды: применяются сразу, чтобы подбирать значение глазами, не
    // перезагружая плату. В NVS они попадут только по "save".
    if (verb == "bri") {
        cfg.dispBri = (uint16_t)rest.toInt();
        #if HAS_OLED
        display.setBrightness((uint8_t)cfg.dispBri);
        #endif
        Serial.printf("[CFG] яркость %u применена (нужен save)\n", cfg.dispBri);
        return;
    }
    if (verb == "vext") {
        cfg.vextOn = (uint16_t)rest.toInt();
        #if defined(VEXT_PIN)
        pinMode(VEXT_PIN, OUTPUT);
        digitalWrite(VEXT_PIN, cfg.vextOn ? HIGH : LOW);
        Serial.printf("[CFG] питание периферии: пин %d -> %s (нужен save)\n",
                      (int)VEXT_PIN, cfg.vextOn ? "HIGH" : "LOW");
        #else
        Serial.println("[CFG] у этой платы нет управляемого питания периферии");
        #endif
        return;
    }
    if (verb == "help") { cfgHelp(); return; }
    if (verb == "show") { cfgPrint(rest == "all"); return; }
    if (verb == "save") { cfgSave(); return; }
    if (verb == "reboot") { Serial.println("[CFG] перезагрузка..."); delay(200); ESP.restart(); return; }
    if (verb == "clear") {
        const CfgField* fl = cfgFind(rest);
        if (!fl) { Serial.printf("[CFG] неизвестное поле '%s'\n", rest.c_str()); return; }
        cfgSetDefault(*fl);
        Serial.printf("[CFG] %s очищено (нужен save)\n", rest.c_str());
        return;
    }
    if (verb == "set") {
        int sp2 = rest.indexOf(' ');
        if (sp2 <= 0) { Serial.println("[CFG] нужно: set <поле> <значение>"); return; }
        String field = rest.substring(0, sp2);
        String value = rest.substring(sp2 + 1);
        value.trim();
        const CfgField* fl = cfgFind(field);
        if (!fl) { Serial.printf("[CFG] неизвестное поле '%s', см. help\n", field.c_str()); return; }
        cfgSetField(*fl, value);
        Serial.printf("[CFG] %s задано (нужен save)\n", field.c_str());
        return;
    }
    Serial.printf("[CFG] неизвестная команда '%s', см. help\n", verb.c_str());
}

#ifdef SENSOR_NODE
// millis() последней правки по радио, ещё не подтверждённой "save"; 0 — таких нет
static unsigned long cfgPendingSince = 0;

// Настройка сенсора по радио. Присваивание идёт только в память: пока не пришёл "save",
// любую ошибку чинит снятие питания. Поэтому смена ключа канала или параметров радио
// становится опасной лишь после save и перезагрузки.
void cfgHandleMeshCfg(const String& rest) {
    if (rest == "save") {
        cfgSave();
        cfgPendingSince = 0;   // подтверждено, сторож больше не нужен
        sensorSendMsg("cfg:ok:save");
        return;
    }
    if (rest == "reboot") {
        sensorSendMsg("cfg:ok:reboot");
        delay(500);
        ESP.restart();
        return;
    }
    if (rest == "get") {
        // Один компактный ответ вместо двух десятков сообщений; секреты не отдаём.
        // Пустые поля пропускаем: у сенсора не заданы WiFi, MQTT и приватный канал, а
        // "(пусто)" занимает 12 байт в UTF-8 и вытесняло из ответа всё интересное.
        // Всё не влезает в одно групповое сообщение (лимит ~200 символов), поэтому шлём
        // частями с паузой: пока сенсор передаёт, он не слышит, и бот тоже должен успеть
        // принять предыдущую часть.
        String out = "cfg:val:";
        for (size_t i = 0; i < FIELD_COUNT; i++) {
            if (FIELDS[i].secret) continue;
            String v = cfgValueStr(FIELDS[i], false);
            if (v.length() == 0 || v == "(пусто)") continue;
            String piece = String(FIELDS[i].cmd) + "=" + v + ";";
            if (out.length() + piece.length() > 180) {
                sensorSendMsg(out.c_str());
                delay(1500);
                out = "cfg:val:";
            }
            out += piece;
        }
        if (out.length() > 8) sensorSendMsg(out.c_str());
        return;
    }
    int eq = rest.indexOf('=');
    if (eq <= 0) {
        sensorSendMsg("cfg:err:format");
        return;
    }
    String field = rest.substring(0, eq);
    String value = rest.substring(eq + 1);
    const CfgField* fl = cfgFind(field);
    if (!fl) {
        sensorSendMsg(("cfg:err:" + field).c_str());
        return;
    }
    cfgSetField(*fl, value);
    cfgPendingSince = millis();
    Serial.printf("[CFG] по радио: %s задано (жду save)\n", field.c_str());
    sensorSendMsg(("cfg:ok:" + field).c_str());
}

void cfgPendingTick() {
    if (cfgPendingSince == 0) return;
    if (otaActive) return;               // посреди прошивки перезагружаться нельзя
    if (millis() - cfgPendingSince < CFG_PENDING_REVERT_MS) return;
    Serial.println("[CFG] подтверждения save не было — перезагрузка к сохранённым настройкам");
    delay(100);
    ESP.restart();
}
#endif

int cfgFieldCount() { return (int)FIELD_COUNT; }

const char* cfgFieldName(int idx) {
    return (idx >= 0 && idx < (int)FIELD_COUNT) ? FIELDS[idx].cmd : "";
}

String cfgFieldValue(int idx, bool secrets) {
    if (idx < 0 || idx >= (int)FIELD_COUNT) return "";
    return cfgValueStr(FIELDS[idx], secrets);
}

bool cfgFieldSecret(int idx) {
    return (idx >= 0 && idx < (int)FIELD_COUNT) ? FIELDS[idx].secret : false;
}

bool cfgApply(const String& field, const String& value) {
    const CfgField* fl = cfgFind(field);
    if (!fl) return false;
    cfgSetField(*fl, value);
    return true;
}

void cfgConsoleTick() {
    static String line;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            String done = line;
            line = "";
            cfgHandleLine(done);
        } else if (line.length() < 200) {
            line += c;
        }
    }
}
