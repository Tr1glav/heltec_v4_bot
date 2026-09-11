#include "config.h"
#include "globals.h"
#include "crypto.h"
#include "radio.h"
#include "mesh.h"

uint8_t* findPeerPub(uint8_t hash) {
    for (int i = 0; i < PEER_CACHE_MAX; i++) {
        if (peerCache[i].hash == hash && peerCache[i].pub[0] != 0) return peerCache[i].pub;
    }
    return NULL;
}

void rememberPeerPub(uint8_t hash, const uint8_t* pub) {
    int slot = -1;
    uint32_t oldest = 0xFFFFFFFF;
    for (int i = 0; i < PEER_CACHE_MAX; i++) {
        if (peerCache[i].hash == hash) { slot = i; break; }
        if (peerCache[i].last_seen < oldest) { oldest = peerCache[i].last_seen; slot = i; }
    }
    peerCache[slot].hash = hash;
    memcpy(peerCache[slot].pub, pub, 32);
    peerCache[slot].last_seen = millis();
}

void initAdvertIdentity() {
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
               (const uint8_t*)DEVICE_NAME, strlen(DEVICE_NAME), bot_priv);
    Ed25519::derivePublicKey(bot_pub, bot_priv);

    // Ed25519 private key (64 Б) для X25519-обмена при ответе в личку.
    // seed = тот же bot_priv, pub должен совпасть с bot_pub (RFC8032).
    uint8_t pub_check[32];
    ed25519_create_keypair(pub_check, bot_prv64, bot_priv);
    if (memcmp(pub_check, bot_pub, 32) != 0) {
        Serial.println("[ADV] WARNING: ed25519 pub mismatch (!)");
    }

    #ifndef BOT_ID_HASH
    ownShortHash = bot_pub[0];   // авто: hash ноды = первый байт pubkey
    #endif
    Serial.printf("[ADV] identity pub: ");
    for (int i = 0; i < 32; i++) Serial.printf("%02X", bot_pub[i]);
    Serial.printf(", own short hash: 0x%02X\n", ownShortHash);
}

bool checkAndMarkSeen(uint8_t* data, int len) {
    if (len < 2) return true;  // битый пакет
    uint8_t header = data[0];
    uint8_t payload_type = (header >> 2) & 0x0F;
    int offset = 1;
    if (((header & 0x03) == 0x00) || ((header & 0x03) == 0x03)) offset += 4;
    if (offset >= len) return true;
    uint8_t path_len = data[offset++];
    uint8_t hash_size = (path_len >> 6) + 1;
    uint8_t hop_count = path_len & 0x3F;
    offset += hop_count * hash_size;
    if (offset >= len) return true;

    uint8_t hash_ctx[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, &payload_type, 1);
    mbedtls_md_update(&ctx, &data[offset], len - offset);
    mbedtls_md_finish(&ctx, hash_ctx);
    mbedtls_md_free(&ctx);

    // выбираем кольцевой буфер по типу пакета
    int count  = (payload_type == 0x04) ? SEEN_ADVERT_HASH_COUNT : SEEN_HASH_COUNT;
    uint8_t* hashes = (payload_type == 0x04) ? seen_advert_hashes : seen_hashes;
    int* nextIdx    = (payload_type == 0x04) ? &seen_advert_next_idx : &seen_next_idx;

    // ищем в кольцевом буфере
    for (int i = 0; i < count; i++) {
        if (memcmp(hash_ctx, &hashes[i * SEEN_HASH_SIZE], SEEN_HASH_SIZE) == 0) {
            return true;
        }
    }
    // помечаем
    memcpy(&hashes[*nextIdx * SEEN_HASH_SIZE], hash_ctx, SEEN_HASH_SIZE);
    *nextIdx = (*nextIdx + 1) % count;
    return false;
}

void addChannelKey16(const char* name, const uint8_t* key16) {
    MeshChannel& ch = channels[numChannels];
    memset(ch.secret, 0, sizeof(ch.secret));
    memcpy(ch.secret, key16, 16);
    uint8_t sha256_result[32];
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), ch.secret, 16, sha256_result);
    ch.hash = sha256_result[0];
    ch.name = name;
    numChannels++;
    Serial.printf("Channel %s: key ", name);
    for (int i = 0; i < 16; i++) Serial.printf("%02x", ch.secret[i]);
    Serial.printf(", hash 0x%02X\n", ch.hash);
}

