#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <RadioLib.h>
#include <SPI.h>
#include <mbedtls/md.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <Ed25519.h>
#include <ed_25519.h>
#include <string.h>
#include <stdarg.h>
#include <sys/time.h>
#include <time.h>
#include <Update.h>

#if defined(MQTT_ENABLED) || defined(COMPANION_NODE)
#include <LittleFS.h>        // компаньон держит в файле список контактов
#endif

#ifdef MQTT_ENABLED
#include <WiFi.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <esp_sntp.h>
#include <esp_partition.h>
#endif

#include "board_config.h"
// Роли разложены на независимые признаки: features.h выводит их из ролевых флагов,
// если окружение не задало иначе. Подключается после board_config.h — часть признаков
// опирается на свойства платы (например HAS_OLED).
#include "features.h"
// Имя узла, каналы, WiFi и MQTT берутся из NVS (cfg), а не из build-флагов
#include "appconfig.h"

// Часовой пояс
#define TZ_OFFSET_HOURS 3
#define LOCAL_TZ "Europe/Moscow UTC+3"

// ===== КАНАЛЫ =====
#define MAX_CHANNELS 8        // 4 своих + место для каналов, добавленных из приложения
// Открытый текст группового сообщения [ts 4][type 1][«имя: »][текст]:
// шифр ≤ 240 + MAC 2 + заголовок 3 ≤ 255 Б (лимит кадра SX1262)
#define GROUP_TEXT_MAX_PLAIN 240
// Столько текста влезает в личное сообщение: [время 4][попытка 1][текст][0] шифруется
// блоками по 16, плюс заголовок 4 и MAC 2 — итог укладывается в кадр SX1262 (255 Б).
#define DM_TEXT_MAX 200

// ---- Сенсорные сообщения ----
#define SENSOR_MSG_BUTTON "button"
#define SENSOR_MSG_BUTTON2 "button2"
#define SENSOR_MSG_HELLO   "hello"
#define SENSOR_MSG_HELLO_REQ "hello?"   // бот просит сенсоры отметиться (кнопка на странице OTA)
// Тройное нажатие кнопки на сенсоре: эхо-запрос к координатору для проверки связи.
// Запрос уходит ОДИНОЧНОЙ посылкой — иначе во время ответа мерялись бы повторы.
#define SENSOR_MSG_PING     "ping:"      // сенсор -> координатор: ping:<номер>
#define SENSOR_MSG_PONG     "pong:"      // координатор -> сенсор: pong:<номер>:<rssi>:<snr>
// Пауза перед ответом на /ping и на личное сообщение. Отправитель шлёт своё сообщение
// не один раз, а повторами, и пока он передаёт — радио полудуплексное — он ничего не
// слышит. Ответ, посланный раньше, приходится ровно на эти повторы и пропадает.
// Значение покрывает повторы отправителя с запасом.
// Пауза перед ответом на пинг и личные сообщения. Случайная и с запасом: пока отправитель
// доканчивает свои повторы, он нас не слышит, а при одинаковой паузе несколько узлов
// отвечают одновременно и глушат друг друга.
#define PING_REPLY_DELAY_MIN_MS 1500
#define PING_REPLY_DELAY_MAX_MS 3500
// Пауза перед ответом на опрос "hello?" со страницы: разводит ответы узлов по времени.
#define HELLO_REPLY_DELAY_MIN_MS 1500
#define HELLO_REPLY_DELAY_MAX_MS 9000
#define PING_TIMEOUT_MS     5000UL       // не дождались ответа — показать это на экране
#define PING_SHOW_MS        15000UL      // сколько держать результат на экране
#ifndef SNS_BTN_DBL_WINDOW_MS
#define SNS_BTN_DBL_WINDOW_MS 700
#endif
// Нажатие короче секунды — это триггер (button/button2/проверка связи). Длинное
// нажатие триггером не считается вовсе, а от двух секунд будит погасший экран:
// иначе случайное удержание кнопки в кармане слало бы сообщения в сеть.
#define BTN_TRIGGER_MAX_MS 1000
#define BTN_WAKE_MS        2000

