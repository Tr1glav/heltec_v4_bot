#include "config.h"
#include "globals.h"
#include "crypto.h"
#include "radio.h"
#include "mesh.h"
#include "display.h"
#include "ota.h"
#include "companion.h"

// ===== Разбор входящего пакета =====
// Выделено из mesh.cpp: распознавание типа пакета, проверка адресата, расшифровка
// группового и личного сообщения, служебные команды сенсорного канала и ответы на них.

// Формат ответа как в bot.py: "hops:direct" либо "hops:N, route:aa → bb → cc"
void buildPingReply(char* out, size_t outlen, const uint8_t* path, uint8_t hop_count, uint8_t path_hash_size) {
    if (hop_count == 0) {
        snprintf(out, outlen, "hops:direct");
        return;
    }
    size_t pos = snprintf(out, outlen, "hops:%u, route:", hop_count);
    for (int h = 0; h < hop_count; h++) {
        size_t need = (h > 0 ? 4 : 0) + 2 * path_hash_size + 1;
        if (pos + need > outlen) break;
        if (h > 0) { out[pos++] = ' '; out[pos++] = 0xE2; out[pos++] = 0x86; out[pos++] = 0x92; }  // " →"
        for (int b = 0; b < path_hash_size; b++) {
            pos += snprintf(out + pos, outlen - pos, "%02x", path[h * path_hash_size + b]);
        }
    }
}

