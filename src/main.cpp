// MeshCore bot/listener + sensor node. setup()/loop() only —
// логика вынесена в модули (lib/meshcore): radio, mesh, ota, mqtt, display.
// Вручную поддерживаемый файл; никакой генерации из parts/.
#include "config.h"
#include "globals.h"
#include "crypto.h"
#include "radio.h"
#include "mesh.h"
#include "display.h"
#include "ota.h"
#include "mqtt.h"

void initSystemClock() {
    struct timeval tv;
    tv.tv_sec = BUILD_UNIX_TIME;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);

    // Локальное время считаем вручную: UTC + фиксированное смещение.
    time_t local = (time_t)BUILD_UNIX_TIME + (time_t)TZ_OFFSET_HOURS * 3600;
    struct tm tm_now;
    gmtime_r(&local, &tm_now);
    strftime(sysTimeStr, sizeof(sysTimeStr), "%Y-%m-%d %H:%M:%S", &tm_now);
    Serial.printf("[RTC] SysTime set from host: %s (%s)\n", sysTimeStr, LOCAL_TZ);
    Serial.printf("[RTC] epoch=%lld\n", (long long)time(NULL));
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== MESHCORE LISTENER ===\n");
    
    initSystemClock();
    
    // ===== ПИТАНИЕ ПЕРИФЕРИИ (VEXT) =====
    #if HAS_OLED && defined(VEXT_PIN)
    pinMode(VEXT_PIN, OUTPUT);
    digitalWrite(VEXT_PIN, VEXT_EN_ACTIVE);
    delay(300);
    #endif
    
    // ===== FEM (усилитель KCT8103L на V4) =====
    #if HAS_FEM
    Serial.println("Init FEM...");
    pinMode(FEM_VCC_PIN, OUTPUT);
    pinMode(FEM_EN_PIN, OUTPUT);
    pinMode(FEM_TX_PIN, OUTPUT);
    digitalWrite(FEM_VCC_PIN, HIGH);
    delay(10);
    digitalWrite(FEM_EN_PIN, HIGH);
    delay(10);
    digitalWrite(FEM_TX_PIN, LOW);  // RX
    delay(10);
    Serial.println("FEM OK");
    #endif
    
    // ===== OLED =====
    #if HAS_OLED
    pinMode(OLED_RESET, OUTPUT);
    digitalWrite(OLED_RESET, LOW);
    delay(10);
    digitalWrite(OLED_RESET, HIGH);
    delay(200);

    #if defined(SDA_PIN) && defined(SCL_PIN)
    Wire.begin(SDA_PIN, SCL_PIN);
    #else
    Wire.begin();
    #endif
    Wire.setClock(100000);   // 400k часть OLED-панелей V4 "мусорит" — снижаем

    // Сканер I2C: некоторые экземпляры Heltec V4 живут на 0x3D, а не 0x3C.
    // Полоски/мусор на экране часто = неверный адрес или ранний инит.
    uint8_t oledAddr = SCREEN_ADDRESS;
    bool addrFound = false;
    for (uint8_t a = 0x03; a < 0x78; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            Serial.printf("[I2C] device found at 0x%02X\n", a);
            if (a == 0x3C || a == 0x3D) {
                oledAddr = a;
                addrFound = true;
            }
        }
    }
    if (!addrFound) {
        Serial.printf("[OLED] no 0x3C/0x3D found, defaulting to 0x%02X\n", oledAddr);
    } else {
        Serial.printf("[OLED] address = 0x%02X\n", oledAddr);
    }

    if (!display.begin(SSD1306_SWITCHCAPVCC, oledAddr)) {
        // пробуем альтернативный адрес
        uint8_t alt = (oledAddr == 0x3C) ? 0x3D : 0x3C;
        if (!display.begin(SSD1306_SWITCHCAPVCC, alt)) {
            Serial.println("Display FAILED!");
            while (1) delay(1000);
        } else {
            Serial.printf("OLED ok at 0x%02X (alt)\n", alt);
        }
    } else {
        Serial.printf("OLED ok at 0x%02X\n", oledAddr);
    }

    // Принудительно очищаем 2 раза и снимаем dim — уже фактически
    // устраняет "полоски" начального мусора на SSD1306
    display.dim(false);
    display.clearDisplay();
    display.display();
    delay(50);
    display.clearDisplay();
    display.display();
    
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("MeshCore");
    display.println("V4.3 init...");
    display.display();
    #endif
    
    #if BUTTON_PIN >= 0
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    #endif
    
    // ===== SPI =====
    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
    
    // ===== RESET =====
    pinMode(LORA_RST, OUTPUT);
    digitalWrite(LORA_RST, LOW);
    delay(20);
    digitalWrite(LORA_RST, HIGH);
    delay(100);
    
    // ===== LORA =====
    if (!initLoRa()) {
        #if HAS_OLED
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("LoRa ERROR!");
        display.display();
        #endif
        while (1) {
            delay(1000);
            Serial.println("LoRa init FAILED");
        }
    }
    
    deriveChannels();
    initAdvertIdentity();