void deriveChannels() {
    uint8_t key16[16];

    // #public: фиксированный PSK из MeshCore
    const char* psk_b64 = "izOH6cXN6mrJ5e26oRXNcg==";
    size_t olen = 0;
    memset(key16, 0, sizeof(key16));
    mbedtls_base64_decode(key16, 16, &olen, (const uint8_t*)psk_b64, strlen(psk_b64));
    addChannelKey16("#public", key16);

    // #connections: автоключ = SHA256("#connections")[0:16]
    const char* name = "#connections";
    uint8_t sha256_result[32];
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
               (const uint8_t*)name, strlen(name), sha256_result);
    memcpy(key16, sha256_result, 16);
    addChannelKey16(name, key16);

    // Приватный канал MQTT-бота — из build-флагов.
    #ifdef MQTT_ENABLED
    loadPrivateChannel();
    #endif
    // Сенсорный канал: нужен и MQTT-боту, и сенсорным платам (без WiFi),
    // поэтому добавляем всегда. Источник — build-флаги (secrets.ini).
    loadSensorChannel();
}

int findChannelByName(const char* name) {
    for (int i = 0; i < numChannels; i++) {
        if (strcmp(channels[i].name, name) == 0) return i;
    }
    return -1;
}

int setPrivateChannel(const String& name, const String& keyb64) {
    if (name.length() == 0 || name.length() > 32) return -1;
    uint8_t key16[16];
    int keyType = privateKeyTo16(keyb64, key16);
    if (keyType < 0) {   // автоключ
        uint8_t sha256_result[32];
        mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                   (const uint8_t*)name.c_str(), name.length(), sha256_result);
        memcpy(key16, sha256_result, 16);
    }

    int idx = findChannelByName(name.c_str());
    if (idx >= 0) {
        // канал существует — проверяем, не поменялся ли ключ
        if (memcmp(channels[idx].secret, key16, 16) != 0) {
            memset(channels[idx].secret, 0, sizeof(channels[idx].secret));
            memcpy(channels[idx].secret, key16, 16);
            uint8_t sha256_result[32];
            mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                       channels[idx].secret, 16, sha256_result);
            channels[idx].hash = sha256_result[0];
            Serial.printf("[PRV] channel %s key UPDATED, hash 0x%02X\n",
                          name.c_str(), channels[idx].hash);
        }
        privateChannelIdx = idx;
        privateChannelName = channels[idx].name;
        return idx;
    }
    if (numChannels >= MAX_CHANNELS) {
        Serial.println("[PRV] MAX_CHANNELS reached, channel not added");
        return -1;
    }
    privateChannelName = name;             // держим имя в String
    addChannelKey16(privateChannelName.c_str(), key16);
    privateChannelIdx = numChannels - 1;
    Serial.printf("[PRV] private channel added: %s key+hash 0x%02X (idx %d)\n",
                  privateChannelName.c_str(), channels[privateChannelIdx].hash,
                  privateChannelIdx);
    return privateChannelIdx;
}

void loadPrivateChannel() {
    // Каналы задаются ТОЛЬКО из build-флагов (secrets.ini), не из HA.
    if (strlen(PRIVATE_CHANNEL_NAME) > 0) {
        Serial.printf("[PRV] channel from build flags: %s\n", PRIVATE_CHANNEL_NAME);
        setPrivateChannel(PRIVATE_CHANNEL_NAME, PRIVATE_CHANNEL_KEY);
    }
}

