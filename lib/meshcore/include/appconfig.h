#pragma once

#include <Arduino.h>

// Настройки развёртывания живут в NVS, а не в прошивке. Благодаря этому образ не
// содержит ни паролей, ни ключей каналов, ни имени узла: один и тот же файл подходит
// всем платам одного типа, его можно выкладывать куда угодно, а смена пароля WiFi или
// частоты сети не требует пересборки. Раздел nvs лежит отдельно от app0/app1, поэтому
// конфиг переживает обновление прошивки; стирает его только полное стирание флеша.
//
// Компилируются в прошивку только свойства железа: код платы, пины, наличие экрана и FEM.
struct AppConfig {
    String name;        // имя узла: адресат mesh OTA, отправитель сообщений, устройство в HA
    String wifiSsid;
    String wifiPass;
    String mqttHost;
    uint16_t mqttPort;
    String mqttUser;
    String mqttPass;
    String prvName;     // приватный канал
    String prvKey;      // base64 от 16 байт; пусто — автоключ по имени канала
    String snsName;     // канал сенсоров
    String snsKey;
    String txChannel;   // канал, в который уходят команды из Home Assistant

    // Параметры радио MeshCore. Все узлы сети обязаны совпадать по ним до бита,
    // иначе пакеты просто не будут приниматься.
    float loraFreq;     // МГц
    float loraBw;       // кГц
    uint16_t loraSf;    // фактор расширения 5..12
    uint16_t loraCr;    // коэффициент кодирования 5..8
    int16_t loraTx;     // мощность передатчика, дБм
    uint16_t loraPre;   // длина преамбулы, символов
    uint16_t loraSync;  // слово синхронизации, обычно 0x12

    int16_t tzOffset;   // часовой пояс, часы от UTC (для экрана и логов)
    uint16_t dispBri;   // яркость экрана 0..255
    uint16_t vextOn;    // уровень на пине питания периферии, включающий его: 1 = HIGH, 0 = LOW
    uint16_t autoUpd;   // 1 — сам ставить новые версии из релизов GitHub
};

extern AppConfig cfg;

// Читает конфиг из NVS, подставляя значения по умолчанию.
// true — устройство настроено (задано имя узла)
bool cfgLoad();
void cfgSave();
bool cfgReady();
// Печатает конфиг; пароли и ключи показываются только при secrets = true
void cfgPrint(bool secrets);
// Неблокирующая консоль настройки на USB-порту: "help" выводит список команд
void cfgConsoleTick();

// Перебор полей для веб-интерфейса: страница OTA показывает и правит тот же набор,
// что и консоль, без второго списка полей, который пришлось бы держать в синхронности.
int cfgFieldCount();
const char* cfgFieldName(int idx);
String cfgFieldValue(int idx, bool secrets);
bool cfgFieldSecret(int idx);
// Присваивает значение полю по имени; false — поля с таким именем нет
bool cfgApply(const String& field, const String& value);

#ifdef SENSOR_NODE
// Команда настройки, пришедшая по радио: часть сообщения после "cfg:<имя узла>:".
// Понимает "<поле>=<значение>", "save", "reboot" и "get".
void cfgHandleMeshCfg(const String& rest);
// Сторож несохранённых правок: без "save" сенсор перезагрузится к прежним настройкам
void cfgPendingTick();
#endif