#ifdef MQTT_ENABLED
    loadTxChannel();
    // Сеть НЕ блокируем: WiFi/MQTT поднимаются фоном в loop (tickRetryConnections).
    // Радио начинает слушать сразу, без задержки на TCP/а-коннект.
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);   // без modem-sleep: меньше задержка отвёта
    // Дамп РЕАЛЬНОЙ таблицы разделов с устройства — для отладки LittleFS.
    // Если spiffs нет/другой офсет — mesh OTA работать не будет.
    {
        slog("[PART] flash chip size: %u KB\n", ESP.getFlashChipSize() / 1024);
        esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY,
                                                         ESP_PARTITION_SUBTYPE_ANY, NULL);
        for (; it != NULL; it = esp_partition_next(it)) {
            const esp_partition_t* p = esp_partition_get(it);
            slog("[PART] %-10s type=%d sub=%d off=0x%06X size=%uK\n",
                 p->label, (int)p->type, (int)p->subtype,
                 (unsigned)p->address, p->size / 1024);
        }
        if (it) esp_partition_iterator_release(it);
    }
    // хранилище /ota.bin для mesh OTA сенсоров (partition "spiffs", 1.5 MB)
    if (LittleFS.begin(true)) {
        slog("[LITTLEFS] OK\n");
        // Самотест: write/read/remove — доказывает, что ФС реально работает.
        {
            File t = LittleFS.open("/.selftest", "w");
            if (!t) {
                slog("[LITTLEFS] self-test: open(w) FAILED\n");
            } else if (t.write((const uint8_t*)"meshcore", 8) != 8) {
                slog("[LITTLEFS] self-test: write FAILED\n");
                t.close();
            } else {
                t.close();
                File t2 = LittleFS.open("/.selftest", "r");
                if (!t2) {
                    slog("[LITTLEFS] self-test: open(r) FAILED\n");
                } else {
                    char buf[16] = {0};
                    int n = t2.read((uint8_t*)buf, sizeof(buf) - 1);
                    t2.close();
                    slog("[LITTLEFS] self-test: read %d bytes = \"%s\"\n",
                         n, (n > 0) ? buf : "(fail)");
                }
                LittleFS.remove("/.selftest");
            }
        }
    } else {
        slog("[LITTLEFS] FAILED (begin) — mesh OTA недоступен\n");
    }
    if (LittleFS.exists("/ota.bin")) {
        File f = LittleFS.open("/ota.bin", "r");
        otaFwSize = f ? (uint32_t)f.size() : 0;
        otaFwReady = (otaFwSize > 0);
        if (f) f.close();
        slog("[OTA] /ota.bin: %u байт (mesh OTA ready=%d)\n",
             (unsigned)otaFwSize, (int)otaFwReady);
    }
    setupMQTT();            // только конфиг (префиксы/сервер/коллбэк)
    setupOtaServer();       // HTTP OTA на :3232 (обновление прошивки по WiFi)
    #endif

    radio.startReceive();
    isListening = true;
    lastDirectAdvertMs = lastFloodAdvertMs = millis();
    
    #if HAS_OLED
    display.clearDisplay();
    // Стартовая заставка: только имя устройства по центру экрана (128x64).
    display.setTextSize(2);                     // 12x16 симв.
    const char* splash = "MeshCore";
    display.setCursor((128 - (int)strlen(splash) * 12) / 2, (64 - 16) / 2);
    display.print(splash);
    display.setTextSize(1);
    display.display();
    #endif
    
    Serial.printf("Listening on %s...\n", channelListStr().c_str());
    lastScreenActivityMs = millis();   // старт таймера автовыключения экрана
}

