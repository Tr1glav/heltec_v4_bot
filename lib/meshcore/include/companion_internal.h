#pragma once

#include "config.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>

// Внутренний заголовок компаньона: коды протокола и то, что делят между собой транспорт
// BLE с хранилищем контактов (companion.cpp) и разбор кадров приложения
// (companion_proto.cpp). Наружу это не нужно — публичный интерфейс в companion.h.

#if FEATURE_COMPANION

// --- протокол: коды из docs/companion_protocol.md оригинального MeshCore ---
#define CMD_APP_START              1
#define CMD_SEND_TXT_MSG           2
#define CMD_SEND_CHANNEL_TXT_MSG   3
#define CMD_GET_CONTACTS           4
#define CMD_GET_DEVICE_TIME        5
#define CMD_SET_DEVICE_TIME        6
#define CMD_SEND_SELF_ADVERT       7
#define CMD_SET_ADVERT_NAME        8
#define CMD_SYNC_NEXT_MESSAGE     10
#define CMD_SET_RADIO_PARAMS      11
#define CMD_GET_BATT_AND_STORAGE  20
#define CMD_DEVICE_QUERY          22
#define CMD_GET_CHANNEL           31
#define CMD_SET_CHANNEL           32
#define CMD_ADD_UPDATE_CONTACT     9
#define CMD_RESET_PATH            13
#define CMD_REMOVE_CONTACT        15
#define CMD_GET_CONTACT_BY_KEY    30
#define CMD_GET_ADVERT_PATH       42
#define CMD_SET_FLOOD_SCOPE_KEY   54
#define CMD_GET_DEFAULT_FLOOD_SCOPE 64

#define RESP_CODE_OK                0
#define RESP_CODE_ERR               1
#define RESP_CODE_CONTACTS_START    2
#define RESP_CODE_CONTACT           3
#define RESP_CODE_END_OF_CONTACTS   4
#define RESP_CODE_SELF_INFO         5
#define RESP_CODE_SENT              6
#define RESP_CODE_CURR_TIME         9
#define RESP_CODE_NO_MORE_MESSAGES 10
#define RESP_CODE_BATT_AND_STORAGE 12
#define RESP_CODE_DEVICE_INFO      13
#define RESP_CODE_CHANNEL_MSG_RECV 8    // формат для приложений до версии 3
#define RESP_CODE_CHANNEL_MSG_RECV_V3 17
#define RESP_CODE_CHANNEL_INFO     18
#define RESP_CODE_ADVERT_PATH      22
#define RESP_CODE_DEFAULT_FLOOD_SCOPE 28
#define PUSH_CODE_ADVERT         0x80    // знакомый узел объявился снова
#define PUSH_CODE_MSG_WAITING    0x83
#define PUSH_CODE_NEW_ADVERT     0x8A    // узел услышан впервые, кадр целиком

// Кадр ошибки в оригинале — два байта: RESP_CODE_ERR и причина
#define ERR_CODE_UNSUPPORTED_CMD    1
#define ERR_CODE_NOT_FOUND          2
#define ERR_CODE_TABLE_FULL         3
#define ERR_CODE_ILLEGAL_ARG        6

#define ADV_TYPE_CHAT              1
// Признаки в первом байте поля адверта. Порядок полей за ним строгий и зависит от
// признаков: координаты, два необязательных поля, и только потом имя.
#define ADV_LATLON_MASK         0x10
#define ADV_FEAT1_MASK          0x20
#define ADV_FEAT2_MASK          0x40
#define ADV_NAME_MASK           0x80
#define COMPANION_VER_CODE        13      // версия протокола, которую мы заявляем
#define COMPANION_FW_NAME         "meshcore-fork " FW_VERSION
#define MAX_FRAME_SIZE           176
#define MSG_QUEUE_MAX              8
// Кадры от приложения разбираются в главном цикле, а приходят пачкой из колбэка BLE:
// без очереди вторая команда затирала первую, и она пропадала молча.
#define IN_QUEUE_MAX               6

// UUID сервиса Nordic UART — именно по ним приложение ищет устройство
#define NUS_SERVICE "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"   // приложение -> прошивка
#define NUS_TX      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"   // прошивка -> приложение

struct QueuedMsg {
    uint8_t channelIdx;
    uint8_t pathLen;
    int8_t snr4;          // SNR, умноженный на 4 — так требует протокол
    uint32_t ts;
    // Столько текста несёт кадр сообщения: MAX_FRAME_SIZE минус 11 байт заголовка.
    // При 100 байтах длинные сообщения канала обрезались, хотя место в кадре было.
    char text[MAX_FRAME_SIZE - 11 + 1];
};

// Запись в NVS откладываем: адверты приходят пачками, а каждая запись во флеш —
// это износ и задержка. Сохраняем, когда поток утих.
#define CONTACTS_SAVE_DELAY_MS 5000

// --- состояние, общее для транспорта и разбора кадров ---
extern volatile bool blePaired;
extern QueuedMsg msgQueue[];
extern uint8_t msgHead, msgCount;
extern uint8_t inQueue[][MAX_FRAME_SIZE];
extern uint8_t inQueueLen[];
extern volatile uint8_t inHead, inCount;
extern portMUX_TYPE inMux;
extern uint32_t inDropped;

struct Contact {
    uint8_t  pub[32];
    uint8_t  type;
    uint8_t  flags;
    uint8_t  outPathLen;        // 0xFF — путь неизвестен, отвечаем флудом
    uint8_t  outPath[64];
    char     name[32];
    uint32_t lastAdvert;        // по часам самого узла
    int32_t  lat, lon;
    uint32_t lastmod;           // по нашим часам: по нему приложение до-синхронизируется
    uint8_t  advPathLen;        // путь, которым пришёл последний адверт (байт длины как в эфире)
    uint8_t  advPath[64];       // сами хэши ретрансляторов — их приложение и показывает
};
extern Contact contacts[];
int  contactFrame(uint8_t code, const Contact& c, uint8_t* buf);

// --- контакты и каналы: хранилище в companion.cpp, ими пользуется и разбор кадров ---
void sendFrameToApp(const uint8_t* data, size_t len);
int  contactFind(const uint8_t* pub);
void contactTouch();
void contactsIterStep();
void contactsSave();
extern bool contactsDirty;
extern unsigned long contactsDirtyMs;
void appChannelsSave();
extern int contactIterIdx, appChanBase;
extern uint32_t contactIterSince, contactIterNewest;
extern uint8_t contactCount;

#endif // FEATURE_COMPANION
