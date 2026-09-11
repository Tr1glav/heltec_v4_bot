#include "config.h"
#include "globals.h"
#include "crypto.h"
#include "mesh.h"
#include "mqtt.h"
#include "ota.h"

#ifdef MQTT_ENABLED
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    char msg[256];
    int mlen = min((unsigned int)255, length);
    memcpy(msg, payload, mlen);
    msg[mlen] = 0;

    // --- meshcore/bot/.../cmd/send ---
    if (strstr(topic, "/cmd/send")) {
        if (!isListening) {
            Serial.println("[MQTT] radio off, ignoring send cmd");
            return;
        }
        if (otaSessionActive()) {
            Serial.println("[MQTT] mesh OTA in progress, ignoring send cmd");
            return;
        }
        String txt = msg;
        uint8_t frame[300];
        int fl = buildGroupFrameFlood(mqttTxChannel, txt, frame, sizeof(frame), NULL, 0);
        if (fl > 0) {
            Serial.printf("[MQTT TX] %s: %s (%dB)\n", channels[mqttTxChannel].name,
                          (String(DEVICE_NAME) + ": " + txt).c_str(), fl);
            floodSend3(mqttTxChannel, frame, fl);
        }
        return;
    }

    // --- meshcore/bot/.../cmd/listening ---
    if (strstr(topic, "/cmd/listening")) {
        if (strncmp(msg, "ON", 2) == 0 && !isListening) {
            isListening = true;
            radio.startReceive();
            Serial.println("[MQTT] listening ON");
        } else if (strncmp(msg, "OFF", 3) == 0 && isListening) {
            isListening = false;
            radio.standby();
            Serial.println("[MQTT] listening OFF");
        }
        char stateTopic[96];
        snprintf(stateTopic, sizeof(stateTopic), "%s/state", mqttPrefix);
        mqtt.publish(stateTopic, isListening ? "ON" : "OFF", true);
        return;
    }

}

