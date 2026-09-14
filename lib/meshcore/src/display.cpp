#include "config.h"
#include "globals.h"
#include "crypto.h"
#include "display.h"
#include "companion.h"   // код сопряжения BLE на экране компаньона

#ifdef SENSOR_NODE
// "12m05s" / "3h07m" — сколько прошло с момента sinceMs
static String agoStr(unsigned long sinceMs) {
    unsigned long s = (millis() - sinceMs) / 1000;
    char buf[16];
    if (s < 3600) snprintf(buf, sizeof(buf), "%lum%02lus", s / 60, s % 60);
    else          snprintf(buf, sizeof(buf), "%luh%02lum", s / 3600, (s % 3600) / 60);
    return buf;
}
#endif

#if HAS_BATTERY
// Замер раз в 5 с: чаще не нужно, а делитель лишний раз не дёргаем
#define BAT_READ_MS 5000

float batteryVoltage() {
    static unsigned long lastMs = 0;
    static float volts = 0;
    if (volts > 0 && millis() - lastMs < BAT_READ_MS) return volts;
    lastMs = millis();
    pinMode(VBAT_CTRL_PIN, OUTPUT);
    digitalWrite(VBAT_CTRL_PIN, VBAT_CTRL_ACTIVE);
    delay(10);                                   // делителю нужно установиться
    analogSetPinAttenuation(VBAT_PIN, ADC_11db); // на делителе ~0.85 В при полной батарее
    uint32_t mv = 0;
    for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(VBAT_PIN);
    digitalWrite(VBAT_CTRL_PIN, VBAT_CTRL_ACTIVE == HIGH ? LOW : HIGH);
    volts = (mv / 8.0f) * VBAT_DIVIDER / 1000.0f;
    return volts;
}

// Без аккумулятора делитель даёт около нуля: на V3 это выглядело как "0.00V" на
// экране и 0% в сообщениях. Порог заведомо ниже любого рабочего LiPo.
bool batteryPresent() {
    return batteryVoltage() > 2.5f;
}

// Кривая разряда LiPo: напряжение к проценту заряда нелинейно
int batteryPercent() {
    if (!batteryPresent()) return -1;
    static const float curve[][2] = {
        { 3.30f, 0 }, { 3.55f, 10 }, { 3.65f, 25 }, { 3.75f, 50 },
        { 3.90f, 75 }, { 4.05f, 90 }, { 4.20f, 100 },
    };
    float v = batteryVoltage();
    if (v <= curve[0][0]) return 0;
    const int n = sizeof(curve) / sizeof(curve[0]);
    for (int i = 1; i < n; i++) {
        if (v < curve[i][0]) {
            float k = (v - curve[i - 1][0]) / (curve[i][0] - curve[i - 1][0]);
            return (int)(curve[i - 1][1] + k * (curve[i][1] - curve[i - 1][1]) + 0.5f);
        }
    }
    return 100;
}
#else
float batteryVoltage() { return 0; }
int batteryPercent() { return -1; }
bool batteryPresent() { return false; }
#endif

#ifdef SENSOR_NODE
// Результат проверки связи держим на экране PING_SHOW_MS вместо обычного статуса
static void drawPingResult() {
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    // Экран сенсора везде на английском — держим единый язык
    display.println("LINK TEST");
    if (pingFailed) {
        display.println("");
        display.println("no reply");
        display.printf("waited %lu s\n", PING_TIMEOUT_MS / 1000);
        display.display();
        return;
    }
    char a[12], b[12];
    display.printf("rtt:  %lu ms\n", pingRttMs);
    display.printf("rx:   %s dBm\n", fmtFix(pingRssi, 0, a, sizeof(a)));
    display.printf("snr:  %s dB\n", fmtFix(pingSnr, 1, b, sizeof(b)));
    display.printf("peer: %d dBm\n", pingPeerRssi);
    display.printf("hops: %u%s\n", pingHops, pingHops == 0 ? " (direct)" : "");
    display.display();
}
#endif

#ifdef COMPANION_NODE
// Код сопряжения во весь экран: его набирают в приложении при первом подключении.
// Цифры крупные (шрифт 18x24), потому что читать их приходится с расстояния вытянутой руки.
static void drawBlePin() {
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("PAIR CODE");          // экран устройства везде на английском
    char pin[12];
    snprintf(pin, sizeof(pin), "%06u", (unsigned)companionBlePin());
    display.setTextSize(3);
    display.setCursor((128 - 6 * 18) / 2, 22);
    display.print(pin);
    display.setTextSize(1);
    display.setCursor(0, 56);
    display.print(cfgReady() ? cfg.name.c_str() : "MeshCore");
    display.display();
}
#endif

#ifdef SENSOR_NODE
// Экран гаснет в простое: на аккумуляторе панель ест 15–25 мА, а смотрят на неё
// редко. Будит длинное нажатие кнопки (см. main.cpp). Содержимое видеопамяти при
// выключении сохраняется, поэтому включение мгновенное и без повторной настройки.
static bool screenOn = true;
static unsigned long screenWakeMs = 0;

void screenWake() {
    screenWakeMs = millis();
    if (screenOn) return;
    screenOn = true;
    display.setPower(true);
}

bool screenIsOn() { return screenOn; }

// Длинное нажатие кнопки: горит — гасим, погас — зажигаем. Ручное решение сильнее
// сторожа простоя: выключенный вручную экран сам не загорится.
void screenToggle() {
    if (screenOn) {
        screenOn = false;
        display.setPower(false);
        Serial.println("[SCR] экран выключен кнопкой");
    } else {
        screenWake();
        Serial.println("[SCR] экран включён кнопкой");
    }
}