// ===== ЭНЕРГОСБЕРЕЖЕНИЕ (узлы на аккумуляторе; координатор питается от сети) =====
// Панель ест 15–25 мА и большую часть времени никем не читается.
#define SCREEN_IDLE_OFF_MS (2UL * 60 * 1000)
// На 80 МГц BLE и LoRa работают штатно, а ток меньше на 20–30 мА. Ниже 80 МГц
// радиоподсистема не работает, поэтому это и есть нижняя граница. На время прошивки
// по воздуху частота поднимается обратно: там кадры идут каждые 8 мс.
#define CPU_MHZ_IDLE 80
#define CPU_MHZ_FAST 240
#ifndef SENSOR_HEARTBEAT_MS
#define SENSOR_HEARTBEAT_MS (10UL * 60 * 1000)
#endif

// Пауза без сообщений от датчика, после которой его availability уходит в offline
#ifndef SENSOR_OFFLINE_MS
#define SENSOR_OFFLINE_MS (15UL * 60 * 1000)
#endif

// ===== ТЕМПЕРАТУРА =====
#ifndef TEMP_SENSOR_OFFSET
#define TEMP_SENSOR_OFFSET 0
#endif

// ===== ОТВЕТ НА /ping =====
#define MAX_REPLY_PATH 63

// ===== КЭШ ПУБЛИЧНЫХ КЛЮЧЕЙ НОД =====
#define PEER_CACHE_MAX 8
struct PeerEntry {
    uint8_t hash;
    uint8_t pub[32];
    uint32_t last_seen;
};

// ===== ДЛИНА ПУТИ В КАДРЕ =====
// Старшие два бита байта path_len задают размер хэша одного ретранслятора, младшие шесть —
// число хопов. Вся сеть обязана считать одинаково, поэтому значение одно на все платы:
// два байта на хоп (меньше совпадений хэшей), при пути в 64 байта это максимум 32 хопа.
#define PATH_HASH_SIZE 2
#define PATH_LEN_INIT  ((uint8_t)((PATH_HASH_SIZE - 1) << 6))   // 0 хопов, хэш нужного размера

// Список узлов, который видит приложение. Лежит не в NVS (там всего 20 КБ на весь
// раздел, вместе с настройками), а файлом в LittleFS, поэтому предел задаёт только
// массив в памяти: 200 записей — это около 43 КБ при 320 КБ на плате.
#define COMPANION_MAX_CONTACTS 200

// ===== ИДЕНТИЧНОСТЬ НОДЫ ДЛЯ ADVERT =====
#define ADVERT_PERIOD_MS        (5UL * 60 * 1000)
#define ADVERT_FLOOD_PERIOD_MS (30UL * 60 * 1000)
#define ADV_ROUTE_DIRECT 0x02
#define ADV_ROUTE_FLOOD  0x01

// ===== MQTT / HOME ASSISTANT =====
#ifdef MQTT_ENABLED
#define MQTT_STATUS_INTERVAL_MS  60000
#define MQTT_RECONNECT_INTERVAL_MS 5000
#define NTP_RESYNC_INTERVAL_MS (60UL * 60 * 1000)
#define SENSOR_TIME_SYNC_INTERVAL_MS (5UL * 60 * 1000)
#define LASTMSG_RESET_MS 4000
#define SNS_BTN_CLEAR_MS 500
#endif

// ===== STRAZH: LORA AGC rearm =====
#define RADIO_REARM_INTERVAL_MS 30000

// ===== ДЕДУПЛИКАЦИЯ =====
#define SEEN_HASH_SIZE 8
#define SEEN_HASH_COUNT 64
#define SEEN_ADVERT_HASH_COUNT 16