void publishDiscovery() {
    // Префикс HA: homeassistant
    // Device block общий для всех сущностей
    char devBlock[256];
    snprintf(devBlock, sizeof(devBlock),
        "\"identifiers\":[\"meshcore_bot_%s\"],"
        "\"name\":\"%s\","
        "\"manufacturer\":\"MeshCore\","
        "\"model\":\"ESP32-S3 Listener\","
        "\"sw_version\":\"" FW_VERSION "\"",
        DEVICE_NAME, DEVICE_NAME);

    char topic[128], payload[512];

    // --- Sensor: последний отправитель (state = имя отправителя) ---
    snprintf(topic, sizeof(topic), "homeassistant/sensor/meshcore_%s/last_sender/config", DEVICE_NAME);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s LastSender\","
        "\"state_topic\":\"%s/state\","
        "\"value_template\":\"{{ value_json.sender }}\","
        "\"json_attributes_topic\":\"%s/state\","
        "\"unique_id\":\"meshcore_%s_lastsender\","
        "\"device\":{%s}}",
        DEVICE_NAME, mqttPrefix, mqttPrefix, DEVICE_NAME, devBlock);
    mqtt.publish(topic, payload, true);

    // --- Sensor: статус ---
    snprintf(topic, sizeof(topic), "homeassistant/sensor/meshcore_%s/status/config", DEVICE_NAME);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s Status\","
        "\"state_topic\":\"%s/status\","
        "\"value_template\":\"{{ value_json.uptime }}\","
        "\"json_attributes_topic\":\"%s/status\","
        "\"unique_id\":\"meshcore_%s_stat\","
        "\"icon\":\"mdi:server\","
        "\"device\":{%s}}",
        DEVICE_NAME, mqttPrefix, mqttPrefix, DEVICE_NAME, devBlock);
    mqtt.publish(topic, payload, true);

    // --- Sensor: IP адрес _mqtt (атрибут ip из /status) ---
    snprintf(topic, sizeof(topic), "homeassistant/sensor/meshcore_%s/ip/config", DEVICE_NAME);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s IP\","
        "\"state_topic\":\"%s/status\","
        "\"value_template\":\"{{ value_json.ip }}\","
        "\"unique_id\":\"meshcore_%s_ip\","
        "\"icon\":\"mdi:ip-network\","
        "\"device\":{%s}}",
        DEVICE_NAME, mqttPrefix, DEVICE_NAME, devBlock);
    mqtt.publish(topic, payload, true);

    // --- Sensor: температура CPU (встроенный датчик ESP32-S3) ---
    snprintf(topic, sizeof(topic), "homeassistant/sensor/meshcore_%s/temp/config", DEVICE_NAME);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s Temp\","
        "\"state_topic\":\"%s/status\","
        "\"value_template\":\"{{ value_json.temp | float }}\","
        "\"unit_of_measurement\":\"°C\","
        "\"unique_id\":\"meshcore_%s_temp\","
        "\"icon\":\"mdi:thermometer\","
        "\"device\":{%s}}",
        DEVICE_NAME, mqttPrefix, DEVICE_NAME, devBlock);
    mqtt.publish(topic, payload, true);

    // --- Sensor: версия прошивки (поле version из /status) ---
    snprintf(topic, sizeof(topic), "homeassistant/sensor/meshcore_%s/version/config", DEVICE_NAME);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s Firmware\","
        "\"state_topic\":\"%s/status\","
        "\"value_template\":\"{{ value_json.version }}\","
        "\"unique_id\":\"meshcore_%s_version\","
        "\"icon\":\"mdi:chip\","
        "\"entity_category\":\"diagnostic\","
        "\"device\":{%s}}",
        DEVICE_NAME, mqttPrefix, DEVICE_NAME, devBlock);
    mqtt.publish(topic, payload, true);

    // --- Sensor: last_msg выбранного канала (для триггеров) ---
    snprintf(topic, sizeof(topic), "homeassistant/sensor/meshcore_%s/lastmsg/config", DEVICE_NAME);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s LastMsg\","
        "\"state_topic\":\"%s/lastmsg\","
        "\"force_update\":true,"
        "\"unique_id\":\"meshcore_%s_lastmsg\","
        "\"icon\":\"mdi:message-arrow-right\","
        "\"device\":{%s}}",
        DEVICE_NAME, mqttPrefix, DEVICE_NAME, devBlock);
    mqtt.publish(topic, payload, true);

    // --- Text: отправка сообщения ---
    snprintf(topic, sizeof(topic), "homeassistant/text/meshcore_%s/send/config", DEVICE_NAME);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s Send\","
        "\"command_topic\":\"%s/cmd/send\","
        "\"unique_id\":\"meshcore_%s_send\","
        "\"icon\":\"mdi:message-text\","
        "\"device\":{%s}}",
        DEVICE_NAME, mqttPrefix, DEVICE_NAME, devBlock);
    mqtt.publish(topic, payload, true);

    // --- Switch: listening ---
    snprintf(topic, sizeof(topic), "homeassistant/switch/meshcore_%s/listening/config", DEVICE_NAME);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s Listening\","
        "\"command_topic\":\"%s/cmd/listening\","
        "\"state_topic\":\"%s/lstate\","
        "\"unique_id\":\"meshcore_%s_sw\","
        "\"icon\":\"mdi:radio\","
        "\"device\":{%s}}",
        DEVICE_NAME, mqttPrefix, mqttPrefix, DEVICE_NAME, devBlock);
    mqtt.publish(topic, payload, true);

    Serial.printf("[MQTT] discovery published for <%s>\n", DEVICE_NAME);

    // Удаляем старый сенсор "Message" (переименован в LastSender): пустой
    // retained в его discovery-топике убирает сущность из HA. Только один раз,
    // при первой публикации discovery.
    if (!discoveryPublished) {
        char oldTopic[128];
        snprintf(oldTopic, sizeof(oldTopic),
                 "homeassistant/sensor/meshcore_%s/message/config", DEVICE_NAME);
        mqtt.publish(oldTopic, "", true);
    }
    discoveryPublished = true;
}

