#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RadioLib.h>
#include <SPI.h>
#include <mbedtls/md.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

// ===== ПИНЫ ДЛЯ HELTEC V4.3 =====
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET   21
#define SCREEN_ADDRESS 0x3C
#define SDA_PIN 17
#define SCL_PIN 18
#define VEXT_PIN 36
#define BUTTON_PIN 0

// ===== ПИНЫ LORA (SX1262) =====
#define LORA_CS   8
#define LORA_RST  12
#define LORA_DIO1 14
#define LORA_BUSY 13

// ===== ПИНЫ SPI =====
#define LORA_SCK  9
#define LORA_MISO 11
#define LORA_MOSI 10

// ===== ПИНЫ FEM (УСИЛИТЕЛЬ KCT8103L) ДЛЯ V4.3 =====
#define FEM_VCC_PIN 7
#define FEM_EN_PIN  2
#define FEM_TX_PIN  5

// ===== MESHCORE ПАРАМЕТРЫ =====
#define LORA_FREQ 868.731018
#define LORA_BW   62.5
#define LORA_SF   8
#define LORA_CR   7
#define LORA_SYNC_WORD 0x12
#define LORA_TX_POWER 10
#define LORA_PREAMBLE 16

// Имя устройства (идёт в заголовке исходящих сообщений)
#define DEVICE_NAME "Tr1glav_esp_bot"

// Часовой пояс для отображения локального времени (из BUILD_UNIX_TIME)
#define LOCAL_TZ "Europe/Moscow"

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);

// ===== КАНАЛЫ =====
// #public использует FIXED PSK (base64 "izOH6cXN6mrJ5e26oRXNcg==" -> 16 байт).
// #connections создан приложением с автоключом: SHA256("#connections")[0:16].
// В обоих случаях секрет в эфире: 32 байта = 16 ключа + 16 нулей,
// хэш канала = SHA256(ключ16)[0], AES по первым 16 байтам, HMAC по 32.
struct MeshChannel {
    uint8_t secret[32];
    uint8_t hash;
    const char* name;
};
#define MAX_CHANNELS 2
MeshChannel channels[MAX_CHANNELS];
int numChannels = 0;

// ===== ПЕРЕМЕННЫЕ =====
bool isListening = false;
int packetCount = 0;
String lastMessage = "";
String lastSender = "";
String lastChannelName = "#public";
char lastPath[100] = "";
float lastRSSI = 0;
float lastSNR = 0;
bool buttonPressed = false;
unsigned long lastDisplayUpdate = 0;
unsigned long lastRxDisplay = 0;

// ===== СТРАЖ ЗДОРОВЬЯ LORA =====
// Как в Dispatcher: периодически сбрасываем AGC (warm sleep -> startReceive),
// иначе SX1262 со временем глохнет и перестаёт ловить пакеты ("засыпает").
#define RADIO_REARM_INTERVAL_MS 10000
unsigned long lastReArmMs = 0;

// Warm sleep сбрасывает аналоговый фронтенд (AGC/LNA), затем заново в RX.
void rearmRadioAGC() {
    radio.sleep();
    radio.startReceive();
    lastReArmMs = millis();
    Serial.println("[RADIO] AGC reset, RX re-armed");
}

// ===== ДЕДУПЛИКАЦИЯ (как SimpleMeshTables в MeshCore) =====
// Хэш считается по payload_type + payload (без header/transport/path),
// поэтому копии одного сообщения, пришедшие разными путями, совпадают.
#define SEEN_HASH_SIZE 8
#define SEEN_HASH_COUNT 64
uint8_t seen_hashes[SEEN_HASH_COUNT * SEEN_HASH_SIZE];
int seen_next_idx = 0;
uint32_t duplicateCount = 0;

// Возвращает true, если пакет уже видели, и помечает его как виденный.
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

    // ищем в кольцевом буфере
    for (int i = 0; i < SEEN_HASH_COUNT; i++) {
        if (memcmp(hash_ctx, &seen_hashes[i * SEEN_HASH_SIZE], SEEN_HASH_SIZE) == 0) {
            return true;
        }
    }
    // помечаем
    memcpy(&seen_hashes[seen_next_idx * SEEN_HASH_SIZE], hash_ctx, SEEN_HASH_SIZE);
    seen_next_idx = (seen_next_idx + 1) % SEEN_HASH_COUNT;
    return false;
}