void loop() {
    #ifdef MQTT_ENABLED
    otaServer.handleClient();   // HTTP OTA: принимаем реквесты не блокируя радио
    otaBotTick();               // mesh OTA: таймауты повтора чанков

    // ===== MQTT RECONNECT (неблокирующий, раз в 5 с) =====
    if (millis() - lastMqttReconnectMs > MQTT_RECONNECT_INTERVAL_MS) {
        lastMqttReconnectMs = millis();
        tickRetryConnections();
    }
    if (mqttConnected) mqtt.loop();
    #endif

    // ===== ADVERT (периодический) =====
    if (isListening && !otaFastMode) {
        if (!advertBootSent && millis() > 6000) {   // стартовый beacon
            advertBootSent = true;
            sendAdvert(ADV_ROUTE_DIRECT);
        }
        if (millis() - lastDirectAdvertMs >= ADVERT_PERIOD_MS) {
            lastDirectAdvertMs = millis();
            sendAdvert(ADV_ROUTE_DIRECT);
        }
        if (millis() - lastFloodAdvertMs >= ADVERT_FLOOD_PERIOD_MS) {
            lastFloodAdvertMs = millis();
            sendAdvert(ADV_ROUTE_FLOOD);
        }
    }

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
                if (pktLen > 0) {
                    Serial.printf("\n[RX] len=%d RSSI=%.1f SNR=%.1f ", pktLen, rssi, snr);
                    // hex-дамп опускаем в fast-режиме: каждый пакет — это ~8 мс на UART
                    if (!otaFastMode)
                        for (int i = 0; i < min(pktLen, 24); i++) Serial.printf("%02X", buffer[i]);
                    Serial.println();
                }
                if (pktLen > 0 && otaFastMode &&
                    buffer[0] == RAW_MAGIC0 && buffer[1] == RAW_MAGIC1) {
                    // чистый LoRa OTA: сырые фреймы вне meshcore
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
                    if (pktLen > 0 && !parsed && !otaFastMode && ((buffer[0] >> 2) & 0x0F) == 0x05) {
                        if (!screenOff) {
                            display.setTextSize(1);
                            display.clearDisplay();
                            display.setCursor(0, 0);
                            display.printf("RX %dB RSSI:%.0f\n", pktLen, rssi);
                            display.printf("SNR:%.0f pkts:%d\n", snr, packetCount);
                            display.printf("hex:");
                            for (int i = 0; i < min(pktLen, 21); i++) display.printf("%02X", buffer[i]);
                            display.display();
                        }
                        lastRxDisplay = millis();
                    }

// не перезатираем экран 5 сек после сообщения
                    if (parsed) {
                        lastRxDisplay = millis();
                        lastScreenActivityMs = millis();   // активность — отодвигает авто-гашение
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
                radio.clearIrqStatus();
                rearmRadioAGC();
            }
        }
    }

    // Показать статус на экране (обновляем раз в 500мс)
    if (isListening && !screenOff && (millis() - lastDisplayUpdate > 500)) {
        lastDisplayUpdate = millis();
        if (!otaFastMode && (millis() - lastRxDisplay > 5000)) {
            drawIdleStatus();
        }
    }

    #ifdef MQTT_ENABLED
    // ===== MQTT STATUS (раз в 60 сек) =====
    if (mqttConnected && millis() - lastStatusPublishMs > MQTT_STATUS_INTERVAL_MS) {
        lastStatusPublishMs = millis();
        publishStatus();
    }
    // ===== ДОСТУПНОСТЬ ДАТЧИКОВ: если от датчика давно ничего нет — offline =====
    if (mqttConnected && millis() - lastSensorAvailCheckMs > 10000) {
        lastSensorAvailCheckMs = millis();
        for (int i = 0; i < sensorDeviceDiscCount; i++) {
            if (sensorOnlineNow[i] && millis() - sensorLastActive[i] > SENSOR_OFFLINE_MS) {
                sensorOnlineNow[i] = false;
                char slug[48];
                mqttSlug(sensorDeviceDisc[i].c_str(), slug, sizeof(slug));
                char tAvail[128];
                snprintf(tAvail, sizeof(tAvail), "%s/sensor/%s/available", mqttPrefix, slug);
                mqtt.publish(tAvail, "offline", true);
                Serial.printf("[SNS] %s OFFLINE (no data for %lus)\n",
                              sensorDeviceDisc[i].c_str(), (unsigned long)(SENSOR_OFFLINE_MS / 1000));
            }
        }
    }
    // ===== Ре-синк NTP раз в час (configTime снова делает stop+init) =====
    if (ntpStarted && wifiConnected && (int32_t)(millis() - lastNtpSyncMs) >= (int32_t)NTP_RESYNC_INTERVAL_MS) {
        lastNtpSyncMs = millis();
        configTime(0, 0, "pool.ntp.org");
    }
    // ===== Первая успешная синхронизация SNTP: лог + сразу рассылка времени сенсорам =====
    // Отслеживаем статус lwIP SNTP (SNTP_SYNC_STATUS_COMPLETED), а не сдвиг часов:
    // при свежем билде реальное время может лишь на минуты отличаться от build-time.
    if (ntpStarted && !ntpSyncedLogged &&
        sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        ntpSyncedLogged = true;
        time_t local = time(NULL) + (time_t)TZ_OFFSET_HOURS * 3600;
        struct tm tm_now;
        gmtime_r(&local, &tm_now);
        char tbuf[32];
        strftime(tbuf, sizeof(tbuf), "%H:%M:%S %d.%m.%Y", &tm_now);
        Serial.printf("[RTC] NTP time synced: %s\n", tbuf);
        if (sensorChannelIdx >= 0) {
            lastSensorTimeSyncMs = millis();
            sendSensorTimeSync();
        }
    }
    // ===== Рассылка актуального времени сенсорам в сенсорный канал =====
    // Работает только при реально синхронизированном времени (ntpSyncedLogged):
    // build-time устаревает, и датчики должны получать истинный epoch.
    if (ntpSyncedLogged && sensorChannelIdx >= 0 &&
        (int32_t)(millis() - lastSensorTimeSyncMs) >= (int32_t)SENSOR_TIME_SYNC_INTERVAL_MS) {
        lastSensorTimeSyncMs = millis();
        sendSensorTimeSync();
    }
    // ===== Сброс lastmsg после паузы (чтобы повторный одинаковый текст триггерил HA) =====
    clearLastMsg();
    // ===== Сброс text-топика после триггера "button" (повторное нажатие = новый state_changed) =====
    clearSensorBtnText();
    #endif

    // Sensor node: button = trigger ("button"), hello = heartbeat раз в N минут