void publishMessage() {
    if (!mqttConnected) return;
    char escSender[64], escText[128], escChan[64], escRoute[128];
    jsonEscape(lastSender.c_str(), escSender, sizeof(escSender));
    jsonEscape(lastMessage.c_str(), escText, sizeof(escText));
    jsonEscape(lastChannelName.c_str(), escChan, sizeof(escChan));
    jsonEscape(lastPath[0] ? lastPath : "direct", escRoute, sizeof(escRoute));
    char topic[96], payload[384];
    snprintf(topic, sizeof(topic), "%s/state", mqttPrefix);
    snprintf(payload, sizeof(payload),
        "{\"sender\":\"%s\",\"text\":\"%s\",\"channel\":\"%s\","
        "\"rssi\":%.1f,\"snr\":%.1f,\"hops\":%d,\"route\":\"%s\"}",
        escSender, escText, escChan,
        lastRSSI, lastSNR, lastHopCount, escRoute);
    mqtt.publish(topic, payload);
    Serial.printf("[MQTT] message published\n");

    // last_msg: публикуется только для ВЫБРАННОГО в HA канала (для триггеров).
    // Значение меняется только когда пришло сообщение с канала mqttTxChannel.
    // Спустя LASTMSG_RESET_MS после публикации обнуляется: HA-триггер на
    // одинаковый текст срабатывает снова (состояние менялось /gate -> "" -> /gate).
    if (lastChannelIdx == mqttTxChannel && lastMessage.length() > 0) {
        char lmTopic[96];
        snprintf(lmTopic, sizeof(lmTopic), "%s/lastmsg", mqttPrefix);
        mqtt.publish(lmTopic, lastMessage.c_str(), true);
        lastmsgClearAt = millis() + LASTMSG_RESET_MS;
        lastmsgPendingClear = true;
        Serial.printf("[MQTT] lastmsg published (ch %s)\n", channels[mqttTxChannel].name);
    }
}

void clearLastMsg() {
    if (!lastmsgPendingClear || !mqttConnected) return;
    if ((int32_t)(millis() - lastmsgClearAt) < 0) return;   // ещё не время (учёт wrap)
    lastmsgPendingClear = false;
    char lmTopic[96];
    snprintf(lmTopic, sizeof(lmTopic), "%s/lastmsg", mqttPrefix);
    mqtt.publish(lmTopic, "", true);
    Serial.println("[MQTT] lastmsg cleared");
}

void clearSensorBtnText() {
    if (!snsBtnPendingClear || !mqttConnected) return;
    if ((int32_t)(millis() - snsBtnClearAt) < 0) return;
    snsBtnPendingClear = false;
    if (snsBtnSlug[0] == 0) return;
    char tText[128];
    snprintf(tText, sizeof(tText), "%s/sensor/%s/text", mqttPrefix, snsBtnSlug);
    mqtt.publish(tText, "", false);
    Serial.printf("[SNS] button text cleared for %s\n", snsBtnSlug);
    snsBtnSlug[0] = 0;
}

void mqttSlug(const char* name, char* out, int maxLen) {
    int n = 0;
    for (int i = 0; name[i] && n < maxLen - 1; i++) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') out[n++] = c;
        else out[n++] = '_';
    }
    out[n] = 0;
}