// ===== ВЫЧИСЛЕНИЕ КЛЮЧЕЙ КАНАЛОВ =====
// Устанавливает канал: secret32 = key16 + 16 нулей, hash = SHA256(key16)[0].
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
}

// ===== ШИФРОВАНИЕ GRP_TXT (encrypt-then-MAC как в MeshCore) =====
// src_len >= 1. dest: [MAC 2B][ciphertext]; возвращает 2 + padded len.
int encryptGroupText(const uint8_t* secret32, uint8_t* dest, const uint8_t* src, int src_len) {
    if (src_len <= 0) return 0;
    int padded = (src_len + 15) & ~15;
    uint8_t* cipher = dest + 2;  // MAC спереди

    // AES-128-ECB по блокам, хвост добиваем нулями
    uint8_t block[16];
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, secret32, 128);
    for (int i = 0; i < padded; i += 16) {
        memset(block, 0, 16);
        memcpy(block, src + i, min(16, src_len - i));
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, block, cipher + i);
    }
    mbedtls_aes_free(&aes);

    // HMAC-SHA256(cipher) с полным 32-байтным секретом
    uint8_t hmac_out[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    secret32, 32, cipher, padded, hmac_out);
    dest[0] = hmac_out[0];
    dest[1] = hmac_out[1];
    return 2 + padded;
}

// ===== РАСШИФРОВКА GRP_TXT (MAC-then-decrypt как в MeshCore) =====
String decryptGroupText(const uint8_t* secret32, uint8_t* mac, uint8_t* ciphertext, int len) {
    if (len <= 0 || len % 16 != 0) return "";

    // Проверяем HMAC-SHA256(ciphertext) с полным 32-байтным секретом
    uint8_t hmac_out[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    secret32, 32,
                    ciphertext, len, hmac_out);
    if (hmac_out[0] != mac[0] || hmac_out[1] != mac[1]) return "";

    // AES-128-ECB расшифровка
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, secret32, 128);

    uint8_t plaintext[256];
    for (int i = 0; i < len; i += 16) {
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, ciphertext + i, plaintext + i);
    }
    mbedtls_aes_free(&aes);

    // plaintext: [timestamp 4B][txt_type 1B][text...]
    String message = "";
    for (int i = 5; i < len; i++) {
        if (plaintext[i] == 0) break;
        message += (char)plaintext[i];
    }
    return message;
}

// ===== ОТВЕТ НА /ping =====
bool sendGroupMessageOnChannel(int chIdx, const String& msg);  // fwd decl
// Формат ответа как в bot.py: "hops:direct" либо "hops:N, route:aa → bb → cc"
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

// ===== ПАРСИНГ MESHCORE ПАКЕТА =====
bool parseMeshCorePacket(uint8_t* data, int len) {
    if (len < 6) return false;
    
    uint8_t header = data[0];
    uint8_t payload_type = (header >> 2) & 0x0F;
    uint8_t route_type = header & 0x03;
    
    if (payload_type != 0x05) return false;  // только GRP_TXT
    
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
    offset += hop_count * path_hash_size;
    
    if (offset >= len) return false;
    uint8_t channel_hash = data[offset++];
    if (offset + 2 > len) return false;
    
    // ищем канал по хэшу
    int chIdx = -1;
    for (int i = 0; i < numChannels; i++) {
        if (channel_hash == channels[i].hash) { chIdx = i; break; }
    }
    if (chIdx < 0) return false;
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
    
    packetCount++;
    lastRSSI = radio.getRSSI();
    lastSNR = radio.getSNR();
    
    int colonPos = message.indexOf(": ");
    if (colonPos > 0) {
        lastSender = message.substring(0, colonPos);
        lastMessage = message.substring(colonPos + 2);
    } else {
        lastSender = "?";
        lastMessage = message;
    }

    // не обрабатываем собственные сообщения (эхо собственного флуда)
    if (lastSender == DEVICE_NAME) return false;
    
    Serial.printf("\n=== PACKET #%d (%s) ===\n", packetCount, lastChannelName.c_str());
    Serial.printf("From: %s\n", lastSender.c_str());
    Serial.printf("Msg: %s\n", lastMessage.c_str());
    Serial.printf("Route: %s (hops=%u)\n", lastPath[0] ? lastPath : "direct", hop_count);
    Serial.printf("RSSI: %.1f dBm, SNR: %.1f dB\n", lastRSSI, lastSNR);
    
    lastRxDisplay = millis();
    
    // Показать сообщение на экране
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

    // === ОТВЕТ НА /ping В КАНАЛЕ #connections ===
    if (chIdx == 1 && lastMessage == "/ping") {
        char reply[100];
        buildPingReply(reply, sizeof(reply), path_bytes, hop_count, path_hash_size);
        Serial.printf("[PING] reply: %s\n", reply);
        String full = String(reply);
        sendGroupMessageOnChannel(chIdx, full);
    }
    
    return true;
}

