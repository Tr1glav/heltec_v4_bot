#include "config.h"
#include "globals.h"
#include "crypto.h"
#include "radio.h"
#include "mesh.h"
#include "ota.h"
#include "display.h"
#ifdef MQTT_ENABLED
#include "mqtt.h"
#endif

// ===== Приём из эфира =====
// Выделено из главного цикла: опрос радио, разбор кадров и поддержание приёмника.
// Здесь же сброс АРУ по расписанию и разбор сырых кадров быстрого канала во время
// прошивки по радио — всё, что связано с приёмом, собрано в одном месте.

void radioRxTick() {
    // ===== ПРИЁМ =====
    if (isListening) {
        // Периодический сброс AGC, если не идёт приём пакета прямо сейчас.
        // Не сбрасываем, пока стоит RX_DONE (иначе потеряем пакет).
        bool rxPending = (radio.getIrqFlags() & RADIOLIB_SX126X_IRQ_RX_DONE) != 0;
        if (!rxPending && (millis() - lastReArmMs > RADIO_REARM_INTERVAL_MS)) {
            rearmRadioAGC();
            rxPending = (radio.getIrqFlags() & RADIOLIB_SX126X_IRQ_RX_DONE) != 0;
        }

        // читать только если радио действительно получило пакет (RX_DONE)
        if (rxPending) {
            uint8_t buffer[256];
            int state = radio.readData(buffer, sizeof(buffer));
            if (state == RADIOLIB_ERR_NONE) {
                int pktLen = radio.getPacketLength();
                float rssi = radio.getRSSI();
                float snr = radio.getSNR();
                // в fast-режиме лог каждого кадра стоит миллисекунды UART на кадр
                if (pktLen > 0 && !otaFastMode) {
                    char sr[12], ss[12];
                    Serial.printf("\n[RX] len=%d RSSI=%s SNR=%s ", pktLen,
                                  fmtFix(rssi, 1, sr, sizeof(sr)), fmtFix(snr, 1, ss, sizeof(ss)));
                    for (int i = 0; i < min(pktLen, 24); i++) Serial.printf("%02X", buffer[i]);
                    Serial.println();
                }
                if (pktLen > 0 && otaFastMode &&
                    buffer[0] == RAW_MAGIC0 && buffer[1] == RAW_MAGIC1) {
                    // mesh OTA: сырые кадры вне meshcore
                    fastRxFrames++;
                    otaRawDidTx = false;
                    otaHandleRawFrame(buffer, pktLen);
                    lastReArmMs = millis();
                    if (!otaRawDidTx) radio.startReceive();
                } else if (checkAndMarkSeen(buffer, pktLen)) {
                    duplicateCount++;
                    Serial.printf("[DUP] skipped (total dups=%lu)\n", duplicateCount);
                } else {
                    bool parsed = parseMeshCorePacket(buffer, pktLen);

                    // hex-экран только для GRP_TXT, который не расшифровался
                    // (рекламные/служебные пакеты экран не трогаем)
                    #ifndef SENSOR_NODE
                    if (pktLen > 0 && !parsed && !otaFastMode && ((buffer[0] >> 2) & 0x0F) == 0x05) {
                        display.setTextSize(1);
                        display.clearDisplay();
                        display.setCursor(0, 0);
                        char dr[12], ds[12];
                        display.printf("RX %dB RSSI:%s\n", pktLen, fmtFix(rssi, 0, dr, sizeof(dr)));
                        display.printf("SNR:%s pkts:%d\n", fmtFix(snr, 0, ds, sizeof(ds)), packetCount);
                        display.printf("hex:");
                        for (int i = 0; i < min(pktLen, 21); i++) display.printf("%02X", buffer[i]);
                        display.display();
                        lastRxDisplay = millis();
                    }
                    #endif

                    // не перезатираем экран 5 сек после сообщения
                    if (parsed) {
                        lastRxDisplay = millis();
                        #ifdef MQTT_ENABLED
                        publishMessage();
                        #endif
                    }
                    lastReArmMs = millis();  // был приём — сброс AGC откладываем
                    radio.startReceive();
                }
            } else {
                // Захват сорвался (CRC и т.п.) — флаг RX_DONE мог остаться,
                // что приведёт к бесконечному циклу. Сбрасываем флаги и ре-армим.
                Serial.printf("[RX] readData error %d, re-arming\n", state);
                if (otaFastMode) fastRxErrors++;
                radio.clearIrqStatus();
                rearmRadioAGC();
            }
        }
    }
}