void publishSensorDisc(const String& sender, const char* slug) {
    if (!mqttConnected) return;
    char devBlock[160];
    snprintf(devBlock, sizeof(devBlock),
        "\"identifiers\":[\"meshcore_sensor_%s\"],\"name\":\"MeshBot Sensor %s\","
        "\"manufacturer\":\"MeshCore\",\"model\":\"Sensor node\"",
        slug, slug);
    char topic[128], payload[512];
    // Данные: sensor с текстовым state (HA MQTT text требует command_topic,
    // а у нас сущность read-only — это state от сенсора).
    snprintf(topic, sizeof(topic), "homeassistant/sensor/meshcore_sensor_%s/text/config", slug);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s data\",\"state_topic\":\"%s/sensor/%s/text\","
        "\"icon\":\"mdi:sprout\",\"unique_id\":\"meshcore_sensor_%s_text\","
        "\"entity_category\":\"diagnostic\","
        "\"device\":{%s}}",
        sender.c_str(), mqttPrefix, slug, slug, devBlock);
    mqtt.publish(topic, payload, true);
    Serial.printf("[MQTT] sensor device discovery: %s\n", slug);

    char retTopic[128], retPayload[512];
    snprintf(retTopic, sizeof(retTopic), "homeassistant/sensor/meshcore_sensor_%s/rssi/config", slug);
    snprintf(retPayload, sizeof(retPayload),
        "{\"name\":\"%s RSSI\",\"state_topic\":\"%s/sensor/%s/rssi\","
        "\"unit_of_measurement\":\"dBm\",\"device_class\":\"signal_strength\","
        "\"unique_id\":\"meshcore_sensor_%s_rssi\","
        "\"device\":{%s}}",
        sender.c_str(), mqttPrefix, slug, slug, devBlock);
    mqtt.publish(retTopic, retPayload, true);
    Serial.printf("[MQTT] sensor rssi discovery: %s\n", slug);

    // Availability: бинарник device_class=connectivity, «online» пока датчик шлёт.
    char alvTopic[128], alvPayload[512];
    snprintf(alvTopic, sizeof(alvTopic),
             "homeassistant/binary_sensor/meshcore_sensor_%s/available/config", slug);
    snprintf(alvPayload, sizeof(alvPayload),
        "{\"name\":\"%s available\",\"state_topic\":\"%s/sensor/%s/available\","
        "\"payload_on\":\"online\",\"payload_off\":\"offline\",\"device_class\":\"connectivity\","
        "\"unique_id\":\"meshcore_sensor_%s_available\","
        "\"device\":{%s}}",
        sender.c_str(), mqttPrefix, slug, slug, devBlock);
    mqtt.publish(alvTopic, alvPayload, true);
    Serial.printf("[MQTT] sensor availability discovery: %s\n", slug);

    char verTopic[128], verPayload[512];
    snprintf(verTopic, sizeof(verTopic), "homeassistant/sensor/meshcore_sensor_%s/version/config", slug);
    snprintf(verPayload, sizeof(verPayload),
        "{\"name\":\"%s firmware\",\"state_topic\":\"%s/sensor/%s/version\","
        "\"icon\":\"mdi:chip\",\"entity_category\":\"diagnostic\","
        "\"unique_id\":\"meshcore_sensor_%s_version\","
        "\"device\":{%s}}",
        sender.c_str(), mqttPrefix, slug, slug, devBlock);
    mqtt.publish(verTopic, verPayload, true);

    // Event entity: MQTT event platform — появляется как device-trigger "Fired"
    // в автоматизациях HA.  При нажатии кнопки сенсор шлёт "button", бот
    // публикует payload "button" в event_type_topic, HA генерирует событие.
    char evtTopic[128], evtPayload[512];
    snprintf(evtTopic, sizeof(evtTopic),
             "homeassistant/event/meshcore_sensor_%s/button/config", slug);
    snprintf(evtPayload, sizeof(evtPayload),
        "{\"name\":\"%s Button\","
        "\"state_topic\":\"%s/sensor/%s/button\","
        "\"event_types\":[\"button\",\"button2\"],"
        "\"unique_id\":\"meshcore_sensor_%s_button\","
        "\"device\":{%s}}",
        sender.c_str(), mqttPrefix, slug, slug, devBlock);
    mqtt.publish(evtTopic, evtPayload, true);
    Serial.printf("[MQTT] sensor button event discovery: %s\n", slug);
}