// ===== МESH OTA =====
// Сколько ждём квитанцию на пачку. Узел отвечает не сразу: получив последний кадр
// пачки или опрос, он сначала записывает во флеш до OTA_WINDOW чанков с распаковкой
// на лету, и лишь потом шлёт ответ. На крупных образах это занимает больше прежних
// 400 мс, и сессия рвалась по несуществующей потере связи при нуле ошибок приёма.
#define OTA_ACK_TIMEOUT_MS 1200
// ota:start идёт на штатном SF8/BW62.5: пакет ~0.5 с в эфире, ответ сенсора ~0.4 с — 400 мс не хватает
#define OTA_START_TIMEOUT_MS 3000
// финал: Update.end() на сенсоре проверяет весь образ перед ответом
#define OTA_END_TIMEOUT_MS 3000
#define OTA_MAX_RETRIES 4
#define OTA_MAX_FW_BYTES (3UL * 1024 * 1024)
#define OTA_DRAW_MS 1000
#define OTA_FAST_FREQ       868.950
// Ответ сенсора на ota:start; число — версия протокола mesh OTA
#define OTA_ACKSTART        "ota:ackstart:1"
// сенсор шлёт ackstart трижды (~1.3 с на SF8) и переключается только после третьей копии
#define OTA_FAST_SETTLE_MS  1200
// Быстрый канал — GFSK. Параметры связаны между собой: полоса приёмника должна
// покрывать 2*(девиация + скорость/2) — правило Карсона, — а девиация задаёт индекс
// модуляции 2*dev/br, который для надёжного приёма держат около единицы.
// Прошлая попытка поднять скорость до 250 кбит/с оставила девиацию 50 кГц и полосу
// 234 кГц: индекс падал до 0.4, а полоса была вдвое уже нужной — связь и разваливалась.
#define OTA_FSK_BR          250.0   // кбит/с
#define OTA_FSK_DEV         125.0   // кГц: индекс модуляции 1.0
#define OTA_FSK_RXBW        467.0   // кГц: ближайшее к 2*(125+125)=500, что умеет SX1262
#define OTA_FSK_PREAMBLE    32      // бит
// Чанков в пачке до подтверждения. Предел — 16: маска подтверждений uint16.
// При 8 на каждые 8 кадров приходился обмен квитанцией, и в замере выходило 3.8 КБ/с
// при теоретических 12.5 КБ/с для GFSK 100 кбит/с — ждём ответ вдвое реже.
#define OTA_WINDOW          16
// пауза между кадрами пачки: сенсор должен вычитать кадр и вернуться в RX до следующего
// Измерено на стенде: при 8 мс сессия занимает 56.7 с, при 4 мс — 61.6 с. Меньшая пауза
// экономит 4.4 с ожидания, но сенсор перестаёт успевать: кадров уходит 1259 вместо 1177,
// опросов 10 вместо 4, а каждый опрос стоит 400 мс. Это граница возможностей приёмника.
#define OTA_BURST_GAP_MS    8

// Сенсорная сторона OTA
#define OTA_SENSOR_STALL_MS 60000
// без первого чанка сенсор возвращается на штатный канал; бот выжидает это время перед повтором
#define OTA_SENSOR_FIRST_CHUNK_MS 5000
// Весь образ принят, остался фрейм DONE. Бот повторяет его каждые OTA_END_TIMEOUT_MS и
// сдаётся после OTA_END_TIMEOUT_MS*(OTA_MAX_RETRIES+2) ≈ 18 с. Этот сторож кроет то же
// окно с запасом: потерялся DONE — сенсор сам возвращается на штатный канал, а не сидит
// весь OTA_SENSOR_STALL_MS (60 с) в быстром, глухой к сети.
#define OTA_SENSOR_END_MS   20000

