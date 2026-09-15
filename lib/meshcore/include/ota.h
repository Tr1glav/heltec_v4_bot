#pragma once

#include "config.h"

void slog(const char* fmt, ...);

// Потоковый поиск маркера платы (FW_MARKER) в образе прошивки. Образ приходит кусками,
// маркер может лечь на границу двух кусков — поэтому храним хвост предыдущего.
struct FwScan {
    char carry[40];    // хвост предыдущего куска
    uint8_t carryLen;
    bool mine;         // встретился маркер нашей платы
    char other[12];    // код чужой платы, если встретился
};
// Кадр сырого протокола прошивки: [BE EF][тип][seq 4][данные][crc16]. Собирают и шлют
// обе стороны, поэтому объявления живут здесь, а не внутри файла одной из сторон.
int rawBuildFrame(uint8_t* frm, uint8_t type, uint32_t seq, const uint8_t* data, int n);
int rawTxFrame(const uint8_t* frm, int f, bool listenAfter = true);

void fwScanReset(FwScan* s);
void fwScanFeed(FwScan* s, const uint8_t* data, size_t n);
// 1 — образ нашей платы, 0 — маркера нет (сборка старше проверки), -1 — чужая плата
int fwScanVerdict(const FwScan* s);
#ifdef MQTT_ENABLED
void logGetSnapshot(String& tailOut, uint32_t& totalOut);
void otaTxGroup(const String& msg);
void otaBotAbort(const char* why);
void otaDrawProgress();
void otaSendStart();
void otaSendEnd();
void otaHandleAck();
void otaBotTick();
void otaInspectStoredFw();
bool otaSessionActive();
// Запуск прошивки сенсора без участия веб-запроса — нужен автообновлению
bool otaStartSession(const String& target);
String buildDiagReport();
void setupOtaServer();
#endif
#if FEATURE_MESH_OTA_RECEIVER
void otaSensorDraw();
void otaSensorAbort(const char* why);
void otaSensorTick();
void otaSensorHandle();
// Разбор входящего кадра на стороне узла — зовёт диспетчер в ota.cpp
void otaHandleRawSensor(const uint8_t* buf, int len);
#endif
void otaHandleRawFrame(const uint8_t* buf, int len);
