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

#ifdef MQTT_ENABLED
#include <WiFi.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <esp_sntp.h>
#include <esp_partition.h>
#endif

#include "board_config.h"

// Имя устройства (идёт в заголовке исходящих сообщений)
#ifndef DEVICE_NAME
#define DEVICE_NAME "Tr1glav_esp_bot"
#endif
#if defined(SENSOR_NODE) && defined(DEVICE_NAME_SENSOR)
#undef DEVICE_NAME
#define DEVICE_NAME DEVICE_NAME_SENSOR
#endif

// Часовой пояс
#define TZ_OFFSET_HOURS 3
#define LOCAL_TZ "Europe/Moscow UTC+3"

// ===== КАНАЛЫ =====
#define MAX_CHANNELS 4

// ---- Сенсорные сообщения ----
#define SENSOR_MSG_BUTTON "button"
#define SENSOR_MSG_BUTTON2 "button2"
#define SENSOR_MSG_HELLO   "hello"
#ifndef SNS_BTN_DBL_WINDOW_MS
#define SNS_BTN_DBL_WINDOW_MS 700
#endif
#ifndef SENSOR_HEARTBEAT_MS
#define SENSOR_HEARTBEAT_MS (10UL * 60 * 1000)
#endif

// ===== ДЕФОЛТЫ ИЗ BUILD-ФЛАГОВ (secrets.ini) =====
#ifndef PRIVATE_CHANNEL_NAME
#define PRIVATE_CHANNEL_NAME ""
#endif
#ifndef PRIVATE_CHANNEL_KEY
#define PRIVATE_CHANNEL_KEY ""
#endif
#ifndef SENSOR_CHANNEL_NAME
#define SENSOR_CHANNEL_NAME ""
#endif
#ifndef SENSOR_CHANNEL_KEY
#define SENSOR_CHANNEL_KEY ""
#endif
#ifndef TX_CHANNEL
#define TX_CHANNEL "#connections"
#endif

// Пауза без сообщений от датчика, после которой его availability уходит в offline
#ifndef SENSOR_OFFLINE_MS
#define SENSOR_OFFLINE_MS (15UL * 60 * 1000)
#endif

// ===== АВТОВЫКЛЮЧЕНИЕ ЭКРАНА =====
#define SCREEN_AUTO_OFF_MS (5UL * 60 * 1000)

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
#define OTA_ACK_TIMEOUT_MS 400
// ota:start идёт на штатном SF8/BW62.5: пакет ~0.5 с в эфире, ответ сенсора ~0.4 с — 400 мс не хватает
#define OTA_START_TIMEOUT_MS 3000
// финал: Update.end() на сенсоре проверяет весь образ перед ответом
#define OTA_END_TIMEOUT_MS 3000
#define OTA_MAX_RETRIES 4
#define OTA_MAX_FW_BYTES (3UL * 1024 * 1024)
#define OTA_DRAW_MS 1000
#define OTA_FAST_FREQ       868.950
#define OTA_FAST_BW         500.0
#define OTA_FAST_SF         5       // при OTA_FAST_FSK 0
#define OTA_FAST_CR         5
// сенсор шлёт ackstart трижды (~1.3 с на SF8) и переключается только после третьей копии
#define OTA_FAST_SETTLE_MS  1200
// v3: 1 = GFSK 100 кбит/с (LoRa SF7/BW500 ~22 кбит/с, но чувствительность хуже), 0 = LoRa OTA_FAST_SF
#define OTA_FAST_FSK        1
#define OTA_FSK_BR          100.0   // кбит/с
#define OTA_FSK_DEV         50.0    // кГц
#define OTA_FSK_RXBW        234.3   // кГц
#define OTA_FSK_PREAMBLE    32      // бит
// v3: чанков в пачке до подтверждения (<= 16: маска uint16)
#define OTA_WINDOW          8
// пауза между кадрами пачки: сенсор должен вычитать кадр и вернуться в RX до следующего
#define OTA_BURST_GAP_MS    15

// Сенсорная сторона OTA
#ifdef SENSOR_NODE
#define OTA_SENSOR_STALL_MS 60000
#define OTA_SENSOR_FIRST_CHUNK_MS 5000
#endif

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
#define RAW_TYPE_DATA_LAST 0x05   // v3: последний кадр пачки, ответить WACK
#define RAW_TYPE_POLL  0x06       // v3: запрос WACK
// сенсор -> бот
#define RAW_TYPE_DONE_ACK 0x83
#define RAW_TYPE_FAIL    0x84
#define RAW_TYPE_WACK    0x85     // v3: seq = первый недостающий чанк, данные = маска 2B LE следующих
// Сжатый файл прошивки (scripts/copy_firmware.py): [OTAZ][размер образа 4B LE][CRC32 образа 4B LE][zlib]
#define OTA_Z_MAGIC "OTAZ"
#define OTA_Z_HDR   12

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
#define FLOOD_RETRY_MS 100
#endif

// Компилятором задавалось BUILD_UNIX_TIME из времени хоста
// FW_VERSION задаёт scripts/gen_version.py из version.txt
#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif
#ifndef BUILD_UNIX_TIME
#define BUILD_UNIX_TIME 0
#endif

// ===== Кэш имён датчиков =====
#define SENSOR_DEV_CACHE_MAX 8

// ===== LOG_TAIL =====
#define LOG_TAIL_MAX 4000