// ===== ОТПРАВКА GRP_TXT =====
// Формирует сообщение "DEVICE_NAME: msg" и шлёт флудом (header 0x15, без transport codes).
bool sendGroupMessageOnChannel(int chIdx, const String& msg) {
    if (chIdx < 0 || chIdx >= numChannels) return false;
    MeshChannel& ch = channels[chIdx];

    uint8_t plaintext[256];
    int plen = 0;
    uint32_t ts = (uint32_t)millis();
    memcpy(plaintext, &ts, 4); plen += 4;           // timestamp (LE)
    plaintext[plen++] = 0;                          // TXT_TYPE_PLAIN
    const char* prefix = DEVICE_NAME ": ";
    memcpy(plaintext + plen, prefix, strlen(prefix)); plen += strlen(prefix);
    size_t mlen = min((size_t)200, msg.length());
    memcpy(plaintext + plen, msg.c_str(), mlen); plen += mlen;

    uint8_t enc[256];
    int enclen = encryptGroupText(ch.secret, enc, plaintext, plen);
    if (enclen <= 0) return false;

    uint8_t frame[300];
    int f = 0;
    frame[f++] = 0x15;        // GRP_TXT | ROUTE_TYPE_FLOOD
    frame[f++] = 0x00;        // path_len: hash_size=1, 0 хопов (построится ретрансляторами)
    frame[f++] = ch.hash;     // channel hash
    memcpy(frame + f, enc, enclen); f += enclen;

    Serial.printf("\n[TX] %s: %s (%dB)\n", ch.name, (DEVICE_NAME ": " + msg).c_str(), f);
    for (int i = 0; i < f; i++) Serial.printf("%02X", frame[i]);
    Serial.println();

    // FEM на передачу
    digitalWrite(FEM_TX_PIN, HIGH);
    int st = radio.transmit(frame, f);
    digitalWrite(FEM_TX_PIN, LOW);

    if (st == RADIOLIB_ERR_NONE) {
        Serial.println("[TX] OK");
    } else {
        Serial.printf("[TX] FAILED %d\n", st);
    }

    // после TX обязательно вернуться в RX; при ошибке — полный ре-арм AGC
    if (radio.startReceive() != RADIOLIB_ERR_NONE) {
        rearmRadioAGC();
    }
    if (st == RADIOLIB_ERR_NONE) {
        return true;
    }
    return false;
}

// ===== ИНИЦИАЛИЗАЦИЯ LORA =====
bool initLoRa() {
    Serial.println("Init LoRa...");
    
    // Вызов begin() БЕЗ параметра TCXO.
    // RadioLib возьмёт значение из макроса SX126X_DIO3_TCXO_VOLTAGE,
    // который мы определим в platformio.ini.
    int state = radio.begin(
        LORA_FREQ, LORA_BW, LORA_SF, LORA_CR,
        LORA_SYNC_WORD, LORA_TX_POWER, LORA_PREAMBLE, 1.8
    );
    
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println("LoRa OK (TCXO from macro)");
        radio.setCRC(true);

        // SX1262 настройки для Heltec V4 (аналог MeshCore std_init)
        radio.setDio2AsRfSwitch(true);
        radio.setRxBoostedGainMode(true);
        radio.setCurrentLimit(140);
        // патч регистра 0x8B5 для улучшенного приёма на Heltec v4
        uint8_t r_data = 0;
        radio.readRegister(0x8B5, &r_data, 1);
        r_data |= 0x01;
        radio.writeRegister(0x8B5, &r_data, 1);
        return true;
    }
    
    Serial.printf("LoRa FAILED: %d\n", state);
    return false;
}

// Компилятором задавалось BUILD_UNIX_TIME из времени хоста (см. scripts/gen_build_time.py)
#ifndef BUILD_UNIX_TIME
#define BUILD_UNIX_TIME 0
#endif

char sysTimeStr[32] = "?";