bool publishSensorMessage() {
    #ifdef MQTT_ENABLED
    if (!mqttConnected) {
        Serial.printf("[SNS] %s: %s (MQTT not connected, skipped)\n",
                      lastSender.c_str(), lastMessage.c_str());
        return false;
    }
    char slug[48];
    mqttSlug(lastSender.c_str(), slug, sizeof(slug));
    if (strlen(slug) == 0) snprintf(slug, sizeof(slug), "unknown");
    // discovery публикуется один раз на отправителя (кэш имён).
    bool known = false;
    int discIdx = -1;
    for (int i = 0; i < sensorDeviceDiscCount; i++) {
        if (sensorDeviceDisc[i] == lastSender) { known = true; discIdx = i; break; }
    }
    if (!known) {
        publishSensorDisc(lastSender, slug);
        if (mqttConnected && sensorDeviceDiscCount < SENSOR_DEV_CACHE_MAX) {
            discIdx = sensorDeviceDiscCount;
            sensorDeviceDisc[sensorDeviceDiscCount++] = lastSender;
        } else {
            discIdx = -1;
        }
    }
    // availability: любой приём от датчика = online.
    if (discIdx >= 0) {
        sensorLastActive[discIdx] = millis();
        if (!sensorOnlineNow[discIdx]) {
            sensorOnlineNow[discIdx] = true;
            char tAvail[128];
            snprintf(tAvail, sizeof(tAvail), "%s/sensor/%s/available", mqttPrefix, slug);
            mqtt.publish(tAvail, "online", true);
            Serial.printf("[SNS] %s AVAILABLE (online)\n", lastSender.c_str());
        }
    }
    // --- heartbeat: availability и версия прошивки ("hello:<версия>"; старые сенсоры шлют "hello") ---
    if (lastMessage == SENSOR_MSG_HELLO || lastMessage.startsWith(SENSOR_MSG_HELLO ":")) {
        int sep = lastMessage.indexOf(':');
        if (sep > 0) {
            char tVer[128];
            snprintf(tVer, sizeof(tVer), "%s/sensor/%s/version", mqttPrefix, slug);
            mqtt.publish(tVer, lastMessage.c_str() + sep + 1, true);
        }
        Serial.printf("[SNS] heartbeat from %s\n", lastSender.c_str());
        return true;
    }
    // --- данные: text + rssi ---
    char escText[128];
    jsonEscape(lastMessage.c_str(), escText, sizeof(escText));
    char tText[128], tRssi[128];
    snprintf(tText, sizeof(tText), "%s/sensor/%s/text", mqttPrefix, slug);
    snprintf(tRssi, sizeof(tRssi), "%s/sensor/%s/rssi", mqttPrefix, slug);
    mqtt.publish(tText, escText);
    char rssiStr[24];
    snprintf(rssiStr, sizeof(rssiStr), "%.1f", lastRSSI);
    mqtt.publish(tRssi, rssiStr);
    // --- button: event entity trigger для автоматизаций ---
    if ((lastMessage == SENSOR_MSG_BUTTON) || (lastMessage == SENSOR_MSG_BUTTON2)) {
        char tEvt[128];
        snprintf(tEvt, sizeof(tEvt), "%s/sensor/%s/button", mqttPrefix, slug);
        // MQTT event entity ожидает JSON с "event_type" (docs event.mqtt),
        // ретранслированные retained-сообщения отбрасываются.
        char evtJson[64];
        snprintf(evtJson, sizeof(evtJson), "{\"event_type\":\"%s\"}", lastMessage.c_str());
        mqtt.publish(tEvt, evtJson, false);
        // auto-clear: через SNS_BTN_CLEAR_MS текст сбрасывается "",
        // чтобы следующее нажатие снова вызвало "state_changed" в HA.
        snsBtnClearAt = millis() + SNS_BTN_CLEAR_MS;
        snsBtnPendingClear = true;
        strlcpy(snsBtnSlug, slug, sizeof(snsBtnSlug));
        Serial.printf("[SNS] %s from %s — trigger published\n", lastMessage.c_str(), lastSender.c_str());
    } else {
        Serial.printf("[SNS] %s: %s (rssi %.1f)\n", lastSender.c_str(), lastMessage.c_str(), lastRSSI);
    }
    return true;
    #else
    Serial.printf("[SNS] %s: %s (MQTT disabled)\n", lastSender.c_str(), lastMessage.c_str());
    return false;
    #endif
}