// ===== ЧИСТЫЙ LoRa OTA (сырые фреймы вне meshcore) =====
// Данные шлются напрямую radio.transmit/readData на быстрой конфигурации
// (OTA_FAST_*), без группового шифрования и флуд-маршрутизации.
// Формат кадра:
//   [magic0][magic1][type][seq4 LE][data...][crc16 2B LE]
// Фрейм <= 255 Б (лимит SX1262), служебных 11 Б (в DATA ещё MAC 2 Б перед шифром).
#define OTA_RAW_CHUNK_BYTES 240
#define OTA_RAW_FRAME_MAX   (11 + OTA_RAW_CHUNK_BYTES)   // заголовок 7 + MAC 2 + шифр + crc16 2
#define RAW_MAGIC0  0xBE
#define RAW_MAGIC1  0xEF
// бот -> сенсор
#define RAW_TYPE_DATA  0x02
#define RAW_TYPE_DONE  0x03
#define RAW_TYPE_ABORT 0x04
#define RAW_TYPE_DATA_LAST 0x05   // последний кадр пачки, ответить WACK
#define RAW_TYPE_POLL  0x06       // запрос WACK
// сенсор -> бот
#define RAW_TYPE_DONE_ACK 0x83
#define RAW_TYPE_FAIL    0x84
#define RAW_TYPE_WACK    0x85     // seq = первый недостающий чанк, данные = маска 2B LE следующих
// Сжатый файл прошивки (scripts/copy_firmware.py): [OTAZ][размер образа 4B LE][CRC32 образа 4B LE][zlib]
#define OTA_Z_MAGIC "OTAZ"
#define OTA_Z_HDR   12
// Нули, дописываемые в эфир после zlib-потока. Узлы до 0.2.36 распаковывают каждый чанк
// с флагом «вход ещё будет» и на последнем придерживают хвост образа: распаковщику не
// хватает бит в буфере на последние символы. Эти байты дают ему слабину — он доходит до
// конца потока, а лишнее не трогает. Так обновляются и узлы со старой прошивкой.
#define OTA_Z_TAIL_PAD 64
// Размер блока ФС LittleFS. Держим просто как справку: запись, ровно равная блоку, на этой
// плате до флеша не доезжает (хвост образа замирал на границе блока), поэтому скачанный
// образ пишется кусками сети как есть, а потерянный хвост ловится сверкой размера файла.
#define FS_BLOCK_BYTES 4096

// ===== КАНАЛЫ: struct =====
struct MeshChannel {
    uint8_t secret[32];
    uint8_t hash;
    const char* name;
};

// ===== OTA_PHASE =====
enum {
    OTA_PHASE_IDLE = 0,
    OTA_PHASE_WAIT_START,
    OTA_PHASE_DATA,
    OTA_PHASE_WAIT_END,
    OTA_PHASE_DONE
};

// ===== ФЛУД-ОТПРАВКА =====
#ifndef FLOOD_RETRY_MS
#define FLOOD_RETRY_MS 60     // пауза между повторами флуда
#endif

// FW_VERSION и BUILD_UNIX_TIME генерирует scripts/gen_version.py перед сборкой
#include "build_info.h"

// ===== МАРКЕР ПЛАТЫ В ОБРАЗЕ =====
// Каждая прошивка несёт строку MBFW:<код платы>:<версия>. Принимающая сторона ищет её
// в загружаемом образе и отказывается ставить прошивку от другой платы: перепутанный
// файл не запустится, а снимается такой «кирпич» только USB-кабелем.
#define FW_MARK_PREFIX "MBFW:"
#define FW_MARKER      FW_MARK_PREFIX BOARD_CODE ":" FW_VERSION

// Сколько сенсор ждёт подтверждающий "save" после правок настроек по радио. Не дождался —
// перезагружается и возвращается к сохранённым настройкам, чтобы не остаться в
// полуизменённом состоянии, если связь с ботом оборвалась на середине.
#define CFG_PENDING_REVERT_MS (120UL * 1000)

// ===== АВТООБНОВЛЕНИЕ ПО РЕЛИЗАМ =====
// Прошивка не содержит секретов, поэтому файлы релиза лежат открыто и качаются без токена.
#ifndef FW_RELEASE_API
#define FW_RELEASE_API "https://api.github.com/repos/Tr1glav/meshcore-fork/releases/latest"
#endif
#define FW_CHECK_INTERVAL_MS (6UL * 60 * 60 * 1000)   // раз в 6 часов
// Адрес файла собирается из имени окружения узла: <окружение>_v<версия>.otaz
#ifndef FW_RELEASE_DL
#define FW_RELEASE_DL "https://github.com/Tr1glav/meshcore-fork/releases/download/"
#endif
// После обновления одного сенсора следующий ждёт не полный цикл, а этот срок: очередь
// разбирается быстро, но по одному — эфир и сессия прошивки всё равно одни на всех.
#define FW_RECHECK_AFTER_MS  (10UL * 60 * 1000)
#ifndef FW_ENV
#define FW_ENV "unknown"
#endif

// ===== Кэш имён датчиков =====
#define SENSOR_DEV_CACHE_MAX 16

// ===== LOG_TAIL =====
#define LOG_TAIL_MAX 4000