bool parseMeshCorePacket(uint8_t* data, int len) {
    if (len < 6) return false;

    uint8_t header = data[0];
    uint8_t payload_type = (header >> 2) & 0x0F;
    uint8_t route_type = header & 0x03;

    // GRP_TXT (0x05) — групповые сообщения, TXT_MSG (0x02) — личные (ДМ).
    if (payload_type != 0x05 && payload_type != 0x02) {
        // ADVERT (0x04): кэшируем публичные ключи нод — без них не ответить
        // в личку (нужен полный pubkey для X25519). В кадре: [pub 32][ts 4][sig 64][app...].
        if (payload_type == 0x04) {
            int o = 1;
            if (route_type == 0x00 || route_type == 0x03) o += 4;
            if (o < len) {
                uint8_t pl = data[o++];
                const uint8_t* advPath = &data[o];          // хэши ретрансляторов, через которые пришёл адверт
                o += (pl & 0x3F) * (((pl >> 6) & 3) + 1);   // path bytes
                if (o + 32 + 4 + 64 <= len) {
                    uint8_t* pub = &data[o];
                    uint8_t* ts  = &data[o + 32];
                    uint8_t* sig = &data[o + 36];
                    uint8_t* app = &data[o + 100];
                    int applen = len - (o + 100);
                    if (applen < 0) applen = 0;
                    if (applen > 64) applen = 64;
                    uint8_t msg[32 + 4 + 64];
                    int mlen = 0;
                    memcpy(&msg[mlen], pub, 32); mlen += 32;
                    memcpy(&msg[mlen], ts, 4); mlen += 4;
                    memcpy(&msg[mlen], app, applen); mlen += applen;
                    if (Ed25519::verify(sig, pub, msg, mlen)) {
                        rememberPeerPub(pub[0], pub);
                        #ifdef COMPANION_NODE
                        // Приложение строит список собеседников из адвертов: без этого
                        // добавить кого-либо в контакты попросту неоткуда.
                        companionOnAdvert(pub, app, applen, pl, advPath);
                        #endif
                        Serial.printf("[ADV] cached pubkey for <%02X>\n", pub[0]);
                    } else {
                        Serial.printf("[ADV] bad signature for <%02X>\n", pub[0]);
                    }
                }
            }
        }
        return false;
    }

    int offset = 1;
    if (route_type == 0x00 || route_type == 0x03) offset += 4;  // transport codes

    if (offset >= len) return false;
    uint8_t path_len = data[offset++];
    uint8_t path_hash_size = (path_len >> 6) + 1;
    uint8_t hop_count = path_len & 0x3F;
    int pathBytes = hop_count * path_hash_size;
    bool pathOk = hop_count > 0 && offset + pathBytes <= len;

    // путь (хэши ретрансляторов) для экрана и MQTT; длина пути приходит из эфира — пишем с проверкой
    lastPath[0] = 0;
    if (pathOk) {
        size_t pos = 0;
        for (int h = 0; h < hop_count && pos + 6 < sizeof(lastPath); h++) {
            const uint8_t* ph = &data[offset + h * path_hash_size];
            pos += (path_hash_size == 1)
                ? snprintf(lastPath + pos, sizeof(lastPath) - pos, "%02X ", ph[0])
                : snprintf(lastPath + pos, sizeof(lastPath) - pos, "%02X%02X ", ph[0], ph[1]);
        }
    }

    #ifndef SENSOR_NODE
    // копия пути для обратного маршрута в ответе на /ping
    uint8_t replyPath[MAX_REPLY_PATH];
    uint8_t replyHops = 0;
    if (pathOk && pathBytes <= MAX_REPLY_PATH) {
        memcpy(replyPath, &data[offset], pathBytes);
        replyHops = hop_count;
    }
    #endif
    offset += pathBytes;

    if (offset >= len) return false;

    // === Разбор тела пакета: ДМ (TXT_MSG) или групповое (GRP_TXT) ===
    bool personalDm = false;
    uint8_t dmSrc = 0;
    int chIdx = -1;

    if (payload_type == 0x02) {
        // Личное сообщение: payload = [dest_hash 1B][src_hash 1B][MAC 2B][cipher...].
        // Шифруется общим секретом X25519 — текст без ключей ноды не прочитать,
        // но dest_hash (первый байт) показывает, адресовано ли сообщение НАМ.
        if (offset + 2 > len) return false;
        uint8_t dest_hash = data[offset];
        dmSrc = data[offset + 1];
        if (dest_hash != ownShortHash) {
            Serial.printf("[DM] dest=%02X (не нам, наш=%02X) src=%02X, игнор\n", dest_hash, ownShortHash, dmSrc);
            return false;
        }
        personalDm = true;

        // В ДМ нет хэша канала — отвечаем в #connections (личный канал).
        chIdx = 1;
        if (chIdx >= numChannels) chIdx = 0;
        lastChannelIdx = chIdx;
        lastChannelName = "DM #connections";

        char senderHex[8];
        snprintf(senderHex, sizeof(senderHex), "<%02X>", dmSrc);
        lastSender = senderHex;
        lastMessage = "(личное сообщение)";
    } else {
        uint8_t channel_hash = data[offset++];
        if (offset + 2 > len) return false;

        // ищем канал по хэшу
        for (int i = 0; i < numChannels; i++) {
            if (channel_hash == channels[i].hash) { chIdx = i; break; }
        }
        if (chIdx < 0) return false;
        lastChannelIdx = chIdx;
        lastChannelName = channels[chIdx].name;

        uint8_t* mac = &data[offset];         // 2 байта MAC
        uint8_t* ciphertext = &data[offset + 2];  // шифротекст после MAC
        int ciphertext_len = len - (offset + 2);

        // обрезаем до кратного 16
        int ciphertext_len_trunc = ciphertext_len & ~15;
        if (ciphertext_len_trunc <= 0) return false;
        String message = decryptGroupText(channels[chIdx].secret, mac, ciphertext, ciphertext_len_trunc);

        if (message.length() == 0) {
            Serial.println("[!] HMAC не совпал или пустое сообщение");
            return false;
        }

        int colonPos = message.indexOf(": ");
        if (colonPos > 0) {
            lastSender = message.substring(0, colonPos);
            lastMessage = message.substring(colonPos + 2);
        } else {
            lastSender = "?";
            lastMessage = message;
        }
    }

    packetCount++;
    lastRSSI = radio.getRSSI();
    lastSNR = radio.getSNR();
    lastHopCount = hop_count;

    // убираем хвостовые пробелы/переносы (у некоторых клиентов "/ping \n")
    lastMessage.trim();

    // не обрабатываем собственные сообщения (эхо собственного флуда)
    if (lastSender == cfg.name) return false;

    #ifdef COMPANION_NODE
    // Показываем всё, что пришло в канал, включая служебный обмен узлов (hello, ping,
    // pong, time, ota, cfg): по нему видно жизнь сети, а отличить служебное от беседы
    // можно и по самому тексту.
    if (chIdx >= 0) {
        // Отправителя приложение достаёт из начала текста ("Имя: сообщение") — так
        // устроен формат группового сообщения в сети. Мы же разбирали строку на имя и
        // текст и отдавали только текст, поэтому в приложении сообщения были безымянными.
        String forApp = (lastSender.length() > 0 && lastSender != "?")
                      ? lastSender + ": " + lastMessage : lastMessage;
        // Приложению нужен сам байт длины пути, а не число хопов: в нём закодированы и
        // размер хэша ретранслятора, и счётчик, по нему приложение и показывает маршрут.
        companionOnChannelText(chIdx, forApp, lastSNR, path_len);
    }
    #endif

    Serial.printf("\n=== PACKET #%d (%s) ===\n", packetCount, lastChannelName.c_str());
    Serial.printf("From: %s\n", lastSender.c_str());
    Serial.printf("Msg: %s\n", lastMessage.c_str());
    Serial.printf("Route: %s (hops=%u)\n", lastPath[0] ? lastPath : "direct", hop_count);
    char r1[12], s1[12];
    Serial.printf("RSSI: %s dBm, SNR: %s dB\n", fmtFix(lastRSSI, 1, r1, sizeof(r1)),
                  fmtFix(lastSNR, 1, s1, sizeof(s1)));

    lastRxDisplay = millis();

    // Показать сообщение на экране. Во время mesh OTA экран не трогаем — иначе каждый
    // чанк мигает сообщением между кадрами прогресса (otaDrawProgress/otaSensorDraw).
    // Сенсор показывает только свой статус (drawIdleStatus), входящие пакеты не рисует.
    #ifndef SENSOR_NODE
    if (!otaFastMode) {
        display.clearDisplay();
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.println(lastChannelName.c_str());
        display.printf("From: %s\n", lastSender.c_str());
        char r2[12], s2[12];
        display.printf("RSSI:%s SNR:%s\n", fmtFix(lastRSSI, 0, r2, sizeof(r2)),
                       fmtFix(lastSNR, 0, s2, sizeof(s2)));
        if (hop_count > 0) {
            display.drawLine(0, 24, 128, 24, SSD1306_WHITE);
            display.setCursor(0, 26);
            display.print("via: ");
            String pathStr = lastPath;
            if (pathStr.length() > 24) pathStr = pathStr.substring(0, 20) + "..";
            display.println(pathStr);
            display.drawLine(0, 36, 128, 36, SSD1306_WHITE);
            display.setCursor(0, 40);
        } else {
            display.drawLine(0, 32, 128, 32, SSD1306_WHITE);
            display.setCursor(0, 36);
        }
        String showMsg = lastMessage;
        if (showMsg.length() > 26) showMsg = showMsg.substring(0, 24) + "..";
        display.println(showMsg);
        display.display();
    }
    #endif

    // === СЕНСОРНЫЙ КАНАЛ: сообщение уходит в MQTT как отдельное устройство ===
    if (sensorChannelIdx >= 0 && chIdx == sensorChannelIdx) {
        // === MESH OTA: бот принимает ack, сенсор — чанки/управление ===
        if (lastMessage.startsWith("ota:")) {
            #ifdef MQTT_ENABLED
            otaHandleAck();
            #elif defined(SENSOR_NODE)
            otaSensorHandle();
            #endif
            return true;
        }

        // === Настройка по радио: команда адресована конкретному узлу по имени ===
        if (lastMessage.startsWith("cfg:")) {
            #ifdef SENSOR_NODE
            String rest = lastMessage.substring(4);
            int p = rest.indexOf(':');
            if (p > 0 && rest.substring(0, p) == cfg.name) cfgHandleMeshCfg(rest.substring(p + 1));
            #elif defined(MQTT_ENABLED)
            // ответы сенсора видно в журнале на странице, в MQTT их не публикуем
            slog("[CFG] %s: %s\n", lastSender.c_str(), lastMessage.c_str());
            #endif
            return true;
        }

        // === Проверка связи: сенсор шлёт ping:<номер>, координатор сразу отвечает
        //     pong:<номер>:<свой rssi>:<свой snr>, сенсор считает время обмена ===
        if (lastMessage.startsWith(SENSOR_MSG_PING)) {
            #ifdef MQTT_ENABLED
            char reply[48];
            snprintf(reply, sizeof(reply), "%s%s:%d:%d", SENSOR_MSG_PONG,
                     lastMessage.c_str() + strlen(SENSOR_MSG_PING),
                     (int)lround(lastRSSI), (int)lround(lastSNR));
            slog("[PING] %s -> %s\n", lastSender.c_str(), reply);
            sensorSendMsg(reply, FLOOD_RETRY_MS, 1);
            #endif
            return true;
        }
        if (lastMessage.startsWith(SENSOR_MSG_PONG)) {
            #ifdef SENSOR_NODE
            const char* p = lastMessage.c_str() + strlen(SENSOR_MSG_PONG);
            unsigned id = (unsigned)strtoul(p, NULL, 10);
            if (pingSentMs != 0 && id == pingId) {
                pingRttMs = millis() - pingSentMs;
                pingSentMs = 0;
                pingRssi = lastRSSI;
                pingSnr = lastSNR;
                pingHops = lastHopCount;
                pingPeerRssi = 0;
                const char* c1 = strchr(p, ':');
                if (c1) pingPeerRssi = atoi(c1 + 1);
                pingFailed = false;
                pingShowUntil = millis() + PING_SHOW_MS;
                Serial.printf("[PING] ответ за %lu мс, хопов %u\n", pingRttMs, pingHops);
            }
            #endif
            return true;
        }

        // === Опрос со страницы OTA: каждый сенсор ответит hello:<версия> со случайной задержкой,
        //     чтобы ответы нескольких сенсоров не столкнулись в эфире ===
        if (lastMessage == SENSOR_MSG_HELLO_REQ) {
            #ifdef SENSOR_NODE
            sensorHelloDueMs = millis() + random(HELLO_REPLY_DELAY_MIN_MS, HELLO_REPLY_DELAY_MAX_MS);
            #endif
            return true;
        }

        // === Синхронизация времени: бот шлёт "time:<epoch>:<версия бота>" от NTP ===
        if (lastMessage.startsWith("time:")) {
            char* end = NULL;
            uint64_t epoch = (uint64_t)strtoull(lastMessage.c_str() + 5, &end, 10);
            #ifdef SENSOR_NODE
            // версия бота не совпала со своей — экран покажет звёздочку у версии
            if (end && *end == ':') fwVersionDiffers = strcmp(end + 1, FW_VERSION) != 0;
            #endif
            if (epoch > (uint64_t)BUILD_UNIX_TIME) {
                struct timeval tv;
                tv.tv_sec = (time_t)epoch;
                tv.tv_usec = 0;
                settimeofday(&tv, NULL);
                timeSyncMs = millis();
                time_t local = (time_t)epoch + (time_t)cfg.tzOffset * 3600;
                struct tm tm_now;
                gmtime_r(&local, &tm_now);
                char tbuf[32];
                strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm_now);
                Serial.printf("[RTC] SYNCED from channel: %s\n", tbuf);
            }
        }
        #ifdef MQTT_ENABLED
        bool snsPub = publishSensorMessage();
        // Диагностика на экран: канал приёма -> статус публикации в MQTT.
        display.drawLine(0, 48, 128, 48, SSD1306_WHITE);
        display.setCursor(0, 50);
        char r3[12];
        if (snsPub) display.printf("SNS -> MQTT RSSI:%s", fmtFix(lastRSSI, 0, r3, sizeof(r3)));
        else        display.printf("SNS RX, MQTT %s", wifiConnected ? "off" : "no-wifi");
        display.display();
        #else
        Serial.printf("[SNS] %s: %s (MQTT disabled)\n", lastSender.c_str(), lastMessage.c_str());
        #endif
        return true;
    }

    // === ОТВЕТ НА СООБЩЕНИЕ ===
    // Только для MQTT-бота. Сенсорный узел — пассивный: отвечает ТОЛЬКО кнопкой
    // ("button") и heartbeat ("hello"), пинг/DM он не обслуживает.
    #ifndef SENSOR_NODE
    // - Личное (TXT_MSG) с dest_hash == BOT_ID_HASH: отвечаем ВСЕГДА, как на /ping.
    // - Групповой GRP_TXT: на текст "/ping" в #connections либо на любое
    //   DIRECT-сообщение, адресованное устройству.
    bool isDirect = (route_type == 0x02 || route_type == 0x03);
    if (personalDm || (chIdx == 1 && lastMessage == "/ping") || isDirect) {
        // Ждём, пока отправитель закончит свои повторы: пока он передаёт, он нас не
        // слышит, и ответ, посланный раньше, до него просто не дойдёт. Задержка
        // случайная — иначе несколько узлов отвечают разом и глушат друг друга.
        delay(random(PING_REPLY_DELAY_MIN_MS, PING_REPLY_DELAY_MAX_MS));

        char reply[100];
        buildPingReply(reply, sizeof(reply), replyPath, replyHops, path_hash_size);
        Serial.printf("[PING] reply: %s\n", reply);
        uint8_t frame[256];

        // === Личное сообщение: ответ уходит В ЛИЧКУ (TXT_MSG), а не в канал ===
        if (personalDm) {
            uint8_t* peerPub = findPeerPub(dmSrc);
            if (peerPub == NULL) {
                Serial.printf("[DM] pubkey <%02X> неизвестен (нет advert) — ответ не отправлен\n", dmSrc);
                return true;
            }
            int dl = buildPrivateTextFrame(dmSrc, peerPub, reply, frame, sizeof(frame));
            if (dl > 0) {
                Serial.printf("\n[TX DM] to <%02X>: %s (%dB, флудом)\n", dmSrc, reply, dl);
                floodSend(-1, frame, dl);
            }
            return true;
        }

        int f = buildGroupFrameFlood(chIdx, reply, frame, sizeof(frame));
        if (f > 0) {
            Serial.printf("\n[TX] %s: %s: %s (%dB, флудом)\n", channels[chIdx].name, cfg.name.c_str(), reply, f);
            floodSend(chIdx, frame, f);
        }
    }
    #endif  // !SENSOR_NODE (ответ на пинг/DM — только MQTT-бот)

    return true;
}

