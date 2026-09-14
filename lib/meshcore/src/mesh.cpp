#include "config.h"
#include "globals.h"
#include "crypto.h"
#include "radio.h"
#include "mesh.h"
#include "display.h"
#include "ota.h"   // slog: журнал бота, он же виден на странице
#include "companion.h"   // очередь сообщений для телефонного приложения

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
               (const uint8_t*)cfg.name.c_str(), cfg.name.length(), bot_priv);
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

// Секрет канала — 16-байтный ключ, дополненный нулями до 32; hash — первый байт SHA256(ключа)
static void setChannelKey(MeshChannel& ch, const uint8_t* key16) {
    memset(ch.secret, 0, sizeof(ch.secret));
    memcpy(ch.secret, key16, 16);
    uint8_t sha[32];
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), ch.secret, 16, sha);
    ch.hash = sha[0];
}

// Автоключ канала MeshCore: SHA256(имя)[0:16]
static void autoKey16(const char* name, uint8_t key16[16]) {
    uint8_t sha[32];
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t*)name, strlen(name), sha);
    memcpy(key16, sha, 16);
}

// Имена каналов, заведённых не из прошивки: channels[].name — указатель, поэтому
// строка обязана пережить вызов, а не остаться во временном буфере.
static char chanNamePool[MAX_CHANNELS][33];

int channelSetSlot(int idx, const char* name, const uint8_t* key16) {
    if (idx < 0 || idx >= MAX_CHANNELS || name == nullptr || name[0] == 0) return -1;
    if (idx > numChannels) return -1;          // дыр в списке быть не должно
    if (idx == numChannels) numChannels = idx + 1;
    strncpy(chanNamePool[idx], name, 32);
    chanNamePool[idx][32] = 0;
    channels[idx].name = chanNamePool[idx];
    setChannelKey(channels[idx], key16);
    Serial.printf("[CH] канал %d: %s hash 0x%02X\n", idx, chanNamePool[idx], channels[idx].hash);
    return idx;
}

