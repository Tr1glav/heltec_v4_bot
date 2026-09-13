#pragma once

#include <Arduino.h>

#ifdef MQTT_ENABLED
// Автообновление по релизам GitHub. Работает только потому, что в прошивке не осталось
// секретов: файлы релиза публичные, и качать их может кто угодно, включая нас.
struct FwLatest {
    String version;    // "0.1.55" из тега релиза
    String binUrl;     // прошивка для этой платы (по имени окружения сборки)
    String otazUrl;    // образ для сенсоров
    unsigned long checkedMs;
    bool valid;
};
extern FwLatest fwLatest;

// >0 если a новее b, 0 если равны, <0 если старее. Сравнение почастям, не строкой:
// "0.1.9" новее "0.1.10" только при строковом сравнении, что неверно.
int fwVersionCmp(const String& a, const String& b);

bool fwCheckLatest();                      // опросить GitHub, заполнить fwLatest
bool fwSelfUpdate(const String& url);      // скачать и прошить себя
bool fwFetchSensorImage(const String& url);// скачать .otaz в /ota.bin
const char* fwSensorEnvForBoard(const String& board);
void fwUpdateTick();                       // периодическая проверка и автообновление
#endif