int setSensorChannel(const String& name, const String& keyb64) {
    if (name.length() == 0 || name.length() > 32) return -1;
    uint8_t key16[16];
    int keyType = privateKeyTo16(keyb64, key16);
    if (keyType < 0) {   // автоключ
        uint8_t sha256_result[32];
        mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                   (const uint8_t*)name.c_str(), name.length(), sha256_result);
        memcpy(key16, sha256_result, 16);
    }

    int idx = findChannelByName(name.c_str());
    if (idx >= 0) {
        // канал существует — проверяем, не поменялся ли ключ
        if (memcmp(channels[idx].secret, key16, 16) != 0) {
            memset(channels[idx].secret, 0, sizeof(channels[idx].secret));
            memcpy(channels[idx].secret, key16, 16);
            uint8_t sha256_result[32];
            mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                       channels[idx].secret, 16, sha256_result);
            channels[idx].hash = sha256_result[0];
            Serial.printf("[SNS] channel %s key UPDATED, hash 0x%02X\n",
                          name.c_str(), channels[idx].hash);
        }
        sensorChannelIdx = idx;
        sensorChannelName = channels[idx].name;
        return idx;
    }
    if (numChannels >= MAX_CHANNELS) {
        Serial.println("[SNS] MAX_CHANNELS reached, channel not added");
        return -1;
    }
    sensorChannelName = name;
    addChannelKey16(sensorChannelName.c_str(), key16);
    sensorChannelIdx = numChannels - 1;
    Serial.printf("[SNS] sensor channel added: %s key+hash 0x%02X (idx %d)\n",
                  sensorChannelName.c_str(), channels[sensorChannelIdx].hash,
                  sensorChannelIdx);
    return sensorChannelIdx;
}

void loadSensorChannel() {
    // Каналы задаются ТОЛЬКО из build-флагов (secrets.ini), не из HA.
    if (strlen(SENSOR_CHANNEL_NAME) > 0) {
        Serial.printf("[SNS] sensor channel from build flags: %s\n", SENSOR_CHANNEL_NAME);
        setSensorChannel(SENSOR_CHANNEL_NAME, SENSOR_CHANNEL_KEY);
    }
}

#ifdef MQTT_ENABLED
void loadTxChannel() {
    int idx = findChannelByName(TX_CHANNEL);
    if (idx < 0) idx = 1;   // #connections
    if (idx < numChannels) {
        mqttTxChannel = idx;
        Serial.printf("[MQTT] TX channel from build flags: %s\n", channels[idx].name);
    }
}
#endif // MQTT_ENABLED