#ifdef SENSOR_NODE
    otaSensorTick();   // mesh OTA: сторожевое время — при зависании откат к прежней прошивке
    // Sensor node: button = trigger ("button"), hello = heartbeat раз в N минут
    // Во время OTA mesh-отправки подавляем: радио слушает raw-чанки на быстром канале.
    static unsigned long lastBtnPress = 0;
    static unsigned long lastHeartbeat = 0;
    static bool bootHelloSent = false;
    if (!otaActive) {
        if (!bootHelloSent) {
            bootHelloSent = true;
            sensorSendMsg(SENSOR_MSG_HELLO);   // стартовый hello сразу после включения
            // не упреждать первый периодический heartbeat после boot-привета
            lastHeartbeat = millis();
        } else if (millis() - lastHeartbeat >= SENSOR_HEARTBEAT_MS) {
            lastHeartbeat = millis();
            sensorSendMsg(SENSOR_MSG_HELLO);
        }
    }
    if (!otaActive && millis() - lastBtnPress > 5000) {
        if (digitalRead(BUTTON_PIN) == LOW) {
            lastBtnPress = millis();
            // ждём отпускания кнопки (таймаут защиты от залипания)
            unsigned long heldAt = millis();
            while (digitalRead(BUTTON_PIN) == LOW && millis() - heldAt < 5000) delay(2);

            // окно двойного нажатия: если в течение SNS_BTN_DBL_WINDOW_MS
            // кнопку нажмут снова — это button2
            bool isDouble = false;
            unsigned long waitUntil = millis() + SNS_BTN_DBL_WINDOW_MS;
            while (millis() < waitUntil) {
                if (digitalRead(BUTTON_PIN) == LOW) {
                    isDouble = true;
                    unsigned long heldAt2 = millis();
                    while (digitalRead(BUTTON_PIN) == LOW && millis() - heldAt2 < 5000) delay(2);
                    break;
                }
                delay(5);
            }
            sensorSendMsg(isDouble ? SENSOR_MSG_BUTTON2 : SENSOR_MSG_BUTTON);
        }
    }
#endif

    delay(10);
}