void addChannelKey16(const char* name, const uint8_t* key16) {
    MeshChannel& ch = channels[numChannels];
    setChannelKey(ch, key16);
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

    // #connections: автоключ по имени
    autoKey16("#connections", key16);
    addChannelKey16("#connections", key16);

    // Приватный канал: нужен координатору и компаньону — в приложении это обычный
    // чат наравне с остальными, и без него в списке каналов видно только сенсорный.
    #if defined(MQTT_ENABLED) || defined(COMPANION_NODE)
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

// Канал из build-флагов: PSK в base64 или автоключ по имени. channels[].name указывает
// в буфер nameStore, поэтому nameStore должен жить всё время работы (глобальный String).
static int setNamedChannel(const char* tag, const String& name, const String& keyb64,
                           String& nameStore, int& idxStore) {
    if (name.length() == 0 || name.length() > 32) return -1;
    uint8_t key16[16];
    if (privateKeyTo16(keyb64, key16) < 0) autoKey16(name.c_str(), key16);

    int idx = findChannelByName(name.c_str());
    if (idx < 0) {
        if (numChannels >= MAX_CHANNELS) {
            Serial.printf("[%s] MAX_CHANNELS reached, channel not added\n", tag);
            return -1;
        }
        nameStore = name;
        addChannelKey16(nameStore.c_str(), key16);
        idx = numChannels - 1;
        Serial.printf("[%s] channel added: %s hash 0x%02X (idx %d)\n",
                      tag, nameStore.c_str(), channels[idx].hash, idx);
    } else {
        if (memcmp(channels[idx].secret, key16, 16) != 0) {
            setChannelKey(channels[idx], key16);
            Serial.printf("[%s] channel %s key UPDATED, hash 0x%02X\n", tag, name.c_str(), channels[idx].hash);
        }
        if (channels[idx].name != nameStore.c_str()) nameStore = channels[idx].name;
    }
    idxStore = idx;
    return idx;
}

void loadPrivateChannel() {
    // Каналы задаются ТОЛЬКО настройками устройства (NVS), не из HA.
    if (cfg.prvName.length() == 0) return;
    Serial.printf("[PRV] channel from config: %s\n", cfg.prvName.c_str());
    setNamedChannel("PRV", cfg.prvName, cfg.prvKey, privateChannelName, privateChannelIdx);
}

void loadSensorChannel() {
    if (cfg.snsName.length() == 0) return;
    Serial.printf("[SNS] sensor channel from config: %s\n", cfg.snsName.c_str());
    setNamedChannel("SNS", cfg.snsName, cfg.snsKey, sensorChannelName, sensorChannelIdx);
}

#ifdef MQTT_ENABLED
void loadTxChannel() {
    int idx = findChannelByName(cfg.txChannel.c_str());
    if (idx < 0) idx = 1;   // #connections
    if (idx < numChannels) {
        mqttTxChannel = idx;
        Serial.printf("[MQTT] TX channel from config: %s\n", channels[idx].name);
    }
}
#endif // MQTT_ENABLED

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
            sensorHelloDueMs = millis() + random(300, 4000);
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
        // Пауза перед ответом: даём отправителю выйти из TX и перейти в RX,
        // иначе его приёмник «задирается» на старт нашей передачи (desense).
        delay(250);

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

void sendAdvert(uint8_t route_type) {
    uint8_t app[32];
    int applen = 0;
    app[applen++] = 0x80 | 0x01;  // ADV_TYPE_CHAT + имя
    const char* name = cfg.name.c_str();
    int nlen = (int)cfg.name.length();
    if (nlen > 31) nlen = 31;
    memcpy(app + applen, name, nlen);
    applen += nlen;

    uint8_t frame[190];
    int f = 0;
    frame[f++] = (uint8_t)((0x04 << 2) | (route_type & 0x03));  // ADVERT | route
    frame[f++] = PATH_LEN_INIT;  // path_len: размер хэша и 0 хопов

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
    uint8_t plaintext[GROUP_TEXT_MAX_PLAIN];
    // ts — Unix-время в СЕКУНДАХ (как у adverts и личных сообщений): epoch-ms не
    // помещается в uint32. Пока часы не выставлены — millis.
    uint32_t now = (uint32_t)time(NULL);
    uint32_t ts = (now > 1000000000) ? now : (uint32_t)millis();
    memcpy(plaintext, &ts, 4);                      // timestamp (LE)
    plaintext[4] = 0;                               // TXT_TYPE_PLAIN
    size_t plen = 5;
    // "имя: " собирается в рантайме: имя приходит из настроек, а не из макроса
    char prefix[48];
    int pn = snprintf(prefix, sizeof(prefix), "%s: ", cfg.name.c_str());
    if (pn < 0) pn = 0;
    if (pn > (int)sizeof(prefix) - 1) pn = (int)sizeof(prefix) - 1;
    size_t n = min((size_t)pn, sizeof(plaintext) - plen);
    memcpy(plaintext + plen, prefix, n); plen += n;
    n = min((size_t)msg.length(), sizeof(plaintext) - plen);
    memcpy(plaintext + plen, msg.c_str(), n); plen += n;

    return encryptGroupText(channels[chIdx].secret, enc, plaintext, plen);
}

int buildGroupFrameFlood(int chIdx, const String& msg, uint8_t* frame, int maxlen) {
    uint8_t enc[GROUP_TEXT_MAX_PLAIN + 2];
    int enclen = buildGroupEnc(chIdx, msg, enc);
    if (enclen <= 0 || 3 + enclen > maxlen) return 0;
    frame[0] = 0x15;                    // GRP_TXT | ROUTE_TYPE_FLOOD
    frame[1] = PATH_LEN_INIT;           // размер хэша, 0 хопов (путь достроят ретрансляторы)
    frame[2] = channels[chIdx].hash;
    memcpy(frame + 3, enc, enclen);
    return 3 + enclen;
}

int buildPrivateTextFrame(uint8_t dest_hash, const uint8_t* dest_pub,
                          const String& msg, uint8_t* frame, int maxlen) {
    uint8_t secret[32];
    ed25519_key_exchange(secret, dest_pub, bot_prv64);

    uint8_t data[DM_TEXT_MAX + 8];
    int dlen = 0;
    uint32_t ts = (uint32_t)time(NULL);   // Unix-секунды (epoch-ms не лезет в uint32)
    memcpy(data, &ts, 4); dlen += 4;
    data[dlen++] = 0;                        // attempt = 0
    size_t ml = min((size_t)DM_TEXT_MAX, (size_t)msg.length());
    memcpy(data + dlen, msg.c_str(), ml); dlen += ml;
    data[dlen++] = 0;                        // null terminator

    uint8_t enc[DM_TEXT_MAX + 24];           // MAC 2 + шифр, дополненный до кратного 16
    int enclen = encryptGroupText(secret, enc, data, dlen);   // [MAC 2B][cipher]
    if (enclen <= 0 || 4 + enclen > maxlen) return 0;

    int f = 0;
    frame[f++] = 0x09;                       // TXT_MSG | ROUTE_TYPE_FLOOD
    frame[f++] = PATH_LEN_INIT;              // размер хэша, 0 хопов
    frame[f++] = dest_hash;
    frame[f++] = ownShortHash;
    memcpy(frame + f, enc, enclen); f += enclen;
    return f;
}

int sendFrame(int chIdx, const uint8_t* frame, int f) {
    if (chIdx < 0 || chIdx >= numChannels) return RADIOLIB_ERR_UNKNOWN;
    // hex-лог кадра стоит ~40 мс на UART для 245-байтного кадра — в fast-режиме молчим
    if (!otaFastMode) {
        for (int i = 0; i < f; i++) Serial.printf("%02X", frame[i]);
        Serial.println();
    }
    return txFrame((uint8_t*)frame, f);
}

void floodSend(int chIdx, const uint8_t* frame, int f, unsigned int gapMs, int repeats) {
    for (int i = 0; i < repeats; i++) {
        if (chIdx >= 0) sendFrame(chIdx, frame, f);
        else            txFrame((uint8_t*)frame, f);
        if (i < repeats - 1) delay(gapMs);
    }
}

void sensorSendMsg(const char* msg, unsigned int gapMs, int repeats) {
    if (sensorChannelIdx < 0) {
        Serial.printf("[SNS] sensor channel not configured, cannot send \"%s\"\n", msg);
        return;
    }
    uint8_t frame[256];
    int f = buildGroupFrameFlood(sensorChannelIdx, msg, frame, sizeof(frame));
    if (f <= 0) return;
    floodSend(-1, frame, f, gapMs, repeats);
    Serial.printf("[SNS] sent \"%s\" to sensor channel\n", msg);
    #ifdef COMPANION_NODE
    // Собственные передачи в приложение иначе не попадают: в очередь кладётся только
    // принятое из эфира, а свой же флуд отбрасывается как эхо. Кладём прямо здесь, с тем
    // же префиксом имени, с каким сообщение ушло в эфир.
    companionOnChannelText(sensorChannelIdx, cfg.name + ": " + msg, 0.0f, PATH_LEN_INIT, false);
    #endif
    #ifdef SENSOR_NODE
    sensorLastSent = msg;
    sensorLastSentMs = millis();
    #endif
}

// "time:<epoch>:<версия бота>" — сенсор выставляет часы и сверяет свою версию с ботом
#ifdef SENSOR_NODE
// hello:<версия>:<заряд %>:<напряжение>:<код платы> — бот публикует эти поля в MQTT.
// Без измерения батареи вместо значений идёт "-", чтобы позиция кода платы не съезжала.
void sensorSendHello() {
    // Пятым полем идёт окружение сборки: код платы больше не определяет прошивку —
    // сенсор и компаньон живут на одной h43, а образы у них разные.
    char msg[96];
    #if HAS_BATTERY
    if (batteryPresent()) {
        char bv[12];
        snprintf(msg, sizeof(msg), "%s:%s:%d:%s:%s:%s", SENSOR_MSG_HELLO, FW_VERSION,
                 batteryPercent(), fmtFix(batteryVoltage(), 2, bv, sizeof(bv)),
                 BOARD_CODE, FW_ENV);
    } else {
        snprintf(msg, sizeof(msg), "%s:%s:-:-:%s:%s", SENSOR_MSG_HELLO, FW_VERSION,
                 BOARD_CODE, FW_ENV);
    }
    #else
    snprintf(msg, sizeof(msg), "%s:%s:-:-:%s:%s", SENSOR_MSG_HELLO, FW_VERSION,
             BOARD_CODE, FW_ENV);
    #endif
    sensorSendMsg(msg);
}
#endif

#ifdef SENSOR_NODE
// Эхо-запрос: одиночная посылка, чтобы измерять время одного обмена, а не повторов
void sensorPingSend() {
    if (sensorChannelIdx < 0) return;
    pingId = (uint16_t)millis();
    if (pingId == 0) pingId = 1;
    pingFailed = false;
    pingShowUntil = 0;
    char msg[24];
    snprintf(msg, sizeof(msg), "%s%u", SENSOR_MSG_PING, (unsigned)pingId);
    pingSentMs = millis();
    sensorSendMsg(msg, FLOOD_RETRY_MS, 1);
}

void sensorPingTick() {
    if (pingSentMs == 0) return;
    if (millis() - pingSentMs < PING_TIMEOUT_MS) return;
    pingSentMs = 0;
    pingFailed = true;
    pingShowUntil = millis() + PING_SHOW_MS;
    Serial.println("[PING] ответа нет");
}
#endif

void sendSensorTimeSync() {
    char msg[48];
    snprintf(msg, sizeof(msg), "time:%llu:%s", (unsigned long long)time(NULL), FW_VERSION);
    sensorSendMsg(msg);
}

String channelListStr() {
    String s = "";
    for (int i = 0; i < numChannels; i++) {
        if (i > 0) s += " + ";
        s += channels[i].name;
    }
    return s;
}