void buildPingReply(char* out, size_t outlen, const uint8_t* path, uint8_t hop_count, uint8_t path_hash_size) {
    if (hop_count == 0) {
        snprintf(out, outlen, "hops:direct");
        return;
    }
    snprintf(out, outlen, "hops:%u, route:", hop_count);
    size_t pos = strlen(out);
    for (int h = 0; h < hop_count; h++) {
        if (pos + 3 >= outlen) break;
        if (h > 0) { out[pos++] = ' '; out[pos++] = 0xE2; out[pos++] = 0x86; out[pos++] = 0x92; }  // →
        const uint8_t* ph = path + h * path_hash_size;
        for (int b = 0; b < path_hash_size; b++) {
            snprintf(&out[pos], outlen - pos, "%02x", ph[b]);
            pos += 2;
        }
        out[pos] = 0;
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
    const uint8_t* path_bytes = &data[offset];
    
    // сохраняем путь (хэши ретрансляторов) для отображения
    lastPath[0] = 0;
    if (hop_count > 0 && offset + hop_count * path_hash_size <= len) {
        char tmp[8];
        for (int h = 0; h < hop_count; h++) {
            const uint8_t* ph = &data[offset + h * path_hash_size];
            if (path_hash_size == 1) {
                snprintf(tmp, sizeof(tmp), "%02X ", ph[0]);
            } else {
                snprintf(tmp, sizeof(tmp), "%02X%02X ", ph[0], ph[1]);
            }
            strcat(lastPath, tmp);
        }
    }

    // копия пути для обратного маршрута ответа (хэши ретрансляторов)
    replyHopCount = (hop_count > 0 && offset + hop_count * path_hash_size <= len) ? hop_count : 0;
    if (hop_count > 0 && offset + hop_count * path_hash_size <= len) {
        replyHashSize = path_hash_size;
        memcpy(replyPath, &data[offset], hop_count * path_hash_size);
    }
    offset += hop_count * path_hash_size;
    
    if (offset >= len) return false;
    
    // === Разбор тела пакета: ДМ (TXT_MSG) или групповое (GRP_TXT) ===
    bool personalDm = false;
    int chIdx = -1;
    
    if (payload_type == 0x02) {
        // Личное сообщение: payload = [dest_hash 1B][src_hash 1B][MAC 2B][cipher...].
        // Шифруется общим секретом X25519 — текст без ключей ноды не прочитать,
        // но dest_hash (первый байт) показывает, адресовано ли сообщение НАМ.
        if (offset + 2 > len) return false;
        uint8_t dest_hash = data[offset];
        uint8_t src_hash  = data[offset + 1];
        if (dest_hash != ownShortHash) {
            Serial.printf("[DM] dest=%02X (не нам, наш=%02X) src=%02X, игнор\n", dest_hash, ownShortHash, src_hash);
            return false;
        }
        personalDm = true;
        dmSrcHash = src_hash;
        
        // В ДМ нет хэша канала — отвечаем в #connections (личный канал).
        chIdx = 1;
        if (chIdx >= numChannels) chIdx = 0;
        lastChannelIdx = chIdx;
        lastChannelName = "DM #connections";
        
        char senderHex[8];
        snprintf(senderHex, sizeof(senderHex), "<%02X>", src_hash);
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
    if (lastSender == DEVICE_NAME) return false;
    
    Serial.printf("\n=== PACKET #%d (%s) ===\n", packetCount, lastChannelName.c_str());
    Serial.printf("From: %s\n", lastSender.c_str());
    Serial.printf("Msg: %s\n", lastMessage.c_str());
    Serial.printf("Route: %s (hops=%u)\n", lastPath[0] ? lastPath : "direct", hop_count);
    Serial.printf("RSSI: %.1f dBm, SNR: %.1f dB\n", lastRSSI, lastSNR);
    
    lastRxDisplay = millis();
    
    // Показать сообщение на экране (если экран не погашен автовыключением).
    // Во время mesh OTA экран не трогаем — иначе каждый чанк мигает сообщением
    // между кадрами прогресса (otaDrawProgress/otaSensorDraw).
    if (!screenOff && !otaFastMode) {
        display.clearDisplay();
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.println(lastChannelName.c_str());
        display.printf("From: %s\n", lastSender.c_str());
        display.printf("RSSI:%.0f SNR:%.0f\n", lastRSSI, lastSNR);
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

        // === Синхронизация времени: бот шлёт "time:<epoch>" от NTP ===
        if (lastMessage.startsWith("time:")) {
            uint64_t epoch = (uint64_t)strtoull(lastMessage.c_str() + 5, NULL, 10);
            if (epoch > (uint64_t)BUILD_UNIX_TIME) {
                struct timeval tv;
                tv.tv_sec = (time_t)epoch;
                tv.tv_usec = 0;
                settimeofday(&tv, NULL);
                time_t local = (time_t)epoch + (time_t)TZ_OFFSET_HOURS * 3600;
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
        if (snsPub) display.printf("SNS -> MQTT RSSI:%.0f", lastRSSI);
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
        // Пауза перед ответом: даём отправителю выйти из TX и перейти в RX,
        // иначе его приёмник «задирается» на старт нашей передачи (desense).
        delay(250);

        char reply[100];
        buildPingReply(reply, sizeof(reply), replyPath, replyHopCount, replyHashSize);
        Serial.printf("[PING] reply: %s\n", reply);
        pingReplyText = String(reply);

        // === Личное сообщение: ответ уходит В ЛИЧКУ (TXT_MSG), а не в канал ===
        if (personalDm) {
            uint8_t* peerPub = findPeerPub(dmSrcHash);
            if (peerPub != NULL) {
                int dl = buildPrivateTextFrame(dmSrcHash, peerPub, pingReplyText,
                                               dmReplyFrame, sizeof(dmReplyFrame));
                if (dl > 0) {
                    Serial.printf("\n[TX DM] to <%02X>: %s (%dB, flood x3)\n", dmSrcHash, pingReplyText.c_str(), dl);
                    floodSend3(-1, dmReplyFrame, dl);
                }
            } else {
                Serial.printf("[DM] pubkey <%02X> неизвестен (нет advert) — ответ не отправлен\n", dmSrcHash);
            }
            return true;
        }

        int enclen = buildGroupEnc(chIdx, pingReplyText, pingReplyEnc);
        if (enclen <= 0) return true;
        pingReplyEncLen = enclen;

        uint8_t frame[300];
        int f = buildGroupFrameFlood(chIdx, pingReplyText, frame, sizeof(frame),
                                     pingReplyEnc, pingReplyEncLen);
        if (f > 0) {
            Serial.printf("\n[TX] %s: %s (%dB, flood x3)\n", channels[chIdx].name,
                          (DEVICE_NAME ": " + pingReplyText).c_str(), f);
            floodSend3(chIdx, frame, f);
        }
    }
    #endif  // !SENSOR_NODE (ответ на пинг/DM — только MQTT-бот)

    return true;
}

void sendAdvert(uint8_t route_type) {
    uint8_t app[32];
    int applen = 0;
    app[applen++] = 0x80 | 0x01;  // ADV_TYPE_CHAT + имя
    const char* name = DEVICE_NAME;
    int nlen = strlen(name);
    if (nlen > 31) nlen = 31;
    memcpy(app + applen, name, nlen);
    applen += nlen;

    uint8_t frame[190];
    int f = 0;
    frame[f++] = (uint8_t)((0x04 << 2) | (route_type & 0x03));  // ADVERT | route
    frame[f++] = 0x00;  // path_len: hash_size=1, 0 хопов

    memcpy(frame + f, bot_pub, 32); f += 32;
    uint32_t ts = (uint32_t)time(NULL);
    memcpy(frame + f, &ts, 4); f += 4;

    uint8_t msg[32 + 4 + 32];
    int mlen = 0;
    memcpy(msg + mlen, bot_pub, 32); mlen += 32;
    memcpy(msg + mlen, &ts, 4); mlen += 4;
    memcpy(msg + mlen, app, applen); mlen += applen;

    uint8_t sig[64];
    Ed25519::sign(sig, bot_priv, bot_pub, msg, mlen);
    memcpy(frame + f, sig, 64); f += 64;
    memcpy(frame + f, app, applen); f += applen;

    Serial.printf("\n[TX ADV] route=%u (%dB)\n", route_type, f);
    for (int i = 0; i < f; i++) Serial.printf("%02X", frame[i]);
    Serial.println();
    txFrame(frame, f);
}

int buildGroupEnc(int chIdx, const String& msg, uint8_t* enc) {
    if (chIdx < 0 || chIdx >= numChannels) return 0;
    MeshChannel& ch = channels[chIdx];

    uint8_t plaintext[256];
    int plen = 0;
    // В ts[4] уходит Unix-время в СЕКУНДАХ (как у adverts и личных сообщений):
    // epoch-ms не помещается в uint32 (переполняется и клиенты показывают ~2046 год).
    // Если время синхронизировано/из BUILD_UNIX_TIME — шлём секунды, иначе millis.
    uint32_t ts = ((uint32_t)time(NULL) > 1000000000)
        ? (uint32_t)time(NULL)
        : (uint32_t)millis();
    memcpy(plaintext, &ts, 4); plen += 4;           // timestamp (LE)
    plaintext[plen++] = 0;                          // TXT_TYPE_PLAIN
    const char* prefix = DEVICE_NAME ": ";
    memcpy(plaintext + plen, prefix, strlen(prefix)); plen += strlen(prefix);
    size_t mlen = min((size_t)219, msg.length());
    memcpy(plaintext + plen, msg.c_str(), mlen); plen += mlen;

    return encryptGroupText(ch.secret, enc, plaintext, plen);
}

int buildGroupFrameFlood(int chIdx, const String& msg, uint8_t* frame, int maxlen,
                         const uint8_t* enc_in, int enc_in_len) {
    if (chIdx < 0 || chIdx >= numChannels) return 0;
    uint8_t enc[256];
    int enclen;
    if (enc_in != NULL && enc_in_len > 0) {
        enclen = min(enc_in_len, (int)sizeof(enc));
        memcpy(enc, enc_in, enclen);
    } else {
        enclen = buildGroupEnc(chIdx, msg, enc);
    }
    if (enclen <= 0 || 3 + enclen > maxlen) return 0;

    int f = 0;
    frame[f++] = 0x15;        // GRP_TXT | ROUTE_TYPE_FLOOD
    frame[f++] = 0x00;        // path_len: hash_size=1, 0 хопов (построится ретрансляторами)
    frame[f++] = channels[chIdx].hash;
    memcpy(frame + f, enc, enclen); f += enclen;
    return f;
}

int buildGroupFrameReturnPath(int chIdx, const String& msg,
                              const uint8_t* path, uint8_t hop_count, uint8_t hash_size,
                              uint8_t* frame, int maxlen,
                              const uint8_t* enc_in, int enc_in_len) {
    if (chIdx < 0 || chIdx >= numChannels) return 0;
    if (hop_count == 0 || hop_count > 0x3F || hash_size == 0 || hash_size > 8) return 0;
    uint8_t enc[256];
    int enclen;
    if (enc_in != NULL && enc_in_len > 0) {
        enclen = min(enc_in_len, (int)sizeof(enc));
        memcpy(enc, enc_in, enclen);
    } else {
        enclen = buildGroupEnc(chIdx, msg, enc);
    }
    if (enclen <= 0) return 0;

    int pathBytes = hop_count * hash_size;
    if (3 + pathBytes + enclen > maxlen) return 0;

    int f = 0;
    frame[f++] = 0x16;        // GRP_TXT | ROUTE_TYPE_DIRECT
    frame[f++] = (uint8_t)(((hash_size - 1) << 6) | hop_count);
    for (int h = 0; h < hop_count; h++) {
        int src = (hop_count - 1 - h) * hash_size;   // разворачиваем путь
        for (int b = 0; b < hash_size; b++) frame[f++] = path[src + b];
    }
    frame[f++] = channels[chIdx].hash;
    memcpy(frame + f, enc, enclen); f += enclen;
    return f;
}

int buildPrivateTextFrame(uint8_t dest_hash, const uint8_t* dest_pub,
                          const String& msg, uint8_t* frame, int maxlen) {
    uint8_t secret[32];
    ed25519_key_exchange(secret, dest_pub, bot_prv64);

    uint8_t data[128];
    int dlen = 0;
    uint32_t ts = (uint32_t)time(NULL);   // Unix-секунды (epoch-ms не лезет в uint32)
    memcpy(data, &ts, 4); dlen += 4;
    data[dlen++] = 0;                        // attempt = 0
    size_t ml = min((size_t)96, msg.length());
    memcpy(data + dlen, msg.c_str(), ml); dlen += ml;
    data[dlen++] = 0;                        // null terminator

    uint8_t enc[128];
    int enclen = encryptGroupText(secret, enc, data, dlen);   // [MAC 2B][cipher]
    if (enclen <= 0 || 4 + enclen > maxlen) return 0;

    int f = 0;
    frame[f++] = 0x09;                       // TXT_MSG | ROUTE_TYPE_FLOOD
    frame[f++] = 0x40;                       // path_len: hash_size=2, 0 хопов
    frame[f++] = dest_hash;
    frame[f++] = ownShortHash;
    memcpy(frame + f, enc, enclen); f += enclen;
    return f;
}

int sendFrame(int chIdx, const uint8_t* frame, int f) {
    if (chIdx < 0 || chIdx >= numChannels) return RADIOLIB_ERR_UNKNOWN;
    // hex-лог кадра стоит ~40 мс на UART для 245-байтного чанка — при mesh OTA
    // на каждый аck/чанк это минуты суммарно, поэтому в fast-режиме молчим.
    if (!otaFastMode) {
        for (int i = 0; i < f; i++) Serial.printf("%02X", frame[i]);
        Serial.println();
    }
    return txFrame((uint8_t*)frame, f);
}

void floodSend3(int chIdx, const uint8_t* frame, int f, unsigned int gapMs) {
    for (int i = 0; i < 3; i++) {
        if (chIdx >= 0) sendFrame(chIdx, frame, f);
        else            txFrame((uint8_t*)frame, f);
        if (i < 2) delay(gapMs);
    }
}

void sendSensorTimeSync() {
    if (sensorChannelIdx < 0) return;
    uint8_t frame[300];
    char msg[40];
    snprintf(msg, sizeof(msg), "time:%llu", (unsigned long long)time(NULL));
    int f = buildGroupFrameFlood(sensorChannelIdx, msg, frame, sizeof(frame), NULL, 0);
    if (f > 0) {
        Serial.printf("\n[TIME] -> %s: %s\n", channels[sensorChannelIdx].name, msg);
        floodSend3(sensorChannelIdx, frame, f);
    }
}

String channelListStr() {
    String s = "";
    for (int i = 0; i < numChannels; i++) {
        if (i > 0) s += " + ";
        s += channels[i].name;
    }
    return s;
}