// Устанавливает системные часы ESP32-S3 в момент старта из времени хоста
// в момент сборки/прошивки. NTP не используется.
void initSystemClock() {
    struct timeval tv;
    tv.tv_sec = BUILD_UNIX_TIME;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);

    setenv("TZ", LOCAL_TZ, 1);
    tzset();

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(sysTimeStr, sizeof(sysTimeStr), "%Y-%m-%d %H:%M:%S", &tm_now);
    Serial.printf("[RTC] SysTime set from host: %s (%s)\n", sysTimeStr, LOCAL_TZ);
    Serial.printf("[RTC] epoch=%lld\n", (long long)now);
}

void setup() {
    Serial.begin(115200);
    delay(3000);
    Serial.println("\n=== MESHCORE LISTENER V4.3 ===\n");
    
    initSystemClock();
    
    // ===== ПИТАНИЕ OLED =====
    pinMode(VEXT_PIN, OUTPUT);
    digitalWrite(VEXT_PIN, LOW);
    delay(300);
    
    // ===== FEM =====
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
    
    // ===== OLED =====
    pinMode(OLED_RESET, OUTPUT);
    digitalWrite(OLED_RESET, LOW);
    delay(10);
    digitalWrite(OLED_RESET, HIGH);
    delay(200);

    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setClock(400000);

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
    
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    
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
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("LoRa ERROR!");
        display.display();
        while (1) {
            delay(1000);
            Serial.println("LoRa init FAILED");
        }
    }
    
    deriveChannels();
    radio.startReceive();
    isListening = true;
    
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("MeshCore");
    display.println("#public + #connections");
    display.println("Listening...");
    display.print(sysTimeStr);
    display.display();
    
    Serial.println("Listening on #public + #connections...\n");
}

void loop() {
    // ===== КНОПКА =====
    if (digitalRead(BUTTON_PIN) == LOW) {
        if (!buttonPressed) {
            buttonPressed = true;
            isListening = !isListening;
            if (isListening) radio.startReceive();
            else radio.standby();
            
            display.clearDisplay();
            display.setCursor(0, 0);
            display.println("MeshCore");
            display.println(isListening ? "Listening..." : "STOPPED");
            display.printf("Pkts: %d Dups: %lu\n", packetCount, duplicateCount);
            display.display();
            delay(300);
        }
    } else {
        buttonPressed = false;
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
                    for (int i = 0; i < min(pktLen, 24); i++) Serial.printf("%02X", buffer[i]);
                    Serial.println();
                }
                if (checkAndMarkSeen(buffer, pktLen)) {
                    duplicateCount++;
                    Serial.printf("[DUP] skipped (total dups=%lu)\n", duplicateCount);
                } else {
                    bool parsed = parseMeshCorePacket(buffer, pktLen);

                    // hex-экран только для GRP_TXT, который не расшифровался
                    // (рекламные/служебные пакеты экран не трогаем)
                    if (pktLen > 0 && !parsed && ((buffer[0] >> 2) & 0x0F) == 0x05) {
                        display.setTextSize(1);
                        display.clearDisplay();
                        display.setCursor(0, 0);
                        display.printf("RX %dB RSSI:%.0f\n", pktLen, rssi);
                        display.printf("SNR:%.0f pkts:%d\n", snr, packetCount);
                        display.printf("hex:");
                        for (int i = 0; i < min(pktLen, 21); i++) display.printf("%02X", buffer[i]);
                        display.display();
                        lastRxDisplay = millis();
                    }

                    // не перезатираем экран 5 сек после сообщения
                    if (parsed) lastRxDisplay = millis();
                }
                radio.startReceive();
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
    if (isListening && (millis() - lastDisplayUpdate > 500)) {
        lastDisplayUpdate = millis();
        if (millis() - lastRxDisplay > 5000) {
            display.clearDisplay();
            display.setTextSize(1);
            display.setCursor(0, 0);
            display.println("MeshCore listen");
            display.println("Listening...");
            // часы из системного времени (обновляются каждые 500 мс вместе с экраном)
            time_t now = time(NULL);
            struct tm tm_now;
            localtime_r(&now, &tm_now);
            char tbuf[32];
            strftime(tbuf, sizeof(tbuf), "%H:%M:%S  %d.%m", &tm_now);
            display.println(tbuf);
            display.printf("Pkts: %d\n", packetCount);
            if (lastMessage.length() > 0) {
                display.printf("Last: %s\n", lastMessage.substring(0, 20).c_str());
            }
            display.display();
        }
    }
    
    delay(10);
}