void publishStatus() {
    if (!mqttConnected) return;
    // Экранируем кавычки в именах/сообщениях для JSON
    char prvEsc[64];
    jsonEscape(privateChannelName.c_str(), prvEsc, sizeof(prvEsc));
    char chEsc[64];
    const char* chName = (mqttTxChannel >= 0 && mqttTxChannel < numChannels)
                         ? channels[mqttTxChannel].name : "?";
    jsonEscape(chName, chEsc, sizeof(chEsc));
    char topic[96], payload[384];
    unsigned long upSec = millis() / 1000;
    float tempC = cpuTempC();
    snprintf(topic, sizeof(topic), "%s/status", mqttPrefix);
    snprintf(payload, sizeof(payload),
        "{\"version\":\"" FW_VERSION "\",\"wifi\":true,\"mqtt\":true,\"lora_rx\":%s,"
        "\"uptime\":%lu,\"packets\":%d,\"duplicates\":%lu,"
        "\"temp\":%.1f,\"ip\":\"%s\","
        "\"channel\":\"%s\",\"private\":\"%s\"}",
        isListening ? "true" : "false",
        upSec, packetCount, duplicateCount,
        tempC,
        wifiConnected ? WiFi.localIP().toString().c_str() : "0.0.0.0",
        chEsc, prvEsc);
    mqtt.publish(topic, payload, true);

    // Listening state
    char lTopic[96];
    snprintf(lTopic, sizeof(lTopic), "%s/lstate", mqttPrefix);
    mqtt.publish(lTopic, isListening ? "ON" : "OFF", true);
}

void setupMQTT() {
    // Собираем префиксы топиков
    snprintf(mqttPrefix, sizeof(mqttPrefix), "meshcore/bot/%s", DEVICE_NAME);
    snprintf(mqttDiscoveryPrefix, sizeof(mqttDiscoveryPrefix), "homeassistant");

    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    mqtt.setBufferSize(768);   // discovery-селект (options) весит до ~600 Б
    mqtt.setSocketTimeout(3);  // ограничиваем блокировку connect() до ~3 с
}

void tickRetryConnections() {
    // ===== WiFi =====
    bool connectedNow = (WiFi.status() == WL_CONNECTED);
    if (wifiConnected && !connectedNow) {
        // обрыв — сбрасываем флаги, дальше переподключаемся
        wifiConnected = false;
        mqttConnected = false;
    }
    if (!connectedNow) {
        if (!wifiConnInProgress) {
            wifiConnInProgress = true;
            wifiConnStartMs = millis();
            Serial.println("[WiFi] connecting...");
            WiFi.disconnect();
            WiFi.begin(WIFI_SSID, WIFI_PASS);
        } else if ((int32_t)(millis() - wifiConnStartMs) > 15000) {
            Serial.println("[WiFi] timeout, retry in 5s");
            wifiConnInProgress = false;
        }
        return;   // без WiFi MQTT не трогаем
    }
    if (!wifiConnected) {
        wifiConnected = true;
        wifiConnInProgress = false;
        Serial.printf("[WiFi] connected (%s)\n", WiFi.localIP().toString().c_str());
        // SNTP сразу при появлении WiFi (не ждём MQTT): часы уточняются с
        // сервера времени, build-time устаревает уже через пару дней.
        if (!ntpStarted) {
            ntpStarted = true;
            lastNtpSyncMs = millis();
            configTime(0, 0, "pool.ntp.org");   // синхронизация в UTC
        }
    }

    // ===== MQTT =====
    if (mqttConnected && mqtt.connected()) return;
    if (mqttConnected) mqttConnected = false;

    Serial.printf("[MQTT] connecting to %s:%d ...", MQTT_BROKER, MQTT_PORT);
    char clientId[48];
    snprintf(clientId, sizeof(clientId), "meshcore_%s_%lu", DEVICE_NAME, millis() % 100000);

    if (mqtt.connect(clientId, MQTT_USER, MQTT_PASS)) {
        mqttConnected = true;
        Serial.println(" OK");
        char cmdTopic[96];
        snprintf(cmdTopic, sizeof(cmdTopic), "%s/cmd/send", mqttPrefix);
        mqtt.subscribe(cmdTopic);
        snprintf(cmdTopic, sizeof(cmdTopic), "%s/cmd/listening", mqttPrefix);
        mqtt.subscribe(cmdTopic);

        // Discovery публикуем только при ПЕРВОМ подключении (reconnect его
        // повторяет поток retained-конфигов). Обновления — по факту изменений.
        if (!discoveryPublished) publishDiscovery();
        publishStatus();
    } else {
        mqttConnected = false;
        Serial.printf(" FAILED (rc=%d)\n", mqtt.state());
    }
}

#endif // MQTT_ENABLED