void screenTick() {
    if (!screenOn) return;
    // Прошивка по радио идёт — экран держим зажжённым и не гасим, иначе ход
    // сессии (проценты, счётчики) не будет виден на панели.
    if (otaActive) { screenWakeMs = millis(); return; }
    #ifdef COMPANION_NODE
    // Пока приложение не сопряжено, на экране код сопряжения — гасить нельзя,
    // иначе подключиться будет нечем.
    if (!companionBleLinked()) { screenWakeMs = millis(); return; }
    #endif
    if (millis() - screenWakeMs < SCREEN_IDLE_OFF_MS) return;
    screenOn = false;
    display.setPower(false);
    Serial.println("[SCR] экран погашен (простой)");
}
#endif

#if defined(COMPANION_NODE) && HAS_OLED
// Руна Bluetooth 7x11 в правом верхнем углу: рисуем линиями, а не шрифтом — в нашем
// наборе такого знака нет, а картинка в памяти стоила бы больше, чем пять отрезков.
// Показывается, только когда телефон действительно подключён и сопряжён.
static void drawBtIcon(int x, int y) {
    display.drawLine(x + 3, y,      x + 3, y + 10, SSD1306_WHITE);  // ствол
    display.drawLine(x + 3, y,      x + 6, y + 3,  SSD1306_WHITE);  // верхний луч
    display.drawLine(x + 6, y + 3,  x,     y + 7,  SSD1306_WHITE);
    display.drawLine(x + 3, y + 10, x + 6, y + 7,  SSD1306_WHITE);  // нижний луч
    display.drawLine(x + 6, y + 7,  x,     y + 3,  SSD1306_WHITE);
}
#endif

void drawIdleStatus() {
    #ifdef SENSOR_NODE
    if (!screenOn) return;          // панель выключена — не тратим шину I2C впустую
    if (pingShowUntil != 0 && (long)(millis() - pingShowUntil) < 0) { drawPingResult(); return; }
    #endif
    #ifdef COMPANION_NODE
    // Пока телефон не подключился, экран занят кодом сопряжения: его вводят в приложении,
    // и код меняется при каждом запуске, так что подсмотреть его можно только здесь.
    if (!companionBleLinked()) { drawBlePin(); return; }
    #endif
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    // часы из системного времени (обновляются каждые 500 мс вместе с экраном)
    time_t now = time(NULL) + (time_t)cfg.tzOffset * 3600;
    struct tm tm_now;
    gmtime_r(&now, &tm_now);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S  %d.%m", &tm_now);
    unsigned long up = millis() / 1000;
    #ifdef SENSOR_NODE
    display.println(cfgReady() ? cfg.name.c_str() : "NO CONFIG");
    display.println(tbuf);
    if (timeSyncMs) display.printf("sync %s ago\n", agoStr(timeSyncMs).c_str());
    else            display.println("sync: never");
    display.printf("Up: %luh%02lum\n", up / 3600, (up % 3600) / 60);
    if (sensorLastSent.length() > 0) {
        display.printf("TX: %s\n", sensorLastSent.substring(0, 17).c_str());
        display.printf("    %s ago\n", agoStr(sensorLastSentMs).c_str());
    }
    #else
    #ifdef MQTT_ENABLED
    // WiFi/MQTT статус: 1 строка, "+" = подключено, "-" = нет
    const char* w = wifiConnected ? "+" : "-";
    const char* m = mqttConnected ? "+" : "-";
    display.printf("WiFi:%s MQTT:%s\n", w, m);
    if (mqttTxChannel >= 0 && mqttTxChannel < numChannels) {
        display.printf("TX: %s\n", channels[mqttTxChannel].name);
    }
    #endif
    display.println(tbuf);
    // uptime + температура CPU (встроенный датчик ESP32-S3)
    int t = (int)cpuTempC();
    display.printf("Up:%luh%02lum T:%dC\n", up / 3600, (up % 3600) / 60, t);
    display.printf("Pkts: %d\n", packetCount);
    if (lastMessage.length() > 0) {
        display.printf("Last: %s\n", lastMessage.substring(0, 20).c_str());
    }
    #endif
    display.setCursor(0, 56);
    display.print("v" FW_VERSION);
    #ifdef SENSOR_NODE
    if (fwVersionDiffers) display.print("*");
    #endif
    #if HAS_BATTERY
    // без аккумулятора индикатор не рисуем вовсе, чтобы не показывать "0% 0.00V"
    if (batteryPresent()) {
        char bat[16];
        char v[12];
        snprintf(bat, sizeof(bat), "%d%% %sV", batteryPercent(),
                 fmtFix(batteryVoltage(), 2, v, sizeof(v)));
        display.setCursor(SCREEN_WIDTH - (int)strlen(bat) * 6, 56);
        display.print(bat);
    }
    #endif
    #if defined(COMPANION_NODE) && HAS_OLED
    // Значок связи с телефоном — в правом верхнем углу, чтобы не спорить с часами
    if (companionBleLinked()) drawBtIcon(SCREEN_WIDTH - 8, 0);
    #endif
    display.display();
}


// ===== Обновление экрана статуса =====
// Выделено из главного цикла: раз в полсекунды перерисовываем статус, но не затираем
// только что показанное сообщение и не трогаем экран во время прошивки по радио —
// там своя картинка с ходом сессии.
void statusScreenTick() {
    // Показать статус на экране (обновляем раз в 500мс)
    if (isListening && (millis() - lastDisplayUpdate > 500)) {
        lastDisplayUpdate = millis();
        #ifdef SENSOR_NODE
        bool rxScreenHeld = false;   // сенсор входящие пакеты не рисует, статус не ждёт паузы после приёма
        #else
        bool rxScreenHeld = millis() - lastRxDisplay <= 5000;
        #endif
        if (!otaFastMode && !rxScreenHeld) {
            drawIdleStatus();
        }
    }
}
